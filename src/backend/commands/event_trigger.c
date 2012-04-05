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
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_language.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/dbcommands.h"
#include "commands/event_trigger.h"
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

/*
 * Cache the event triggers in a format that's suitable to finding which
 * function to call at "hook" points in the code. The catalogs are not helpful
 * at search time, because we can't both edit a single catalog entry per each
 * command, have a user friendly syntax and find what we need in a single index
 * scan.
 *
 * This cache is indexed by Event Command id (see pg_event_trigger.h) then
 * Event Id. It's containing a list of function oid.
 *
 * We're wasting some memory here, but that's local and in the kB range... so
 * the easier code makes up fot it big time.
 */
static void BuildEventTriggerCache(bool force_rebuild);

List *
EventCommandTriggerCache[EVTG_MAX_TRIG_EVENT_COMMAND][EVTG_MAX_TRIG_EVENT];

bool event_trigger_cache_is_stalled = true;

static void check_event_trigger_name(const char *trigname, Relation tgrel);

/*
 * Check permission: command triggers are only available for superusers. Raise
 * an exception when requirements are not fullfilled.
 *
 * It's not clear how to accept that database owners be able to create command
 * triggers, a superuser could run a command that fires a trigger's procedure
 * written by the database owner and now running with superuser privileges.
 */
static void
CheckEventTriggerPrivileges()
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
InsertEventTriggerTuple(Relation tgrel, char *trigname, TrigEvent event,
						Oid funcoid, char evttype, List *cmdlist)
{
	Oid         trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	ObjectAddress myself, referenced;
	ArrayType  *tagArray;

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(event);
	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(trigname);
	values[Anum_pg_event_trigger_evtfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_event_trigger_evttype - 1] = CharGetDatum(evttype);
	values[Anum_pg_event_trigger_evtenabled - 1] = CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);

	if (cmdlist == NIL)
		tagArray = NULL;
	else
	{
		ListCell   *lc;
		Datum	   *tags;
		int			i = 0, l = list_length(cmdlist);

		tags = (Datum *) palloc(l * sizeof(Datum));

		foreach(lc, cmdlist)
		{
			tags[i++] = Int16GetDatum(lfirst_int(lc));
		}
		tagArray = construct_array(tags, l, INT2OID, 2, true, 's');
	}
	values[Anum_pg_event_trigger_evttags - 1] = PointerGetDatum(tagArray);

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
	myself.classId = EventTriggerRelationId;
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
CreateEventTrigger(CreateEventTrigStmt *stmt, const char *queryString)
{
	Relation	tgrel;
	Oid			funcoid, trigoid;
	Oid			funcrettype;

	CheckEventTriggerPrivileges();

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
	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/*
	 * Scan pg_event_trigger for existing triggers on command. We do this only
	 * to give a nice error message if there's already a trigger of the same
	 * name. (The unique index on evtname would complain anyway.)
	 *
	 * NOTE that this is cool only because we have AccessExclusiveLock on
	 * the relation, so the trigger set won't be changing underneath us.
	 */
	check_event_trigger_name(stmt->trigname, tgrel);

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
	/*
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
	*/

	if (funcrettype != CMDTRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"command_trigger\"",
						NameListToString(stmt->funcname))));

	/* Specifically drop support for event triggers on event triggers */
/*
	switch (nodeTag(stmt))
	{
		case T_RenameStmt:
			if (((RenameStmt *) stmt)->renameType == OBJECT_EVENT_TRIGGER)
				return;

		case T_DropStmt:
			if (((DropStmt *) stmt)->removeType == OBJECT_EVENT_TRIGGER)
				return;

		case T_IndexStmt:
			if (((IndexStmt *)stmt)->concurrent)
				return;

		default:
			break;
	}
*/

	trigoid = InsertEventTriggerTuple(tgrel, stmt->trigname, stmt->event,
									  funcoid, stmt->timing, stmt->cmdlist);

	heap_close(tgrel, RowExclusiveLock);

	BuildEventTriggerCache(true);

	return trigoid;
}

/*
 * Guts of command trigger deletion.
 */
