/*-------------------------------------------------------------------------
 *
 * cmdtrigger.c
 *	  PostgreSQL COMMAND TRIGGER support code.
 *
 * Portions Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/cmdtrigger.c
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
#include "pgstat.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"

static void check_cmdtrigger_name(const char *command, const char *trigname, Relation tgrel);
static RegProcedure * list_triggers_for_command(const char *command, char type);

/*
 * Create a trigger.  Returns the OID of the created trigger.
 */
Oid
CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString)
{
	Relation	tgrel;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	/* cmd trigger args: cmd_string, cmd_nodestring, schemaname, objectname */
	Oid			fargtypes[5] = {TEXTOID, TEXTOID, TEXTOID, TEXTOID, TEXTOID};
	Oid			funcoid;
	Oid			funcrettype;
	Oid			trigoid;
	char        ctgtype;
	ObjectAddress myself,
				referenced;

	/*
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 5, fargtypes, false);
	funcrettype = get_func_rettype(funcoid);

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
	check_cmdtrigger_name(stmt->command, stmt->trigname, tgrel);

	switch (stmt->timing)
	{
		case TRIGGER_TYPE_BEFORE:
		{
	        RegProcedure *procs = list_triggers_for_command(stmt->command, CMD_TRIGGER_FIRED_INSTEAD);
		    ctgtype = CMD_TRIGGER_FIRED_BEFORE;
			if (procs[0] != InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" already has INSTEAD OF triggers",
								stmt->command),
						 errdetail("Commands cannot have both BEFORE and INSTEAD OF triggers.")));
            break;
		}

		case TRIGGER_TYPE_INSTEAD:
		{
	        RegProcedure *before = list_triggers_for_command(stmt->command, CMD_TRIGGER_FIRED_BEFORE);
	        RegProcedure *after;
		    ctgtype = CMD_TRIGGER_FIRED_INSTEAD;
			if (before[0] != InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" already has BEFORE triggers",
								stmt->command),
						 errdetail("Commands cannot have both BEFORE and INSTEAD OF triggers.")));

			after = list_triggers_for_command(stmt->command, CMD_TRIGGER_FIRED_AFTER);
			if (after[0] != InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" already has AFTER triggers",
								stmt->command),
						 errdetail("Commands cannot have both AFTER and INSTEAD OF triggers.")));
            break;
		}

		case TRIGGER_TYPE_AFTER:
		{
	        RegProcedure *procs = list_triggers_for_command(stmt->command, CMD_TRIGGER_FIRED_INSTEAD);
		    ctgtype = CMD_TRIGGER_FIRED_AFTER;
			if (procs[0] != InvalidOid)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("\"%s\" already has INSTEAD OF triggers",
								stmt->command),
						 errdetail("Commands cannot have both AFTER and INSTEAD OF triggers.")));
            break;
		}
		default:
			elog(ERROR, "unknown trigger type for COMMAND TRIGGER");
	}

	if (ctgtype == CMD_TRIGGER_FIRED_BEFORE && funcrettype != BOOLOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"boolean\"",
						NameListToString(stmt->funcname))));

	if (ctgtype != CMD_TRIGGER_FIRED_BEFORE && funcrettype != VOIDOID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"void\"",
						NameListToString(stmt->funcname))));

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

	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	/* remember oid for record dependencies */
	trigoid = HeapTupleGetOid(tuple);

	heap_freetuple(tuple);
	heap_close(tgrel, RowExclusiveLock);

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 */
	myself.classId = CmdTriggerRelationId;
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
	object.objectId = get_cmdtrigger_oid(stmt->command, stmt->trigname,
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
 * ALTER TRIGGER foo ON COMMAND ... ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
void
AlterCmdTrigger(AlterCmdTrigStmt *stmt)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[2];
	HeapTuple	tup;
	Form_pg_cmdtrigger cmdForm;
	char        tgenabled = pstrdup(stmt->tgenabled)[0]; /* works with gram.y */

	tgrel = heap_open(CmdTriggerRelationId, AccessShareLock);//FIXME: wrong lock level?
	//FIXME: need a row level lock here
	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->command));
	ScanKeyInit(&skey[1],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerCommandNameIndexId, true,
								SnapshotNow, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for command \"%s\" does not exist, skipping",
						stmt->trigname, stmt->command)));

	/* Copy tuple so we can modify it below */
	tup = heap_copytuple(tup);
	cmdForm = (Form_pg_cmdtrigger) GETSTRUCT(tup);

	systable_endscan(tgscan);

	cmdForm->ctgenabled = tgenabled;

	simple_heap_update(tgrel, &tup->t_self, tup);
	CatalogUpdateIndexes(tgrel, tup);

	heap_close(tgrel, AccessShareLock);
	heap_freetuple(tup);
}


