/*-------------------------------------------------------------------------
 *
 * event_trigger.c
 *	  PostgreSQL EVENT TRIGGER support code.
 *
 * Portions Copyright (c) 2011, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/commands/event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "access/xact.h"
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
 * local prototypes
 */
static void AlterEventTriggerOwner_internal(Relation rel,
											HeapTuple tup,
											Oid newOwnerId);

/*
 * Insert Command Trigger Tuple
 *
 * Insert the new pg_event_trigger row, and return the OID assigned to the new
 * row.
 */
static Oid
InsertEventTriggerTuple(char *trigname, TrigEvent event, Oid evtOwner,
						Oid funcoid, List *cmdlist)
{
	Relation tgrel;
	Oid         trigoid;
	HeapTuple	tuple;
	Datum		values[Natts_pg_trigger];
	bool		nulls[Natts_pg_trigger];
	ObjectAddress myself, referenced;
	ArrayType  *tagArray;
	char       *evtevent = event_to_string(event);

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/*
	 * Build the new pg_trigger tuple.
	 */
	memset(nulls, false, sizeof(nulls));

	values[Anum_pg_event_trigger_evtname - 1] = NameGetDatum(trigname);
	values[Anum_pg_event_trigger_evtevent - 1] = NameGetDatum(evtevent);
	values[Anum_pg_event_trigger_evtowner - 1] = ObjectIdGetDatum(evtOwner);
	values[Anum_pg_event_trigger_evtfoid - 1] = ObjectIdGetDatum(funcoid);
	values[Anum_pg_event_trigger_evtenabled - 1] = CharGetDatum(TRIGGER_FIRES_ON_ORIGIN);

	if (cmdlist == NIL)
		nulls[Anum_pg_event_trigger_evttags - 1] = true;
	else
	{
		ListCell   *lc;
		Datum	   *tags;
		int			i = 0, l = list_length(cmdlist);

		tags = (Datum *) palloc(l * sizeof(Datum));

		foreach(lc, cmdlist)
		{
			TrigEventCommand cmd = lfirst_int(lc);
			char *cmdstr = command_to_string(cmd);
			if (cmd == ETC_UNKNOWN || cmdstr == NULL)
				elog(ERROR, "Unknown command %d", cmd);
			tags[i++] = PointerGetDatum(cstring_to_text(cmdstr));
		}
		tagArray = construct_array(tags, l, TEXTOID, -1, false, 'i');

		values[Anum_pg_event_trigger_evttags - 1] = PointerGetDatum(tagArray);
	}

	tuple = heap_form_tuple(tgrel->rd_att, values, nulls);

	trigoid = simple_heap_insert(tgrel, tuple);
	CatalogUpdateIndexes(tgrel, tuple);

	heap_freetuple(tuple);

	/*
	 * Record dependencies for trigger.  Always place a normal dependency on
	 * the function.
	 */
	recordDependencyOnOwner(EventTriggerRelationId, trigoid, evtOwner);

	myself.classId = EventTriggerRelationId;
	myself.objectId = trigoid;
	myself.objectSubId = 0;

	referenced.classId = ProcedureRelationId;
	referenced.objectId = funcoid;
	referenced.objectSubId = 0;
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	heap_close(tgrel, RowExclusiveLock);

	return trigoid;
}

/*
 * Create a trigger.  Returns the OID of the created trigger.
 */