void
RemoveEventTriggerById(Oid trigOid)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/*
	 * Find the trigger to delete.
	 */
	ScanKeyInit(&skey[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(trigOid));

	tgscan = systable_beginscan(tgrel, EventTriggerOidIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "could not find tuple for event trigger %u", trigOid);

	/*
	 * Delete the pg_cmdtrigger tuple.
	 */
	simple_heap_delete(tgrel, &tup->t_self);

	systable_endscan(tgscan);
	heap_close(tgrel, RowExclusiveLock);

	BuildEventTriggerCache(true);
}

/*
 * ALTER EVENT TRIGGER foo ON COMMAND ... ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
void
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	char        tgenabled = pstrdup(stmt->tgenabled)[0]; /* works with gram.y */

	CheckEventTriggerPrivileges();

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);
	ScanKeyInit(&skey[0],
				Anum_pg_event_trigger_evtname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(stmt->trigname));

	tgscan = systable_beginscan(tgrel, EventTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("trigger \"%s\" does not exist",
						stmt->trigname)));

	/* Copy tuple so we can modify it below */
	tup = heap_copytuple(tup);
	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);

	systable_endscan(tgscan);

	evtForm->evtenabled = tgenabled;

	simple_heap_update(tgrel, &tup->t_self, tup);
	CatalogUpdateIndexes(tgrel, tup);

	heap_close(tgrel, RowExclusiveLock);
	heap_freetuple(tup);

	BuildEventTriggerCache(true);
}


/*
 * Rename command trigger
 */
void
RenameEventTrigger(List *name, const char *newname)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Relation	rel;
	Form_pg_event_trigger evtForm;
	char *trigname;

	Assert(list_length(name) == 1);
	trigname = strVal((Value *)linitial(name));

	CheckEventTriggerPrivileges();

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/* newname must be available */
	check_event_trigger_name(newname, rel);

	/* get existing tuple */
	ScanKeyInit(&skey[0],
				Anum_pg_event_trigger_evtname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(rel, EventTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("command trigger \"%s\" does not exist",
						trigname)));

	/* Copy tuple so we can modify it below */
	tup = heap_copytuple(tup);
	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);

	systable_endscan(tgscan);

	/* rename */
	namestrcpy(&(evtForm->evtname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);
	heap_close(rel, NoLock);

	/* Renaming impacts trigger calls orderding */
	BuildEventTriggerCache(true);
}

/*
 * get_event_trigger_oid - Look up an event trigger by name to find its OID.
 *
 * If missing_ok is false, throw an error if trigger not found.  If
 * true, just return InvalidOid.
 */
