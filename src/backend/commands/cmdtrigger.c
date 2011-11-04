/*-------------------------------------------------------------------------
 *
 * trigger.c
 *	  PostgreSQL TRIGGERs support code.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_cmdtrigger.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "commands/cmdtrigger.h"
#include "commands/trigger.h"
#include "parser/parse_func.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/tqual.h"

/*
 * Create a trigger.  Returns the OID of the created trigger.
 */
Oid
CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData key;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	/* cmd trigger args: cmd_string, cmd_nodestring, schemaname, objectname */
	Oid			fargtypes[5] = {TEXTOID, TEXTOID, NAMEOID, NAMEOID, NULL};
	Oid			funcoid;
	Oid			funcrettype;
	Oid			trigoid;
	char        ctgtype;
	ObjectAddress myself,
				referenced;

	/*
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 4, fargtypes, false);
	funcrettype = get_func_rettype(funcoid);
	if (funcrettype != BOOLOID)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function %s must return type \"boolean\"",
						NameListToString(stmt->funcname))));
	}

	/*
	 * Generate the trigger's OID now, so that we can use it in the name if
	 * needed.
	 */
	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	/*
	 * Scan pg_cmdtrigger for existing triggers on command. We do this only to
	 * give a nice error message if there's already a trigger of the same name.
	 * (The unique index on ctgcommand/ctgname would complain anyway.)
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on
	 * the relation, so the trigger set won't be changing underneath us.
	 */
	ScanKeyInit(&key,
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->command));
	tgscan = systable_beginscan(tgrel, CmdTriggerCommandNameIndexId, true,
								SnapshotNow, 1, &key);
	while (HeapTupleIsValid(tuple = systable_getnext(tgscan)))
	{
		Form_pg_cmdtrigger pg_cmdtrigger = (Form_pg_cmdtrigger) GETSTRUCT(tuple);

		if (namestrcmp(&(pg_cmdtrigger->ctgname), stmt->trigname) == 0)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("trigger \"%s\" for command \"%s\" already exists",
							stmt->trigname, stmt->command)));
	}
	systable_endscan(tgscan);

	if (TRIGGER_FIRED_BEFORE(stmt->timing))
		ctgtype = CMD_TRIGGER_FIRED_BEFORE;
	else if (TRIGGER_FIRED_INSTEAD(stmt->timing))
		ctgtype = CMD_TRIGGER_FIRED_INSTEAD;
	else
	{
		Assert(TRIGGER_FIRED_AFTER(stmt->timing));
		ctgtype = CMD_TRIGGER_FIRED_AFTER;
	}

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_cmdtrigger_ctgcommand - 1] = NameGetDatum(stmt->command);
	values[Anum_pg_cmdtrigger_ctgname - 1] = NameGetDatum(stmt->trigname);
	values[Anum_pg_cmdtrigger_ctgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_cmdtrigger_ctgtype - 1] = CharGetDatum(ctgtype);
	values[Anum_pg_cmdtrigger_ctgenabled - 1] = CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);

	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);

	/* force tuple to have the desired OID */
	trigoid = HeapTupleGetOid(tuple);

	/*
	 * Insert tuple into pg_trigger.
	 */
	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	heap_freetuple(tuple);
	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 */
	myself.classId = TriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	return trigoid;
}

/*
 * DropTrigger - drop an individual trigger by name
 */
void
DropCmdTrigger(DropCmdTrigStmt *stmt)
{
	ObjectAddress object;

	object.classId = CmdTriggerRelationId;
	object.objectId = get_cmdtrigger_oid(stmt->trigname, stmt->command,
										 stmt->missing_ok);
	object.objectSubId = 0;

	if (!OidIsValid(object.objectId))
	{
		ereport(NOTICE,
		  (errmsg("trigger \"%s\" for command \"%s\" does not exist, skipping",
				  stmt->trigname, stmt->command)));
		return;
	}

	/*
	 * Do the deletion
	 */
	performDeletion(&object, stmt->behavior);
}

/*
 * Guts of command trigger deletion.
 */
void
RemoveCmdTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;

	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, CmdTriggerOidIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for command trigger %u", trigOid);

	/*
	 * Delete the pg_cmdtrigger tuple.
	 */
	simple_heap_delete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	heap_close(tgrel, RowExclusiveLock);
}

/*
 * get_cmdtrigger_oid - Look up a trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_cmdtrigger_oid(const char *trigname, const char *command, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[2];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_open(CmdTriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(command));
	ScanKeyInit(&skey[1],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerCommandNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("trigger \"%s\" for command \"%s\" does not exist, skipping",
							 trigname, command)));
		oid = InvalidOid;
	}
	else
	{
		oid = HeapTupleGetOid(tup);
	}

	systable_endscan(tgscan);
	heap_close(tgrel, AccessShareLock);
	return oid;
}


/*
 * Functions to execute the command triggers.
 *
 * We call the functions that matches the command triggers definitions in
 * alphabetical order, and give them those arguments:
 *
 *   command string, text
 *   command node string, text
 *   schemaname, name, can be null
 *   objectname, name
 *
 * we rebuild the DDL command we're about to execute from the parsetree.
 *
 * The queryString comes from untrusted places: it could be a multiple
 * queries string that has been passed through psql -c or otherwise in the
 * protocol, or something that comes from an EXECUTE evaluation in plpgsql.
 *
 * Also we need to be able to spit out a normalized (canonical?) SQL
 * command to ease DDL trigger code, and we even provide them with a
 * nodeToString() output.
 *
 */

/*
 * returning false in a before command trigger will cancel the execution of
 * subsequent triggers and of the command itself.
 */
bool
ExecBeforeCommandTriggers(Node *parsetree)
{
	char *command = pg_get_cmddef(parsetree);
	char *nodestr = nodeToString(parsetree);

	return true;
}

/*
 * return the count of triggers we fired
 */
int
ExecInsteadOfCommandTriggers(Node *parsetree)
{
	char *command = pg_get_cmddef(parsetree);
	char *nodestr = nodeToString(parsetree);

	return 0;
}

void
ExecAfterCommandTriggers(Node *parsetree)
{
	char *command = pg_get_cmddef(parsetree);
	char *nodestr = nodeToString(parsetree);

	return;
}
