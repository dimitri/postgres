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
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/cmdtrigger.h"
#include "commands/dbcommands.h"
#include "commands/trigger.h"
#include "parser/parse_func.h"
#include "pgstat.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "tcop/utility.h"

static void check_cmdtrigger_name(const char *trigname, Relation tgrel);

/*
 * Check permission: command triggers are only available for superusers. Raise
 * an exception when requirements are not fullfilled.
 *
 * It's not clear how to accept that database owners be able to create command
 * triggers, a superuser could run a command that fires a trigger's procedure
 * written by the database owner and now running with superuser privileges.
 */
static void
CheckCmdTriggerPrivileges()
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 (errmsg("must be superuser to use command triggers"))));
}

/*
 * Insert Command Trigger Tuple
 *
 * Insert the new pg_cmdtrigger row, and return the OID assigned to the new
 * row.
 */
static Oid
InsertCmdTriggerTuple(Relation tgrel,
					  char *command, char *trigname, Oid funcoid, char ctgtype)
{
	Oid         trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	ObjectAddress myself, referenced;

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_cmdtrigger_ctgcommand - 1] = NameGetDatum(command);
	values[Anum_pg_cmdtrigger_ctgname - 1] = NameGetDatum(trigname);
	values[Anum_pg_cmdtrigger_ctgfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_cmdtrigger_ctgtype - 1] = CharGetDatum(ctgtype);
	values[Anum_pg_cmdtrigger_ctgenabled - 1] = CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);

	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);

	simple_heap_insert(tgrel, tuple);

	CatalogUpdateIndexes(tgrel, tuple);

	/* remember oid for record dependencies */
	trigoid = HeapTupleGetOid(tuple);

	heap_freetuple(tuple);

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
 * Create a trigger.  Returns the OID of the created trigger.
 */
Oid
CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString)
{
	Relation	tgrel;
	Oid			funcoid, trigoid;
	Oid			funcrettype;

	CheckCmdTriggerPrivileges();

	/*
	 * Find and validate the trigger function. When the function is coded in C
	 * it receives an internal argument which is the parse tree as a Node *.
	 *
	 * Only C coded functions can accept an argument of type internal, so we
	 * don't have to explicitely check about the prolang here.
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, true);

	/* we need the trigger type to validate the return type */
	funcrettype = get_func_rettype(funcoid);

	/*
	 * Generate the trigger's OID now, so that we can use it in the name if
	 * needed.
	 */
	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	/*
	 * Scan pg_cmdtrigger for existing triggers on command. We do this only
	 * to give a nice error message if there's already a trigger of the
	 * same name. (The unique index on ctgcommand/ctgname would complain
	 * anyway.)
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on
	 * the relation, so the trigger set won't be changing underneath us.
	 */
	check_cmdtrigger_name(stmt->trigname, tgrel);

	/*
	 * Add some restrictions. We don't allow for AFTER command triggers on
	 * commands that do their own transaction management, such as VACUUM and
	 * CREATE INDEX CONCURRENTLY, because RAISE EXCEPTION at this point is
	 * meaningless, the work as already been commited.
	 *
	 * CREATE INDEX CONCURRENTLY has no specific command tag and can not be
	 * captured here, so we just document that not AFTER command trigger
	 * will get run.
	 */
	if (stmt->timing == CMD_TRIGGER_FIRED_AFTER
		&& (strcmp(stmt->command, "VACUUM") == 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AFTER VACUUM command triggers are not implemented")));

	if (stmt->timing == CMD_TRIGGER_FIRED_AFTER
		&& (strcmp(stmt->command, "CLUSTER") == 0))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AFTER CLUSTER command triggers are not implemented")));

	if (stmt->timing == CMD_TRIGGER_FIRED_AFTER
		&& (strcmp(stmt->command, "CREATE INDEX") == 0))
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("AFTER CREATE INDEX CONCURRENTLY triggers are not supported"),
				 errdetail("The command trigger will not fire on concurrently-created indexes.")));

	if (strcmp(stmt->command, "REINDEX") == 0)
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("REINDEX DATABASE triggers are not supported"),
				 errdetail("The command trigger will not fire on REINDEX DATABASE.")));

	if (funcrettype != CMDTRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"command_trigger\"",
						NameListToString(stmt->funcname))));

	trigoid = InsertCmdTriggerTuple(tgrel, stmt->command, stmt->trigname,
									funcoid, stmt->timing);

	heap_close(tgrel, RowExclusiveLock);

	return trigoid;
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
	ScanKeyData skey[1];
	HeapTuple	tup;
	Form_pg_cmdtrigger cmdForm;
	char        tgenabled = pstrdup(stmt->tgenabled)[0]; /* works with gram.y */

	CheckCmdTriggerPrivileges();

	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);
	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" does not exist, skipping",
						stmt->trigname)));

	/* Copy tuple so we can modify it below */
	tup = heap_copytuple(tup);
	cmdForm = (Form_pg_cmdtrigger) GETSTRUCT(tup);

	systable_endscan(tgscan);

	cmdForm->ctgenabled = tgenabled;

	simple_heap_update(tgrel, &tup->t_self, tup);
	CatalogUpdateIndexes(tgrel, tup);

	heap_close(tgrel, RowExclusiveLock);
	heap_freetuple(tup);
}