Oid
CreateEventTrigger(CreateEventTrigStmt *stmt, const char *queryString)
{
	HeapTuple	tuple;
	Oid			funcoid, trigoid;
	Oid			funcrettype;
	Oid			evtowner = GetUserId();

	/*
	 * It would be nice to allow database owners or even regular users to do
	 * this, but there are obvious privilege escalation risks which would have
	 * to somehow be plugged first.
	 */
	if (!superuser())
		ereport(ERROR,
			(errmsg("permission denied to create event trigger \"%s\"",
                   stmt->trigname),
            errhint("Must be superuser to create an event trigger.")));

	/*
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);

	/* we need the trigger type to validate the return type */
	funcrettype = get_func_rettype(funcoid);

	if (funcrettype != EVTTRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"event_trigger\"",
						NameListToString(stmt->funcname))));

	/*
	 * Give user a nice error message in case an event trigger of the same name
	 * already exists.
	 */
	tuple = SearchSysCache1(EVENTTRIGGERNAME, CStringGetDatum(stmt->trigname));
	if (HeapTupleIsValid(tuple))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists", stmt->trigname)));

	/* Insert the catalog entry */
	trigoid = InsertEventTriggerTuple(stmt->trigname, stmt->event,
									  evtowner, funcoid, stmt->cmdlist);

	return trigoid;
}

/*
 * Guts of event trigger deletion.
 */
void
RemoveEventTriggerById(Oid trigOid)
{
	Relation	tgrel;
	HeapTuple	tup;

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCache1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for event trigger %u", trigOid);

	simple_heap_delete(tgrel, &tup->t_self);

	ReleaseSysCache(tup);

	heap_close(tgrel, RowExclusiveLock);
}

/*
 * ALTER EVENT TRIGGER foo ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
void
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	char        tgenabled = stmt->tgenabled;

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME,
							  CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist",
					stmt->trigname)));
    if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
                       stmt->trigname);

	/* tuple is a copy, so we can modify it below */
	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);
	evtForm->evtenabled = tgenabled;

	simple_heap_update(tgrel, &tup->t_self, tup);
	CatalogUpdateIndexes(tgrel, tup);

	/* clean up */
	heap_freetuple(tup);
	heap_close(tgrel, RowExclusiveLock);
}


/*
 * Rename event trigger
 */
void
RenameEventTrigger(const char *trigname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Form_pg_event_trigger evtForm;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	/* newname must be available */
	if (SearchSysCacheExists1(EVENTTRIGGERNAME, CStringGetDatum(newname)))
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("event trigger \"%s\" already exists", newname)));

	/* trigname must exists */
	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
    if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
                       trigname);

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);

	/* tuple is a copy, so we can rename it now */
	namestrcpy(&(evtForm->evtname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);
}


/*
 * Change event trigger's owner -- by name
 */
void
AlterEventTriggerOwner(const char *name, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(name));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", name)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Change extension owner, by OID
 */
void
AlterEventTriggerOwner_oid(Oid trigOid, Oid newOwnerId)
{
	HeapTuple	tup;
	Relation	rel;

	rel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGEROID, ObjectIdGetDatum(trigOid));

	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
		  errmsg("event trigger with OID %u does not exist", trigOid)));

	AlterEventTriggerOwner_internal(rel, tup, newOwnerId);

	heap_freetuple(tup);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Internal workhorse for changing an event trigger's owner
 */
static void
AlterEventTriggerOwner_internal(Relation rel, HeapTuple tup, Oid newOwnerId)
{
	Form_pg_event_trigger form;

	form = (Form_pg_event_trigger) GETSTRUCT(tup);

	if (form->evtowner == newOwnerId)
		return;

    if (!pg_event_trigger_ownercheck(HeapTupleGetOid(tup), GetUserId()))
        aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EVENT_TRIGGER,
                       NameStr(form->evtname));

	/* New owner must be a superuser */
	if (!superuser_arg(newOwnerId))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to change owner of event trigger \"%s\"",
					NameStr(form->evtname)),
				 errhint("The owner of an event trigger must be a superuser.")));

	form->evtowner = newOwnerId;
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	/* Update owner dependency reference */
	changeDependencyOnOwner(EventTriggerRelationId,
							HeapTupleGetOid(tup),
							newOwnerId);
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
	Oid			oid;

	oid = GetSysCacheOid1(EVENTTRIGGERNAME, CStringGetDatum(trigname));
	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", trigname)));
	return oid;
}