Oid
get_event_trigger_oid(const char *trigname, bool missing_ok)
{
	Relation	tgrel;
	ScanKeyData skey[1];
	SysScanDesc tgscan;
	HeapTuple	tup;
	Oid			oid;

	/*
	 * Find the trigger, verify permissions, set up object address
	 */
	tgrel = heap_open(EventTriggerRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_event_trigger_evtname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, EventTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tup = systable_getnext(tgscan);

	if (!HeapTupleIsValid(tup))
	{
		if (!missing_ok)
			ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("event trigger \"%s\" does not exist",
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
 * Scan pg_event_trigger for existing triggers on event. We do this only to
 * give a nice error message if there's already a trigger of the same name.
 */
void
check_event_trigger_name(const char *trigname, Relation tgrel)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tuple;

	ScanKeyInit(&skey[0],
				Anum_pg_event_trigger_evtname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(trigname));

	tgscan = systable_beginscan(tgrel, EventTriggerNameIndexId, true,
								SnapshotNow, 1, skey);

	tuple = systable_getnext(tgscan);

	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists", trigname)));
	systable_endscan(tgscan);
}

/*
 * Scan the pg_event_trigger catalogs and build the EventTriggerCache, which is
 * an array of commands indexing arrays of events containing the List of
 * function to call, in order.
 *
 * The idea is that the code to fetch the list of functions to process gets as
 * simple as the following:
 *
 *  foreach(cell, EventCommandTriggerCache[TrigEventCommand][TrigEvent])
 */
void
BuildEventTriggerCache(bool force_rebuild)
{
	Relation	rel, irel;
	IndexScanDesc indexScan;
	HeapTuple	tuple;

	if (event_trigger_cache_is_stalled || force_rebuild)
	{
		int i, j;
		for(i=1; i < EVTG_MAX_TRIG_EVENT_COMMAND; i++)
			for(j=1; j < EVTG_MAX_TRIG_EVENT; j++)
				EventCommandTriggerCache[i][j] = NIL;
	}

	rel = heap_open(EventTriggerRelationId, AccessShareLock);
	irel = index_open(EventTriggerNameIndexId, AccessShareLock);

	indexScan = index_beginscan(rel, irel, SnapshotNow, 0, 0);
	index_rescan(indexScan, NULL, 0, NULL, 0);

	/* we use a full indexscan to guarantee that we see event triggers ordered
	 * by name, this way we only even have to append the trigger's function Oid
	 * to the target cache Oid list.
	 */
	while (HeapTupleIsValid(tuple = index_getnext(indexScan, ForwardScanDirection)))
	{
		Form_pg_event_trigger form = (Form_pg_event_trigger) GETSTRUCT(tuple);
		Datum		adatum;
		bool		isNull;
		int			numkeys;
		TrigEvent event;
		TrigEventCommand command;

		/*
		 * First check if this trigger is enabled, taking into consideration
		 * session_replication_role.
		 */
		if (form->evtenabled == TRIGGER_DISABLED)
		{
			continue;
		}
		else if (SessionReplicationRole == SESSION_REPLICATION_ROLE_REPLICA)
		{
			if (form->evtenabled == TRIGGER_FIRES_ON_ORIGIN)
				continue;
		}
		else	/* ORIGIN or LOCAL role */
		{
			if (form->evtenabled == TRIGGER_FIRES_ON_REPLICA)
				continue;
		}

		event = form->evtevent;

		adatum = heap_getattr(tuple, Anum_pg_event_trigger_evttags,
							  RelationGetDescr(rel), &isNull);

		if (isNull)
		{
			/* we store triggers for all commands with command=0
			 *
			 * event triggers created without WHEN clause are targetting all
			 * columns
			 */
			command = 0;
			EventCommandTriggerCache[command][event] =
				lappend_oid(EventCommandTriggerCache[command][event],
							form->evtfoid);
		}
		else
		{
			ArrayType	*arr;
			int16		*tags;
			int			 i;

			arr = DatumGetArrayTypeP(adatum);		/* ensure not toasted */
			numkeys = ARR_DIMS(arr)[0];

			if (ARR_NDIM(arr) != 1 ||
				numkeys < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != INT2OID)
				elog(ERROR, "evttags is not a 1-D smallint array");

			tags = (int16 *) ARR_DATA_PTR(arr);

			for (i = 0; i < numkeys; i++)
			{
				 command = tags[i];

				 EventCommandTriggerCache[command][event] =
					 lappend_oid(EventCommandTriggerCache[command][event],
								 form->evtfoid);
			}
		}
	}
	index_endscan(indexScan);
	index_close(irel, AccessShareLock);
	heap_close(rel, AccessShareLock);

	event_trigger_cache_is_stalled = false;
}

/*
 * Functions to execute the command triggers.
 *
 * We call the functions that matches the command triggers definitions in
 * alphabetical order, and give them those arguments:
 *
 *   toplevel command tag, text
 *   command tag, text
 *   objectId, oid
 *   schemaname, text
 *   objectname, text
 *
 * Those are passed down as special "context" magic variables and need specific
 * support in each PL that wants to support command triggers. All core PL do.
 */

static void
call_event_trigger_procedure(EventContext ev_ctx, TrigEvent tev,
							 RegProcedure proc)
{
	FmgrInfo	flinfo;
	FunctionCallInfoData fcinfo;
	PgStat_FunctionCallUsage fcusage;
	EventTriggerData trigdata;

	fmgr_info(proc, &flinfo);

	/*
	 * Prepare the command trigger function context from the Command Context.
	 * We prepare a dedicated Node here so as not to publish internal data.
	 */
	trigdata.type		= T_EventTriggerData;
	trigdata.toplevel	= ev_ctx->toplevel;
	trigdata.tag		= ev_ctx->tag;
	trigdata.objectId	= ev_ctx->objectId;
	trigdata.schemaname = ev_ctx->schemaname;
	trigdata.objectname = ev_ctx->objectname;
	trigdata.parsetree	= ev_ctx->parsetree;

	/* rebuild the gram.y string from the TrigEvent enum value */
	switch (tev)
	{
		case E_CommandStart:
			trigdata.when = pstrdup("command_start");
			break;

		case E_CommandEnd:
			trigdata.when = pstrdup("command_end");
			break;

		case E_SecurityCheck:
			trigdata.when = pstrdup("security_check");
			break;

		case E_ConsistencyCheck:
			trigdata.when = pstrdup("consistency_check");
			break;

		case E_NameLookup:
			trigdata.when = pstrdup("name_lookup");
			break;

		case E_CreateCommand:
			trigdata.when = pstrdup("create");
			break;

		case E_AlterCommand:
			trigdata.when = pstrdup("alter");
			break;

		case E_RenameCommand:
			trigdata.when = pstrdup("rename");
			break;

		case E_AlterOwnerCommand:
			trigdata.when = pstrdup("alter_owner");
			break;

		case E_AlterSchemaCommand:
			trigdata.when = pstrdup("alter_schema");
			break;

		case E_DropCommand:
			trigdata.when = pstrdup("drop");
			break;
	}

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
 * Routine to call to setup a EventContextData evt.
 */
void
InitEventContext(EventContext evt, const Node *stmt)
{
	evt->command = -1;
	evt->toplevel = NULL;
	evt->tag = (char *) CreateCommandTag((Node *)stmt);
	evt->parsetree  = (Node *)stmt;
	evt->objectId   = InvalidOid;
	evt->objectname = NULL;
	evt->schemaname = NULL;
}

/*
 * InitEventContext() must have been called when CommandFiresTriggers() is
 * called. When CommandFiresTriggers() returns false, the EventContext
 * structure needs not be initialized further.
 *
 * When calling CommandFiresTriggers() the command field must already have been
 * set.
 */
bool
CommandFiresTriggers(EventContext ev_ctx)
{
	int j;

	Assert(ev_ctx->command != -1);

	/* Make sure we have initialized at least once the cache */
	BuildEventTriggerCache(false);

	for(j=1; j < EVTG_MAX_TRIG_EVENT; j++)
		if (EventCommandTriggerCache[ev_ctx->command][j] != NIL)
			/* this command fires at least a trigger */
			return true;

	return false;
}

/*
 * It's still interresting to avoid preparing the Command Context for AFTER
 * command triggers when we have none to Execute, so we provide this API too.
 */
bool
CommandFiresTriggersForEvent(EventContext ev_ctx, TrigEvent tev)
{
	BuildEventTriggerCache(false);
	return EventCommandTriggerCache[ev_ctx->command][tev] != NIL;
}

void
ExecEventTriggers(EventContext ev_ctx, TrigEvent tev)
{
	ListCell *lc;

	BuildEventTriggerCache(false);

	foreach(lc, EventCommandTriggerCache[ev_ctx->command][tev])
	{
		RegProcedure proc = (RegProcedure) lfirst_oid(lc);

		call_event_trigger_procedure(ev_ctx, tev, proc);
	}
	return;
}

/* COMPAT, TO REMOVE */
bool
CommandFiresAfterTriggers(EventContext ev_ctx)
{
	return CommandFiresTriggersForEvent(ev_ctx, E_CommandEnd);
}

void
ExecBeforeCommandTriggers(EventContext ev_ctx)
{
	ExecEventTriggers(ev_ctx, E_CommandStart);
}

void
ExecAfterCommandTriggers(EventContext ev_ctx)
{
	ExecEventTriggers(ev_ctx, E_CommandEnd);
}