/*
 * Rename command trigger
 */
void
RenameCmdTrigger(List *name, const char *newname)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Relation	rel;
	Form_pg_cmdtrigger cmdForm;
	char *trigname;

	Assert(list_length(name) == 1);
	trigname = strVal((Value *)linitial(name));

	CheckCmdTriggerPrivileges();

	rel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	/* newname must be available */
	check_cmdtrigger_name(newname, rel);

	/* get existing tuple */
	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(rel, CmdTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("command trigger \"%s\" does not exist, skipping",
						trigname)));

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
get_cmdtrigger_oid(const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_open(CmdTriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("command trigger \"%s\" does not exist, skipping",
							 trigname)));
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
check_cmdtrigger_name(const char *trigname, Relation tgrel)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyInit(&skey[0],
				Anum_pg_cmdtrigger_ctgname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, CmdTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tuple = systable_getnext(tgscan);

	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("command trigger \"%s\" already exists", trigname)));
	systable_endscan(tgscan);
}

/*
 * Functions to execute the command triggers.
 *
 * We call the functions that matches the command triggers definitions in
 * alphabetical order, and give them those arguments:
 *
 *   command tag, text
 *   objectId, oid
 *   schemaname, text
 *   objectname, text
 *
 */

/*
 * Scan the catalogs and fill in the CommandContext procedures that we will
 * have to call before and after the command.
 */
static bool
ListCommandTriggers(CommandContext cmd, bool list_any_triggers)
{
	int         count = 0;
	Relation	rel, irel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	char *tag;

	if (list_any_triggers)
	{
		tag = "ANY";
		cmd->before_any = cmd->after_any = NIL;
	}
	else
	{
		tag = cmd->tag;
		cmd->before = cmd->after = NIL;
	}

	rel = heap_open(CmdTriggerRelationId, AccessShareLock);
	irel = index_open(CmdTriggerCommandNameIndexId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(tag));

	scandesc = systable_beginscan_ordered(rel, irel, SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext_ordered(scandesc, ForwardScanDirection)))
	{
		Form_pg_cmdtrigger form = (Form_pg_cmdtrigger) GETSTRUCT(tuple);

		if (form->ctgenabled == TRIGGER_DISABLED)
		{
			continue;
		}
		else if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
		{
			if (form->ctgenabled == TRIGGER_FIRES_ON_ORIGIN)
				continue;
		}
		else	/* ORIGIN or LOCAL role */
		{
			if (form->ctgenabled == TRIGGER_FIRES_ON_REPLICA)
				continue;
		}

		switch (form->ctgtype)
		{
			case CMD_TRIGGER_FIRED_BEFORE:
			{
				if (list_any_triggers)
					cmd->before_any = lappend_oid(cmd->before_any, form->ctgfoid);
				else
					cmd->before = lappend_oid(cmd->before, form->ctgfoid);
				break;
			}
			case CMD_TRIGGER_FIRED_AFTER:
			{
				if (list_any_triggers)
					cmd->after_any = lappend_oid(cmd->after_any, form->ctgfoid);
				else
					cmd->after = lappend_oid(cmd->after, form->ctgfoid);
				break;
			}
		}
		count++;
	}
	systable_endscan_ordered(scandesc);

	index_close(irel, AccessShareLock);
	heap_close(rel, AccessShareLock);

	return count > 0;
}

static void
call_cmdtrigger_procedure(CommandContext cmd, RegProcedure proc, const char *when)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	CommandTriggerData trigdata;

	fmgr_info(proc, &flinfo);

	/*
	 * Prepare the command trigger function context from the Command Context.
	 * We prepare a dedicated Node here so as not to publish internal data.
	 */
	trigdata.type		= T_CommandTriggerData;
	trigdata.when		= (char *)when;
	trigdata.tag		= cmd->tag;
	trigdata.objectId	= cmd->objectId;
	trigdata.schemaname = cmd->schemaname;
	trigdata.objectname = cmd->objectname;
	trigdata.parsetree	= cmd->parsetree;

	/*
	 * Call the function, passing no arguments but setting a context.
	 */
	InitFunctionCallInfoData(fcinfo, &flinfo, 0, InvalidOid,
							 (Node *) &trigdata, NULL);

	pgstat_init_function_usage(&fcinfo, &fcusage);
	FunctionCallInvoke(&fcinfo);
	pgstat_end_function_usage(&fcusage, true);

	return;
}

/*
 * Execute the procedures attached to the command. We pass the list of
 * procedures to use (either cmd->before or cmd->after) explicitely.
 *
 * The when argument allows to fill the trigger special variables.
 */
