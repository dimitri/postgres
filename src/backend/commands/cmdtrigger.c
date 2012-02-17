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

static void check_cmdtrigger_name(const char *command, const char *trigname, Relation tgrel);

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
void
CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString)
{
	Relation	tgrel;
	ListCell   *c;
	/* cmd trigger args: when, cmd_tag, objectId, schemaname, objectname [,parsetree] */
	Oid			fargtypes[5] = {TEXTOID, TEXTOID, OIDOID, TEXTOID, TEXTOID};
	Oid			fargtypes_c[6] = {TEXTOID, TEXTOID, OIDOID, TEXTOID, TEXTOID, INTERNALOID};
	Oid			funcoid;
	Oid			funcrettype;

	CheckCmdTriggerPrivileges();

	/*
	 * Find and validate the trigger function. When the function is coded in C
	 * it receives an internal argument which is the parse tree as a Node *.
	 *
	 * Only C coded functions can accept an argument of type internal, so we
	 * don't have to explicitely check about the prolang here.
	 */
	funcoid = LookupFuncName(stmt->funcname, 6, fargtypes_c, true);
	if (funcoid == InvalidOid)
		funcoid = LookupFuncName(stmt->funcname, 5, fargtypes, false);

	/* we need the trigger type to validate the return type */
	funcrettype = get_func_rettype(funcoid);

	/*
	 * Generate the trigger's OID now, so that we can use it in the name if
	 * needed.
	 */
	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);

	foreach(c, stmt->command)
	{
		Oid trigoid;
		A_Const *con = (A_Const *) lfirst(c);
		char    *command = strVal(&con->val);

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
			&& (strcmp(command, "VACUUM") == 0))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("AFTER VACUUM command triggers are not implemented")));

		if (stmt->timing == CMD_TRIGGER_FIRED_AFTER
			&& (strcmp(command, "CREATE INDEX") == 0))
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("CREATE INDEX CONCURRENTLY is not supported"),
					 errdetail("The command trigger will not get fired.")));

		/*
		 * Scan pg_cmdtrigger for existing triggers on command. We do this only
		 * to give a nice error message if there's already a trigger of the
		 * same name. (The unique index on ctgcommand/ctgname would complain
		 * anyway.)
		 *
		 * NOTE that this is cool only because we have AccessExclusiveLock on
		 * the relation, so the trigger set won't be changing underneath us.
		 */
		check_cmdtrigger_name(command, stmt->trigname, tgrel);

		if (funcrettype != VOIDOID)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
					 errmsg("function \"%s\" must return type \"void\"",
							NameListToString(stmt->funcname))));

		trigoid = InsertCmdTriggerTuple(tgrel, command, stmt->trigname, funcoid, stmt->timing);
	}
	heap_close(tgrel, RowExclusiveLock);
}

/*
 * DropTrigger - drop an individual trigger by name
 */
void
DropCmdTrigger(DropCmdTrigStmt *stmt)
{
	ListCell   *c;

	CheckCmdTriggerPrivileges();

	foreach(c, stmt->command)
	{
		ObjectAddress object;
		A_Const *con = (A_Const *) lfirst(c);
		char    *command = strVal(&con->val);

		object.classId = CmdTriggerRelationId;
		object.objectId = get_cmdtrigger_oid(command, stmt->trigname,
											 stmt->missing_ok);
		object.objectSubId = 0;

		if (!OidIsValid(object.objectId))
		{
			ereport(NOTICE,
					(errmsg("trigger \"%s\" for command \"%s\" does not exist, skipping",
							stmt->trigname, command)));
			break;
		}

		/*
		 * Do the deletion
		 */
		performDeletion(&object, stmt->behavior, 0);
	}
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

	CheckCmdTriggerPrivileges();

	tgrel = heap_open(CmdTriggerRelationId, RowExclusiveLock);
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

	heap_close(tgrel, RowExclusiveLock);
	heap_freetuple(tup);
}


/*
 * Rename command trigger
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

	CheckCmdTriggerPrivileges();

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
 *   objectId, oid
 *   schemaname, text
 *   objectname, text
 *
 */

/*
 * Scan the catalogs and fill in the CommandContext procedures that we will
 * have to call before and after the command.
 */
bool
ListCommandTriggers(CommandContext cmd)
{
	int         count = 0;
	Relation	rel, irel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	cmd->before = cmd->after = NIL;

	rel = heap_open(CmdTriggerRelationId, AccessShareLock);
	irel = index_open(CmdTriggerCommandNameIndexId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_cmdtrigger_ctgcommand,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(cmd->tag));

	scandesc = systable_beginscan_ordered(rel, irel, SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext_ordered(scandesc, ForwardScanDirection)))
	{
		Form_pg_cmdtrigger form = (Form_pg_cmdtrigger) GETSTRUCT(tuple);

        /*
		 * Replica support for command triggers is still on the TODO
		 */
		if (form->ctgenabled != 'D')
		{
			switch (form->ctgtype)
			{
				case CMD_TRIGGER_FIRED_BEFORE:
					cmd->before = lappend_oid(cmd->before, form->ctgfoid);
					break;

				case CMD_TRIGGER_FIRED_AFTER:
					cmd->after = lappend_oid(cmd->after, form->ctgfoid);
					break;
			}
			count++;
		}
	}
	systable_endscan_ordered(scandesc);

	index_close(irel, AccessShareLock);
	heap_close(rel, AccessShareLock);

	return count > 0;
}

