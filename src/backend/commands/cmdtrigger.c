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
static RegProcedure *list_triggers_for_command(const char *command, char type);
static RegProcedure *list_all_triggers_for_command(const char *command, char type);

static bool ExecBeforeCommandTriggers(CommandContext cmd,
										  MemoryContext per_command_context,
										  RegProcedure *procs);
static bool ExecInsteadOfCommandTriggers(CommandContext cmd,
											 MemoryContext per_command_context,
											 RegProcedure *procs);

/*
 * Check permission: command triggers are only available for superusers and
 * database owner.  Raise an exception when requirements are not fullfilled.
 */
static void
CheckCmdTriggerPrivileges()
{
	if (!superuser())
		if (!pg_database_ownercheck(MyDatabaseId, GetUserId()))
			aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_DATABASE,
						   get_database_name(MyDatabaseId));
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
	/* cmd trigger args: cmd_tag, objectId, schemaname, objectname [,parsetree] */
	Oid			fargtypes[4] = {TEXTOID, OIDOID, TEXTOID, TEXTOID};
	Oid			fargtypes_c[5] = {TEXTOID, OIDOID, TEXTOID, TEXTOID, INTERNALOID};
	Oid			funcoid;
	Oid			funcrettype;
	char        ctgtype;

	CheckCmdTriggerPrivileges();

	/*
	 * Find and validate the trigger function. When the function is coded in C
	 * it receives an internal argument which is the parse tree as a Node *.
	 *
	 * Only C coded functions can accept an argument of type internal, so we
	 * don't have to explicitely check about the prolang here.
	 */
	funcoid = LookupFuncName(stmt->funcname, 5, fargtypes_c, true);
	if (funcoid == InvalidOid)
		funcoid = LookupFuncName(stmt->funcname, 4, fargtypes, false);

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
		 * Scan pg_cmdtrigger for existing triggers on command. We do this only
		 * to give a nice error message if there's already a trigger of the
		 * same name. (The unique index on ctgcommand/ctgname would complain
		 * anyway.)
		 *
		 * NOTE that this is cool only because we have AccessExclusiveLock on
		 * the relation, so the trigger set won't be changing underneath us.
		 */
		check_cmdtrigger_name(command, stmt->trigname, tgrel);

		switch (stmt->timing)
		{
			case TRIGGER_TYPE_BEFORE:
			{
				RegProcedure *procs = list_all_triggers_for_command(command, CMD_TRIGGER_FIRED_INSTEAD);
				ctgtype = CMD_TRIGGER_FIRED_BEFORE;
				if (procs[0] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("\"%s\" already has INSTEAD OF triggers", command),
							 errdetail("Commands cannot have both BEFORE and INSTEAD OF triggers.")));
				break;
			}

			case TRIGGER_TYPE_INSTEAD:
			{
				RegProcedure *before = list_all_triggers_for_command(command, CMD_TRIGGER_FIRED_BEFORE);
				RegProcedure *after;
				ctgtype = CMD_TRIGGER_FIRED_INSTEAD;
				if (before[0] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("\"%s\" already has BEFORE triggers", command),
							 errdetail("Commands cannot have both BEFORE and INSTEAD OF triggers.")));

				after = list_all_triggers_for_command(command, CMD_TRIGGER_FIRED_AFTER);
				if (after[0] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("\"%s\" already has AFTER triggers", command),
							 errdetail("Commands cannot have both AFTER and INSTEAD OF triggers.")));
				break;
			}

			case TRIGGER_TYPE_AFTER:
			{
				RegProcedure *procs = list_all_triggers_for_command(command, CMD_TRIGGER_FIRED_INSTEAD);
				ctgtype = CMD_TRIGGER_FIRED_AFTER;
				if (procs[0] != InvalidOid)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("\"%s\" already has INSTEAD OF triggers", command),
							 errdetail("Commands cannot have both AFTER and INSTEAD OF triggers.")));
				break;
			}

			default:
			{
				elog(ERROR, "unknown trigger type for COMMAND TRIGGER");
				return;	/* make compiler happy */
			}
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

		trigoid = InsertCmdTriggerTuple(tgrel, command, stmt->trigname, funcoid, ctgtype);
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
static RegProcedure *
list_all_triggers_for_command(const char *command, char type)
{
	RegProcedure *procs = list_triggers_for_command(command, type);
	RegProcedure *anyp  = list_triggers_for_command("ANY", type);

	/* add the ANY trigger at the last position */
	if (anyp != InvalidOid)
	{
		/* list_triggers_for_command ensure procs has at least one free slot */
		int i;
		for (i=0; procs[i] != InvalidOid; i++);
		procs[i++] = anyp[0];
		procs[i] = InvalidOid;
	}
	return procs;
}

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
			/* ensure at least a free slot at the end of the array */
			if ((count+1) == size)
			{
				size += 10;
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
call_cmdtrigger_procedure(CommandContext cmd,
						  RegProcedure proc,
						  MemoryContext per_command_context)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	Datum		result;
	HeapTuple	procedureTuple;
	Form_pg_proc procedureStruct;
	int         nargs = 4;

	fmgr_info_cxt(proc, &flinfo, per_command_context);

	/* we need the procedure's language here to know how many args to call it
	 * with
	 */
	procedureTuple = SearchSysCache1(PROCOID, ObjectIdGetDatum(proc));
	if (!HeapTupleIsValid(procedureTuple))
		elog(ERROR, "cache lookup failed for function %u", proc);
	procedureStruct = (Form_pg_proc) GETSTRUCT(procedureTuple);

	if (procedureStruct->prolang == ClanguageId)
		nargs = 5;

	ReleaseSysCache(procedureTuple);

	/* Can't use OidFunctionCallN because we might get a NULL result */
	InitFunctionCallInfoData(fcinfo, &flinfo, nargs, InvalidOid, NULL, NULL);

	/* We support triggers ON ANY COMMAND so all fields here are nullable. */
	if (cmd->tag != NULL)
		fcinfo.arg[0] = PointerGetDatum(cstring_to_text(pstrdup(cmd->tag)));

	fcinfo.arg[1] = ObjectIdGetDatum(cmd->objectId);

	if (cmd->schemaname != NULL)
		fcinfo.arg[2] = PointerGetDatum(cstring_to_text(pstrdup(cmd->schemaname)));

	if (cmd->objectname != NULL)
		fcinfo.arg[3] = PointerGetDatum(cstring_to_text(pstrdup(cmd->objectname)));

	fcinfo.argnull[0] = cmd->tag == NULL;
	fcinfo.argnull[1] = cmd->objectId == InvalidOid;
	fcinfo.argnull[2] = cmd->schemaname == NULL;
	fcinfo.argnull[3] = cmd->objectname == NULL;

	if (nargs == 5)
	{
		fcinfo.arg[4] = PointerGetDatum(cmd->parsetree);
		fcinfo.argnull[4] = false;
	}

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
 * execution. ExecBeforeCommandTriggers() returns true when execution should
 * stop here because an Instead OF trigger has been run.
 */
bool
ExecBeforeOrInsteadOfCommandTriggers(CommandContext cmd)
{
	bool stop = false;
	MemoryContext per_command_context;
	RegProcedure *before =
		list_triggers_for_command(cmd->tag, CMD_TRIGGER_FIRED_BEFORE);
	RegProcedure *instead =
		list_triggers_for_command(cmd->tag, CMD_TRIGGER_FIRED_INSTEAD);

	per_command_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "BeforeOrInsteadOfTriggerCommandContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	if (before[0] != InvalidOid)
		stop = ExecBeforeCommandTriggers(cmd, per_command_context, before);

	else if (instead[0] != InvalidOid)
		stop = ExecInsteadOfCommandTriggers(cmd, per_command_context, instead);

	/* Release working resources */
	MemoryContextDelete(per_command_context);
	return stop;
}

/*
 * Same as ExecBeforeCommandTriggers() except that we apply the ANY command
 * trigger rather than the triggers for the command_context->tag command.
 */
bool
ExecBeforeOrInsteadOfAnyCommandTriggers(CommandContext cmd)
{
	bool stop = false;
	MemoryContext per_command_context;
	RegProcedure *before =
		list_triggers_for_command("ANY", CMD_TRIGGER_FIRED_BEFORE);
	RegProcedure *instead =
		list_triggers_for_command("ANY", CMD_TRIGGER_FIRED_INSTEAD);

	per_command_context =
		AllocSetContextCreate(CurrentMemoryContext,
							  "BeforeOrInsteadOfTriggerCommandContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	if (before[0] != InvalidOid)
		stop = ExecBeforeCommandTriggers(cmd, per_command_context, before);

	else if (instead[0] != InvalidOid)
		stop = ExecInsteadOfCommandTriggers(cmd, per_command_context, instead);

	/* Release working resources */
	MemoryContextDelete(per_command_context);
	return stop;
}

/*
 * A BEFORE command trigger can choose to "abort" the command by returning
 * false. This function is called by ExecBeforeOrInsteadOfCommandTriggers() so
 * is not exposed to other modules.
 */
static bool
ExecBeforeCommandTriggers(CommandContext cmd,
						  MemoryContext per_command_context,
						  RegProcedure *procs)
{
	MemoryContext oldContext;
	RegProcedure proc;
	int cur = 0;
	bool cont = true;

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	oldContext = MemoryContextSwitchTo(per_command_context);
	MemoryContextReset(per_command_context);

	while (cont && InvalidOid != (proc = procs[cur++]))
		cont = call_cmdtrigger_procedure(cmd, proc, per_command_context);

	MemoryContextSwitchTo(oldContext);

	/* return true when we want to stop executing this command */
	return !cont;
}

/*
 * An INSTEAD OF command trigger will always cancel execution of the command,
 * we only need to know that at least one of them got fired. This function is
 * called by ExecBeforeOrInsteadOfCommandTriggers() so is not exposed to other
 * modules.
 */
static bool
ExecInsteadOfCommandTriggers(CommandContext cmd,
							 MemoryContext per_command_context,
							 RegProcedure *procs)
{
	MemoryContext oldContext;
	RegProcedure proc;
	int cur = 0;

	/*
	 * Do the functions evaluation in a per-command memory context, so that
	 * leaked memory will be reclaimed once per command.
	 */
	oldContext = MemoryContextSwitchTo(per_command_context);
	MemoryContextReset(per_command_context);

	while (InvalidOid != (proc = procs[cur++]))
		call_cmdtrigger_procedure(cmd, proc, per_command_context);

	MemoryContextSwitchTo(oldContext);

	/* return true when we want to stop executing this command */
	return cur > 1;
}

/*
 * An AFTER trigger will have no impact on the command, which already was
 * executed.
 */
static void
ExecAfterCommandTriggersInternal(CommandContext cmd, RegProcedure *procs)
{
	MemoryContext oldContext, per_command_context;
	RegProcedure proc;
	int cur = 0;

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
		call_cmdtrigger_procedure(cmd, proc, per_command_context);

	/* Release working resources */
	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(per_command_context);

	return;
}

void
ExecAfterCommandTriggers(CommandContext cmd)
{
	RegProcedure *after =
		list_triggers_for_command(cmd->tag, CMD_TRIGGER_FIRED_AFTER);

	if (after[0] != InvalidOid)
		ExecAfterCommandTriggersInternal(cmd, after);
}

void
ExecAfterAnyCommandTriggers(CommandContext cmd)
{
	RegProcedure *after =
		list_triggers_for_command("ANY", CMD_TRIGGER_FIRED_AFTER);

	if (after[0] != InvalidOid)
		ExecAfterCommandTriggersInternal(cmd, after);
}