/*
 * Functions to execute the event triggers.
 *
 * We call the functions that matches the event triggers definitions in
 * alphabetical order, and give them those arguments:
 *
 *   toplevel command tag, text
 *   command tag, text
 *   objectId, oid
 *   schemaname, text
 *   objectname, text
 *
 * Those are passed down as special "context" magic variables and need specific
 * support in each PL that wants to support event triggers. All core PL do.
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
	 * Prepare the event trigger function context from the Command Context.
	 * We prepare a dedicated Node here so as not to publish internal data.
	 */
	trigdata.type = T_EventTriggerData;
	trigdata.event = pstrdup(event_to_string(tev));
	trigdata.toplevel = ev_ctx->toplevel;
	trigdata.tag = ev_ctx->tag;
	trigdata.objectId = ev_ctx->objectId;
	trigdata.schemaname = ev_ctx->schemaname;
	trigdata.objectname = ev_ctx->objectname;
	trigdata.parsetree = ev_ctx->parsetree;

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
 *
 * The fields 'objecttype' must be set before calling other entry points. The
 * fields 'operation', 'objectId', 'objectname' and 'schemaname' might be set
 * to interesting values.
 */
void
InitEventContext(EventContext evt, const Node *parsetree)
{
	evt->command = ETC_UNSET;
	evt->toplevel = NULL;
	evt->tag = (char *) CreateCommandTag((Node *)parsetree);
	evt->parsetree = (Node *)parsetree;
	evt->operation = NULL;
	evt->objecttype = -1;
	evt->objectId = InvalidOid;
	evt->objectname = NULL;
	evt->schemaname = NULL;

	/* guess the ongoing operation from the command tag */
	if (strncmp(evt->tag, "CREATE ", 7) == 0)
		evt->operation = "CREATE";
	else if (strncmp(evt->tag, "DROP ", 5) == 0)
		evt->operation = "DROP";
	else if (strncmp(evt->tag, "ALTER ", 6) == 0)
		evt->operation = "ALTER";
}

/*
 * InitEventContext() must have been called first, then the event context field
 * 'objectype' must have been "manually" for command tags supporting several
 * kinds of object, such as T_DropStmt, T_RenameStmt, T_AlterObjectSchemaStmt,
 * T_AlterOwnerStmt or T_DefineStmt.
 *
 * When CommandFiresTriggersForEvent() returns false, the EventContext
 * structure needs not be initialized further.
 */
bool
CommandFiresTriggersForEvent(EventContext ev_ctx, TrigEvent tev)
{
	EventCommandTriggers *triggers;

	if (ev_ctx == NULL)
		return false;

	if (ev_ctx->command == ETC_UNSET)
		ev_ctx->command = get_command_from_nodetag(nodeTag(ev_ctx->parsetree),
												   ev_ctx->objecttype, true);

	if (ev_ctx->command == ETC_UNKNOWN)
		return false;

	triggers = get_event_triggers(tev, ev_ctx->command);

	return triggers->procs != NIL;
}

/*
 * Actually run event triggers for a specific command. We first run ANY
 * command triggers.
 */
void
ExecEventTriggers(EventContext ev_ctx, TrigEvent tev)
{
	EventCommandTriggers *triggers;
	ListCell *lc;

	if (ev_ctx == NULL)
		return;

	if (ev_ctx->command == ETC_UNSET)
		ev_ctx->command = get_command_from_nodetag(nodeTag(ev_ctx->parsetree),
												   ev_ctx->objecttype, true);

	if (ev_ctx->command == ETC_UNKNOWN)
		return;

	triggers = get_event_triggers(tev, ev_ctx->command);

	foreach(lc, triggers->procs)
	{
		RegProcedure proc = (RegProcedure) lfirst_oid(lc);

		call_event_trigger_procedure(ev_ctx, tev, proc);
		CommandCounterIncrement();
	}
	return;
}