static void
exec_command_triggers_internal(CommandContext cmd, char when)
{
	List		*procs[2];
	char		*whenstr;
	ListCell	*cell;
	int i;

	switch (when)
	{
		case CMD_TRIGGER_FIRED_BEFORE:
			whenstr = "BEFORE";
			procs[0] = cmd->before_any;
			procs[1] = cmd->before;
			break;

		case CMD_TRIGGER_FIRED_AFTER:
			whenstr = "AFTER";
			procs[0] = cmd->after;
			procs[1] = cmd->after_any;
			break;

		default:
			elog(ERROR, "unrecognized command trigger condition: %c", when);
			break;
	}

	for(i=0; i<2; i++)
	{
		foreach(cell, procs[i])
		{
			Oid proc = lfirst_oid(cell);

			call_cmdtrigger_procedure(cmd, (RegProcedure)proc, whenstr);
		}
	}
}

/*
 * Routine to call to setup a CommandContextData structure.
 *
 * This ensures that cmd->before and cmd->after are set to meaningful values.
 */
void
InitCommandContext(CommandContext cmd, const Node *stmt)
{
	cmd->tag = (char *) CreateCommandTag((Node *)stmt);
	cmd->parsetree  = (Node *)stmt;
	cmd->objectId   = InvalidOid;
	cmd->objectname = NULL;
	cmd->schemaname = NULL;
	cmd->before     = NIL;
	cmd->after      = NIL;
	cmd->before_any = NIL;
	cmd->after_any  = NIL;
	cmd->oldmctx    = NULL;
	cmd->cmdmctx    = NULL;

	/* Specifically drop support for command triggers on command triggers */
	switch (nodeTag(stmt))
	{
		case T_RenameStmt:
			if (((RenameStmt *) stmt)->renameType == OBJECT_CMDTRIGGER)
				return;

		case T_DropStmt:
			if (((DropStmt *) stmt)->removeType == OBJECT_CMDTRIGGER)
				return;

		case T_IndexStmt:
			if (((IndexStmt *)stmt)->concurrent)
				return;

		default:
			ListCommandTriggers(cmd, true);   /* list ANY command triggers */
			ListCommandTriggers(cmd, false);  /* and triggers for this command tag */
	}
}

/*
 * InitCommandContext() must have been called when CommandFiresTriggers() is
 * called. When CommandFiresTriggers() returns false, cmd structure needs not
 * be initialized further.
 *
 * There's no place where we can skip BEFORE command trigger initialization
 * when we have an AFTER command triggers to run, because objectname and
 * schemaname are needed in both places, so we check both here.
 *
 * Integration is always on the form:
 *
 * if (CommandFiresTriggers(cmd)
 * {
 *      cmd->objectname = pstrdup(...);
 *      ...
 *
 *      ExecBeforeCommandTriggers(cmd);
 * }
 *
 * The same applies to after command triggers, so that we are able to switch
 * Memory contexts all from here.
 */
bool
CommandFiresTriggers(CommandContext cmd)
{
	if (cmd == NULL)
		return false;

	if (cmd->before != NIL || cmd->before_any != NIL
		 || cmd->after != NIL || cmd->after_any != NIL)
	{
		cmd->oldmctx = CurrentMemoryContext;
		cmd->cmdmctx =
			AllocSetContextCreate(CurrentMemoryContext,
								  "CommandTriggerContext",
								  ALLOCSET_DEFAULT_MINSIZE,
								  ALLOCSET_DEFAULT_INITSIZE,
								  ALLOCSET_DEFAULT_MAXSIZE);

		MemoryContextSwitchTo(cmd->cmdmctx);

		return true;
	}
	return false;
}

/*
 * It's still interresting to avoid preparing the Command Context for AFTER
 * command triggers when we have none to Execute, so we provide this API too.
 */
bool
CommandFiresAfterTriggers(CommandContext cmd)
{
	if (cmd == NULL)
		return false;

	if (cmd->before != NIL || cmd->before_any != NIL
		|| cmd->after != NIL || cmd->after_any != NIL)
	{
		MemoryContextSwitchTo(cmd->cmdmctx);
		return true;
	}
	return false;
}

/*
 * In the various Exec...CommandTriggers functions, we still protect against
 * and empty procedure list so as not to create a MemoryContext then switch to
 * it unnecessarily.
 */
void
ExecBeforeCommandTriggers(CommandContext cmd)
{
	if (cmd == NULL)
		return;

	/* that will execute under command trigger memory context */
	exec_command_triggers_internal(cmd, CMD_TRIGGER_FIRED_BEFORE);

	/* switch back to the command Memory Context now */
	MemoryContextSwitchTo(cmd->oldmctx);
}

void
ExecAfterCommandTriggers(CommandContext cmd)
{
	if (cmd == NULL)
		return;

	/* that will execute under command trigger memory context */
	exec_command_triggers_internal(cmd, CMD_TRIGGER_FIRED_AFTER);

	/* switch back to the command Memory Context now */
	MemoryContextSwitchTo(cmd->oldmctx);
}