/*
 * Rename conversion
 */
void
RenameCmdTrigger(List *name, const char *trigname, const char *newname)
{
	SysScanDesc tgscan;
	ScanKeyData skey[2];
	HeapTuple	tup;
	Relation	rel;
	Form_pg_cmdtrigger cmdForm;
	char       *command;

	Assert(list_length(name) == 1);
	command = strVal((Value *)linitial(name));

	rel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	//FIXME: need a row level lock here
	/* newname must be available */
	check_cmdtrigger_name(command, newname, rel);

	/* get existing tuple */
	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(command));
	ScanKeyInit(&skey[1],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(rel, CmdTriggerCommandNameIndexId, true,
								SnapshotNow, 2, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" for command \"%s\" does not exist, skipping",
						trigname, command)));

	/* Copy tuple so we can modify it below */
	tup = heap_copytuple(tup);
	cmdForm = (Form_pg_cmdtrigger) GETSTRUCT(tup);

	systable_endscan(tgscan);

	/* rename */
	namestrcpy(&(cmdForm->ctgname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);
	heap_close(rel, NoLock);
}

/*
 * get_cmdtrigger_oid - Look up a trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_cmdtrigger_oid(const char *command, const char *trigname, bool missing_ok)
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
 * Scan pg_cmdtrigger for existing triggers on command. We do this only to
 * give a nice error message if there's already a trigger of the same name.
 */
void
check_cmdtrigger_name(const char *command, const char *trigname, Relation tgrel)
{
	SysScanDesc tgscan;
	ScanKeyData skey[2];
	HeapTuple	tuple;

	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(command));
	ScanKeyInit(&skey[1],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerCommandNameIndexId, true,
								SnapshotNow, 2, skey);

	tuple = systable_getnext(tgscan);

	elog(DEBUG1, "check_cmdtrigger_name(%s, %s)", command, trigname);

	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("trigger \"%s\" for command \"%s\" already exists",
						trigname, command)));
	systable_endscan(tgscan);
}

/*
 * Functions to execute the command triggers.
 *
 * We call the functions that matches the command triggers definitions in
 * alphabetical order, and give them those arguments:
 *
 *   command tag, text
 *   command string, text
 *   command node string, text
 *   schemaname, text, can be null
 *   objectname, text
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
//FIXME: Return a List here.
//FIXME: add abort-after-first for CreateCmdTrigger?
static RegProcedure *
list_triggers_for_command(const char *command, char type)
{
	int  count = 0, size = 10;
	RegProcedure *procs = (RegProcedure *) palloc(size*sizeof(RegProcedure));

	Relation	rel, irel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	/* init the first entry of the procs array */
	procs[0] = InvalidOid;

	rel = heap_open(CmdTriggerRelationId, AccessShareLock);
	irel = index_open(CmdTriggerCommandNameIndexId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(command));

	scandesc = systable_beginscan_ordered(rel, irel, SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext_ordered(scandesc, ForwardScanDirection)))
	{
		Form_pg_cmdtrigger cmd = (Form_pg_cmdtrigger) GETSTRUCT(tuple);

        /*
		 * Replica support for command triggers is still on the TODO
		 */
		if (cmd->ctgenabled != 'D' && cmd->ctgtype == type)
		{
			if (count == size)
			{
				size += 10;//FIXME: WTF?
				procs = (Oid *)repalloc(procs, size);
			}
			procs[count++] = cmd->ctgfoid;
			procs[count] = InvalidOid;
		}
	}
	systable_endscan_ordered(scandesc);

	index_close(irel, AccessShareLock);
	heap_close(rel, AccessShareLock);

	return procs;
}

static bool
call_cmdtrigger_procedure(RegProcedure proc, CommandContext cmd,
						  MemoryContext per_command_context)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	Datum		result;

	fmgr_info_cxt(proc, &flinfo, per_command_context);

	/* Can't use OidFunctionCallN because we might get a NULL result */
	InitFunctionCallInfoData(fcinfo, &flinfo, 5, InvalidOid, NULL, NULL);

	fcinfo.arg[0] = PointerGetDatum(cstring_to_text(pstrdup(cmd->tag)));
	fcinfo.arg[1] = PointerGetDatum(cstring_to_text(pstrdup(cmd->cmdstr)));

	if (cmd->nodestr != NULL)
		fcinfo.arg[2] = PointerGetDatum(cstring_to_text(pstrdup(cmd->nodestr)));

	if (cmd->schemaname != NULL)
		fcinfo.arg[3] = PointerGetDatum(cstring_to_text(pstrdup(cmd->schemaname)));

	fcinfo.arg[4] = PointerGetDatum(cstring_to_text(pstrdup(cmd->objectname)));

	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = false;
	fcinfo.argnull[2] = cmd->nodestr == NULL;
	fcinfo.argnull[3] = cmd->schemaname == NULL;
	fcinfo.argnull[4] = false;

	pgstat_init_function_usage(&fcinfo, &fcusage);

	result = FunctionCallInvoke(&fcinfo);

	pgstat_end_function_usage(&fcusage, true);

	if (!fcinfo.isnull && DatumGetBool(result) == false)
		return false;
	return true;
}