static bool
call_cmdtrigger_procedure(CommandContext cmd,
						  RegProcedure proc,
						  const char *when,
						  MemoryContext per_command_context)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	int         nargs = 5;

	fmgr_info_cxt(proc, &flinfo, per_command_context);

	/* we need the procedure's language here to know how many args to call it
	 * with
	 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", proc);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	if (procedureStruct->prolang == ClanguageId)
		nargs = 6;

	ReleaseSysCache(procedureTuple);

	/* Can't use OidFunctionCallN because we might get a NULL result */
	InitFunctionCallInfoData(fcinfo, &flinfo, nargs, InvalidOid, NULL, NULL);

	fcinfo.arg[0] = PointerGetDatum(cstring_to_text(pstrdup(when)));

	/* We support triggers ON ANY COMMAND so all fields here are nullable. */
	if (cmd->tag != NULL)
		fcinfo.arg[1] = PointerGetDatum(cstring_to_text(pstrdup(cmd->tag)));

	fcinfo.arg[2] = ObjectIdGetDatum(cmd->objectId);

	if (cmd->schemaname != NULL)
		fcinfo.arg[3] = PointerGetDatum(cstring_to_text(pstrdup(cmd->schemaname)));

	if (cmd->objectname != NULL)
		fcinfo.arg[4] = PointerGetDatum(cstring_to_text(pstrdup(cmd->objectname)));

	fcinfo.argnull[0] = false;
	fcinfo.argnull[1] = cmd->tag == NULL;
	fcinfo.argnull[2] = cmd->objectId == InvalidOid;
	fcinfo.argnull[3] = cmd->schemaname == NULL;
	fcinfo.argnull[4] = cmd->objectname == NULL;

	if (nargs == 6)
	{
		fcinfo.arg[5] = PointerGetDatum(cmd->parsetree);
		fcinfo.argnull[5] = false;
	}

	pgstat_init_function_usage(&fcinfo, &fcusage);

	result = FunctionCallInvoke(&fcinfo);

	pgstat_end_function_usage(&fcusage, true);


	if (!fcinfo.isnull && DatumGetBool(result) == false)
		return false;
	return true;
}

/*
 * A BEFORE command trigger can choose to "abort" the command by returning
 * false. This function is called by ExecBeforeOrInsteadOfCommandTriggers() so
 * is not exposed to other modules.
 */
static void
exec_command_triggers_internal(CommandContext cmd, List *procs, const char *when)
{
	MemoryContext per_command_context, oldContext;
	ListCell   *cell;

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	per_command_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "CommandTriggerContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	oldContext = MemoryContextSwitchTo(per_command_context);
	MemoryContextReset(per_command_context);

	foreach(cell, procs)
	{
		Oid proc = lfirst_oid(cell);
		call_cmdtrigger_procedure(cmd, (RegProcedure)proc, when, per_command_context);
	}
	MemoryContextSwitchTo(oldContext);
}

/*
 * Routine to call to setup a CommandContextData structure.
 *
 * This ensures that cmd->before and cmd->after are set to meaningful values,
 * always NIL when list_triggers is false.
 *
 * In case of ANY trigger init we don't want to list triggers associated with
 * the real command tag, we have another API to do that, see
 * ExecBeforeAnyCommandTriggers() and ExecAfterAnyCommandTriggers().
 */
void
InitCommandContext(CommandContext cmd, const Node *stmt, bool list_any_triggers)
{
	cmd->tag = (char *) CreateCommandTag((Node *)stmt);
	cmd->parsetree  = (Node *)stmt;
	cmd->objectId   = InvalidOid;
	cmd->objectname = NULL;
	cmd->schemaname = NULL;
	cmd->before     = NIL;
	cmd->after      = NIL;

	if (list_any_triggers)
	{
		/* list procedures for "ANY" command */
		char *tag = cmd->tag;

		cmd->tag = "ANY";
		ListCommandTriggers(cmd);
		cmd->tag = tag;
	}
	else
		ListCommandTriggers(cmd);
}

/*
 * InitCommandContext() must have been called when CommandFiresTriggers() is
 * called. When CommandFiresTriggers() returns false, cmd structure needs not
 * be initialized further.
 *
 * There's no place where we can skip BEFORE command trigger initialization
 * when we have an AFTER command triggers to run, because objectname and
 * schemaname are needed in both places, so we check both here.
 */
bool
CommandFiresTriggers(CommandContext cmd)
{
	return cmd != NULL && (cmd->before != NIL || cmd->after != NIL);
}

/*
 * It's still interresting to avoid preparing the Command Context for AFTER
 * command triggers when we have none to Execute, so we provide this API too.
 */
bool
CommandFiresAfterTriggers(CommandContext cmd)
{
	return cmd != NULL && cmd->after != NIL;
}

/*
 * In the various Exec...CommandTriggers functions, we still protect against
 * and empty procedure list so as not to create a MemoryContext then switch to
 * it unnecessarily.
 */
void
ExecBeforeCommandTriggers(CommandContext cmd)
{
	if (cmd != NULL && cmd->before != NIL)
		exec_command_triggers_internal(cmd, cmd->before, "BEFORE");
}

void
ExecAfterCommandTriggers(CommandContext cmd)
{
	if (cmd != NULL && cmd->after != NIL)
		exec_command_triggers_internal(cmd, cmd->after, "AFTER");
}