/*
 * For any given command tag, you can have either Before and After triggers, or
 * Instead Of triggers, not both.
 *
 * Instead Of triggers have to run before the command and to cancel its
 * execution, hence this API where we return the number of InsteadOf trigger
 * procedures we fired.
 */
int
ExecBeforeOrInsteadOfCommandTriggers(Node *parsetree, const char *cmdtag)
{
	MemoryContext per_command_context;
	int nb = 0;

	per_command_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "BeforeOrInsteadOfTriggerCommandContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * You can't have both BEFORE and INSTEAD OF triggers registered on the
	 * same command, so this function is not checking about that and just going
	 * through an empty list in at least one of those cases.  The cost of doing
	 * it this lazy way is an index scan on pg_catalog.pg_cmdtrigger.
	 */
	if (!ExecBeforeCommandTriggers(parsetree, cmdtag, per_command_context))
		nb++;
	nb += ExecInsteadOfCommandTriggers(parsetree, cmdtag, per_command_context);

	/* Release working resources */
	MemoryContextDelete(per_command_context);
	return nb;
}

//FIXME: This looks like it should be static
//FIXME: why not return the number of calls here as well?
bool
ExecBeforeCommandTriggers(Node *parsetree, const char *cmdtag,
						  MemoryContext per_command_context)
{
	MemoryContext oldContext;
	CommandContextData cmd;
	RegProcedure *procs = list_triggers_for_command(cmdtag,
													CMD_TRIGGER_FIRED_BEFORE);
	RegProcedure proc;
	int cur= 0;
	bool cont = true;
	//FIXME: add assert for IsA(parsetree, parsetree)

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	oldContext = MemoryContextSwitchTo(per_command_context);
	MemoryContextReset(per_command_context);

	while (cont && InvalidOid != (proc = procs[cur++]))
	{
		if (cur==1)
		{
			cmd.tag = (char *)cmdtag;
			pg_get_cmddef(&cmd, parsetree);
		}
		cont = call_cmdtrigger_procedure(proc, &cmd, per_command_context);

		if (cont == false)
			elog(WARNING,
				 "command \"%s %s...\" was cancelled by procedure \"%s\"",
				 cmdtag, cmd.objectname, get_func_name(proc));
	}
	MemoryContextSwitchTo(oldContext);
	return cont;
}

/*
 * return the count of triggers we fired
 */
//FIXME: This looks like it should be static
int
ExecInsteadOfCommandTriggers(Node *parsetree, const char *cmdtag,
							 MemoryContext per_command_context)
{
	MemoryContext oldContext;
	CommandContextData cmd;
	RegProcedure *procs = list_triggers_for_command(cmdtag,
													CMD_TRIGGER_FIRED_INSTEAD);
	RegProcedure proc;
	int cur = 0;
	//FIXME: add assert for IsA(parsetree, parsetree)

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	oldContext = MemoryContextSwitchTo(per_command_context);
	MemoryContextReset(per_command_context);

	while (InvalidOid != (proc = procs[cur++]))
	{
		if (cur==1)
		{
			cmd.tag = (char *)cmdtag;
			pg_get_cmddef(&cmd, parsetree);
		}
		call_cmdtrigger_procedure(proc, &cmd, per_command_context);
	}

	MemoryContextSwitchTo(oldContext);
	return cur-1;
}

void
ExecAfterCommandTriggers(Node *parsetree, const char *cmdtag)
{
	MemoryContext oldContext, per_command_context;
	CommandContextData cmd;
	RegProcedure *procs = list_triggers_for_command(cmdtag,
													CMD_TRIGGER_FIRED_AFTER);
	RegProcedure proc;
	int cur = 0;

	//FIXME: add assert for IsA(parsetree, parsetree)

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	per_command_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "AfterTriggerCommandContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	oldContext = MemoryContextSwitchTo(per_command_context);

	while (InvalidOid != (proc = procs[cur++]))
	{
		if (cur==1)
		{
			cmd.tag = (char *)cmdtag;
			pg_get_cmddef(&cmd, parsetree);
		}
		call_cmdtrigger_procedure(proc, &cmd, per_command_context);
	}

	/* Release working resources */
	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(per_command_context);

	return;
}
