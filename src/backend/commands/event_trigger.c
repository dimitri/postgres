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

static HTAB *EventCommandTriggerCache = NULL;
bool event_trigger_cache_is_stalled = true;

/* entry for command event trigger lookup hashtable */
typedef struct
{
	TrigEvent			event;
	TrigEventCommand	command;
	List               *funcs;
} EventCommandTriggerEnt;

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
		nulls[Anum_pg_event_trigger_evttags - 1] = true;
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

		values[Anum_pg_event_trigger_evttags - 1] = PointerGetDatum(tagArray);
	}

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
	 * Find and validate the trigger function.
	 */
	funcoid = LookupFuncName(stmt->funcname, 0, NULL, false);

	/* we need the trigger type to validate the return type */
	funcrettype = get_func_rettype(funcoid);

	if (funcrettype != EVTTRIGGEROID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("function \"%s\" must return type \"command_trigger\"",
						NameListToString(stmt->funcname))));

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

	/* Insert the catalog entry */
	trigoid = InsertEventTriggerTuple(tgrel, stmt->trigname, stmt->event,
									  funcoid, stmt->timing, stmt->cmdlist);

	heap_close(tgrel, RowExclusiveLock);

	/* force rebuild next time we look at the cache */
	event_trigger_cache_is_stalled = true;

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

	/* force rebuild next time we look at the cache */
	event_trigger_cache_is_stalled = true;
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

	/* force rebuild next time we look at the cache */
	event_trigger_cache_is_stalled = true;
}


/*
 * Rename command trigger
 */
void
RenameEventTrigger(const char *trigname, const char *newname)
{
	SysScanDesc tgscan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Relation	rel;
	Form_pg_event_trigger evtForm;

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

	/* force rebuild next time we look at the cache */
	event_trigger_cache_is_stalled = true;
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
 * Add a new function to EventCommandTriggerCache for given command and event,
 * creating a new hash table entry when necessary.
 *
 * Returns the new hash entry value.
 */
static EventCommandTriggerEnt *
add_funcall_to_command_event(TrigEventCommand command,
							 TrigEvent event,
							 Oid func)
{
	uint32 key = (command << 16) + event;
	bool found;
	EventCommandTriggerEnt *hresult;

	hresult = (EventCommandTriggerEnt *)
		hash_search(EventCommandTriggerCache, &key, HASH_ENTER, &found);

	if (found)
	{
		Assert(hresult->command == command && hresult->event == event);
		hresult->funcs = lappend_oid(hresult->funcs, func);
	}
	else
	{
		hresult->command = command;
		hresult->event = event;
		hresult->funcs = list_make1_oid(func);
	}

	elog(NOTICE, "add_funcall_to_command_event: %d %d %p %d",
		 command, event, hresult->funcs, list_length(hresult->funcs));

	return hresult;
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
	HASHCTL		info;
	Relation	rel, irel;
	IndexScanDesc indexScan;
	HeapTuple	tuple;

	if (!event_trigger_cache_is_stalled && !force_rebuild)
		return;

	/* DEBUG */
	elog(NOTICE, "BuildEventTriggerCache rebuild");

	/* build the hash table */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(Oid);
	info.entrysize = sizeof(EventCommandTriggerEnt);
	info.hash = oid_hash;

	EventCommandTriggerCache = hash_create("Local Event Triggers Cache",
										   EVTG_MAX_TRIG_EVENT_COMMAND,
										   &info,
										   HASH_ELEM | HASH_FUNCTION);

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
			/* event triggers created without WHEN clause are targetting all
			 * commands (ANY command trigger)
			 */
			add_funcall_to_command_event(E_ANY, event, form->evtfoid);
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
				 add_funcall_to_command_event(command, event, form->evtfoid);
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
InitEventContext(EventContext evt, const Node *parsetree)
{
	evt->command	= E_UNKNOWN;
	evt->toplevel	= NULL;
	evt->tag		= (char *) CreateCommandTag((Node *)parsetree);
	evt->parsetree  = (Node *)parsetree;
	evt->objectId   = InvalidOid;
	evt->objectname = NULL;
	evt->schemaname = NULL;

	/*
	 * Fill in the event command, which is an enum constant to match against
	 * what's stored into catalogs. As we are storing that on disk, we need the
	 * enum values to be stable, see src/include/catalog/pg_event_trigger.h for
	 * details.
	 */
	switch (nodeTag(parsetree))
	{
		case T_CreateSchemaStmt:
			evt->command = E_CreateSchema;
			break;

		case T_CreateStmt:
			evt->command = E_CreateTable;
			break;

		case T_CreateForeignTableStmt:
			evt->command = E_CreateForeignTable;
			break;

		case T_CreateExtensionStmt:
			evt->command = E_CreateExtension;
			break;

		case T_AlterExtensionStmt:
		case T_AlterExtensionContentsStmt:
			evt->command = E_AlterExtension;
			break;

		case T_CreateFdwStmt:
			evt->command = E_CreateForeignDataWrapper;
			break;

		case T_AlterFdwStmt:
			evt->command = E_AlterForeignDataWrapper;
			break;

		case T_CreateForeignServerStmt:
			evt->command = E_CreateServer;
			break;

		case T_AlterForeignServerStmt:
			evt->command = E_AlterServer;
			break;

		case T_CreateUserMappingStmt:
			evt->command = E_CreateUserMapping;
			break;

		case T_AlterUserMappingStmt:
			evt->command = E_AlterUserMapping;
			break;

		case T_DropUserMappingStmt:
			evt->command = E_DropUserMapping;
			break;

		case T_DropStmt:
			switch (((DropStmt *) parsetree)->removeType)
			{
				case OBJECT_AGGREGATE:
					evt->command = E_DropAggregate;
					break;
				case OBJECT_CAST:
					evt->command = E_DropCast;
					break;
				case OBJECT_COLLATION:
					evt->command = E_DropCollation;
					break;
				case OBJECT_CONVERSION:
					evt->command = E_DropConversion;
					break;
				case OBJECT_DOMAIN:
					evt->command = E_DropDomain;
					break;
				case OBJECT_EXTENSION:
					evt->command = E_DropExtension;
					break;
				case OBJECT_FDW:
					evt->command = E_DropForeignDataWrapper;
					break;
				case OBJECT_FOREIGN_SERVER:
					evt->command = E_DropServer;
					break;
				case OBJECT_FOREIGN_TABLE:
					evt->command = E_DropForeignTable;
					break;
				case OBJECT_FUNCTION:
					evt->command = E_DropFunction;
					break;
				case OBJECT_INDEX:
					evt->command = E_DropIndex;
					break;
				case OBJECT_LANGUAGE:
					evt->command = E_DropLanguage;
					break;
				case OBJECT_OPCLASS:
					evt->command = E_DropOperatorClass;
					break;
				case OBJECT_OPERATOR:
					evt->command = E_DropOperator;
					break;
				case OBJECT_OPFAMILY:
					evt->command = E_DropOperatorFamily;
					break;
				case OBJECT_SCHEMA:
					evt->command = E_DropSchema;
					break;
				case OBJECT_SEQUENCE:
					evt->command = E_DropSequence;
					break;
				case OBJECT_TABLE:
					evt->command = E_DropTable;
					break;
				case OBJECT_TRIGGER:
					evt->command = E_DropTrigger;
					break;
				case OBJECT_TSCONFIGURATION:
					evt->command = E_DropTextSearchConfiguration;
					break;
				case OBJECT_TSDICTIONARY:
					evt->command = E_DropTextSearchDictionary;
					break;
				case OBJECT_TSPARSER:
					evt->command = E_DropTextSearchParser;
					break;
				case OBJECT_TSTEMPLATE:
					evt->command = E_DropTextSearchTemplate;
					break;
				case OBJECT_TYPE:
					evt->command = E_DropType;
					break;
				case OBJECT_VIEW:
					evt->command = E_DropView;
					break;
				case OBJECT_ROLE:
				case OBJECT_EVENT_TRIGGER:
				case OBJECT_ATTRIBUTE:
				case OBJECT_COLUMN:
				case OBJECT_CONSTRAINT:
				case OBJECT_DATABASE:
				case OBJECT_LARGEOBJECT:
				case OBJECT_RULE:
				case OBJECT_TABLESPACE:
					/* no support for specific command triggers */
					break;
			}
			break;

		case T_RenameStmt:
			switch (((RenameStmt *) parsetree)->renameType)
			{
				case OBJECT_ATTRIBUTE:
					evt->command = E_AlterType;
					break;
				case OBJECT_AGGREGATE:
					evt->command = E_AlterAggregate;
					break;
				case OBJECT_CAST:
					evt->command = E_AlterCast;
					break;
				case OBJECT_COLLATION:
					evt->command = E_AlterCollation;
					break;
				case OBJECT_COLUMN:
					evt->command = E_AlterTable;
					break;
				case OBJECT_CONVERSION:
					evt->command = E_AlterConversion;
					break;
				case OBJECT_DOMAIN:
					evt->command = E_AlterDomain;
					break;
				case OBJECT_EXTENSION:
					evt->command = E_AlterExtension;
					break;
				case OBJECT_FDW:
					evt->command = E_AlterForeignDataWrapper;
					break;
				case OBJECT_FOREIGN_SERVER:
					evt->command = E_AlterServer;
					break;
				case OBJECT_FOREIGN_TABLE:
					evt->command = E_AlterForeignTable;
					break;
				case OBJECT_FUNCTION:
					evt->command = E_AlterFunction;
					break;
				case OBJECT_INDEX:
					evt->command = E_AlterIndex;
					break;
				case OBJECT_LANGUAGE:
					evt->command = E_AlterLanguage;
					break;
				case OBJECT_OPCLASS:
					evt->command = E_AlterOperatorClass;
					break;
				case OBJECT_OPERATOR:
					evt->command = E_AlterOperator;
					break;
				case OBJECT_OPFAMILY:
					evt->command = E_AlterOperatorFamily;
					break;
				case OBJECT_SCHEMA:
					evt->command = E_AlterSchema;
					break;
				case OBJECT_SEQUENCE:
					evt->command = E_AlterSequence;
					break;
				case OBJECT_TABLE:
					evt->command = E_AlterTable;
					break;
				case OBJECT_TRIGGER:
					evt->command = E_AlterTrigger;
					break;
				case OBJECT_TSCONFIGURATION:
					evt->command = E_AlterTextSearchConfiguration;
					break;
				case OBJECT_TSDICTIONARY:
					evt->command = E_AlterTextSearchDictionary;
					break;
				case OBJECT_TSPARSER:
					evt->command = E_AlterTextSearchParser;
					break;
				case OBJECT_TSTEMPLATE:
					evt->command = E_AlterTextSearchTemplate;
					break;
				case OBJECT_TYPE:
					evt->command = E_AlterType;
					break;
				case OBJECT_VIEW:
					evt->command = E_AlterView;
					break;
				case OBJECT_ROLE:
				case OBJECT_EVENT_TRIGGER:
				case OBJECT_CONSTRAINT:
				case OBJECT_DATABASE:
				case OBJECT_LARGEOBJECT:
				case OBJECT_RULE:
				case OBJECT_TABLESPACE:
					/* no support for specific command triggers */
					break;
			}
			break;

		case T_AlterObjectSchemaStmt:
			switch (((AlterObjectSchemaStmt *) parsetree)->objectType)
			{
				case OBJECT_AGGREGATE:
					evt->command = E_AlterAggregate;
					break;
				case OBJECT_CAST:
					evt->command = E_AlterCast;
					break;
				case OBJECT_COLLATION:
					evt->command = E_AlterCollation;
					break;
				case OBJECT_CONVERSION:
					evt->command = E_AlterConversion;
					break;
				case OBJECT_DOMAIN:
					evt->command = E_AlterDomain;
					break;
				case OBJECT_EXTENSION:
					evt->command = E_AlterExtension;
					break;
				case OBJECT_FDW:
					evt->command = E_AlterForeignDataWrapper;
					break;
				case OBJECT_FOREIGN_SERVER:
					evt->command = E_AlterServer;
					break;
				case OBJECT_FOREIGN_TABLE:
					evt->command = E_AlterForeignTable;
					break;
				case OBJECT_FUNCTION:
					evt->command = E_AlterFunction;
					break;
				case OBJECT_INDEX:
					evt->command = E_AlterIndex;
					break;
				case OBJECT_LANGUAGE:
					evt->command = E_AlterLanguage;
					break;
				case OBJECT_OPCLASS:
					evt->command = E_AlterOperatorClass;
					break;
				case OBJECT_OPERATOR:
					evt->command = E_AlterOperator;
					break;
				case OBJECT_OPFAMILY:
					evt->command = E_AlterOperatorFamily;
					break;
				case OBJECT_SCHEMA:
					evt->command = E_AlterSchema;
					break;
				case OBJECT_SEQUENCE:
					evt->command = E_AlterSequence;
					break;
				case OBJECT_TABLE:
					evt->command = E_AlterTable;
					break;
				case OBJECT_TRIGGER:
					evt->command = E_AlterTrigger;
					break;
				case OBJECT_TSCONFIGURATION:
					evt->command = E_AlterTextSearchConfiguration;
					break;
				case OBJECT_TSDICTIONARY:
					evt->command = E_AlterTextSearchDictionary;
					break;
				case OBJECT_TSPARSER:
					evt->command = E_AlterTextSearchParser;
					break;
				case OBJECT_TSTEMPLATE:
					evt->command = E_AlterTextSearchTemplate;
					break;
				case OBJECT_TYPE:
					evt->command = E_AlterType;
					break;
				case OBJECT_VIEW:
					evt->command = E_AlterView;
					break;
				case OBJECT_ROLE:
				case OBJECT_EVENT_TRIGGER:
				case OBJECT_ATTRIBUTE:
				case OBJECT_COLUMN:
				case OBJECT_CONSTRAINT:
				case OBJECT_DATABASE:
				case OBJECT_LARGEOBJECT:
				case OBJECT_RULE:
				case OBJECT_TABLESPACE:
					/* no support for specific command triggers */
					break;
			}
			break;

		case T_AlterOwnerStmt:
			switch (((AlterOwnerStmt *) parsetree)->objectType)
			{
				case OBJECT_AGGREGATE:
					evt->command = E_AlterAggregate;
					break;
				case OBJECT_CAST:
					evt->command = E_AlterCast;
					break;
				case OBJECT_COLLATION:
					evt->command = E_AlterCollation;
					break;
				case OBJECT_CONVERSION:
					evt->command = E_AlterConversion;
					break;
				case OBJECT_DOMAIN:
					evt->command = E_AlterDomain;
					break;
				case OBJECT_EXTENSION:
					evt->command = E_AlterExtension;
					break;
				case OBJECT_FDW:
					evt->command = E_AlterForeignDataWrapper;
					break;
				case OBJECT_FOREIGN_SERVER:
					evt->command = E_AlterServer;
					break;
				case OBJECT_FOREIGN_TABLE:
					evt->command = E_AlterForeignTable;
					break;
				case OBJECT_FUNCTION:
					evt->command = E_AlterFunction;
					break;
				case OBJECT_INDEX:
					evt->command = E_AlterIndex;
					break;
				case OBJECT_LANGUAGE:
					evt->command = E_AlterLanguage;
					break;
				case OBJECT_OPCLASS:
					evt->command = E_AlterOperatorClass;
					break;
				case OBJECT_OPERATOR:
					evt->command = E_AlterOperator;
					break;
				case OBJECT_OPFAMILY:
					evt->command = E_AlterOperatorFamily;
					break;
				case OBJECT_SCHEMA:
					evt->command = E_AlterSchema;
					break;
				case OBJECT_SEQUENCE:
					evt->command = E_AlterSequence;
					break;
				case OBJECT_TABLE:
					evt->command = E_AlterTable;
					break;
				case OBJECT_TRIGGER:
					evt->command = E_AlterTrigger;
					break;
				case OBJECT_TSCONFIGURATION:
					evt->command = E_AlterTextSearchConfiguration;
					break;
				case OBJECT_TSDICTIONARY:
					evt->command = E_AlterTextSearchDictionary;
					break;
				case OBJECT_TSPARSER:
					evt->command = E_AlterTextSearchParser;
					break;
				case OBJECT_TSTEMPLATE:
					evt->command = E_AlterTextSearchTemplate;
					break;
				case OBJECT_TYPE:
					evt->command = E_AlterType;
					break;
				case OBJECT_VIEW:
					evt->command = E_AlterView;
					break;
				case OBJECT_ROLE:
				case OBJECT_EVENT_TRIGGER:
				case OBJECT_ATTRIBUTE:
				case OBJECT_COLUMN:
				case OBJECT_CONSTRAINT:
				case OBJECT_DATABASE:
				case OBJECT_LARGEOBJECT:
				case OBJECT_RULE:
				case OBJECT_TABLESPACE:
					/* no support for specific command triggers */
					break;
			}
			break;

		case T_AlterTableStmt:
			evt->command = E_AlterTable;
			break;

		case T_AlterDomainStmt:
			evt->command = E_AlterDomain;
			break;

		case T_DefineStmt:
			switch (((DefineStmt *) parsetree)->kind)
			{
				case OBJECT_AGGREGATE:
					evt->command = E_CreateAggregate;
					break;
				case OBJECT_OPERATOR:
					evt->command = E_CreateOperator;
					break;
				case OBJECT_TYPE:
					evt->command = E_CreateType;
					break;
				case OBJECT_TSPARSER:
					evt->command = E_CreateTextSearchParser;
					break;
				case OBJECT_TSDICTIONARY:
					evt->command = E_CreateTextSearchDictionary;;
					break;
				case OBJECT_TSTEMPLATE:
					evt->command = E_CreateTextSearchTemplate;
					break;
				case OBJECT_TSCONFIGURATION:
					evt->command = E_CreateTextSearchConfiguration;
					break;
				case OBJECT_COLLATION:
					evt->command = E_CreateCollation;
					break;
				default:
					elog(ERROR, "unrecognized define stmt type: %d",
						 (int) ((DefineStmt *) parsetree)->kind);
					break;
			}
			break;

		case T_CompositeTypeStmt:		/* CREATE TYPE (composite) */
		case T_CreateEnumStmt:	/* CREATE TYPE AS ENUM */
		case T_CreateRangeStmt:	/* CREATE TYPE AS RANGE */
			evt->command = E_CreateType;
			break;

		case T_AlterEnumStmt:	/* ALTER TYPE (enum) */
			evt->command = E_AlterType;
			break;

		case T_ViewStmt:		/* CREATE VIEW */
			evt->command = E_CreateView;
			break;

		case T_CreateFunctionStmt:		/* CREATE FUNCTION */
			evt->command = E_CreateFunction;
			break;

		case T_AlterFunctionStmt:		/* ALTER FUNCTION */
			evt->command = E_AlterFunction;
			break;

		case T_IndexStmt:		/* CREATE INDEX */
			evt->command = E_CreateIndex;
			break;

		case T_CreateSeqStmt:
			evt->command = E_CreateSequence;
			break;

		case T_AlterSeqStmt:
			evt->command = E_AlterSequence;
			break;

		case T_LoadStmt:
			evt->command = E_Load;
			break;

		case T_ClusterStmt:
			evt->command = E_Cluster;
			break;

		case T_VacuumStmt:
			evt->command = E_Vacuum;
			break;

		case T_CreateTableAsStmt:
			evt->command = E_CreateTableAs;
			break;

		case T_CreateTrigStmt:
			evt->command = E_CreateTrigger;
			break;

		case T_CreateDomainStmt:
			evt->command = E_CreateDomain;
			break;

		case T_ReindexStmt:
			evt->command = E_Reindex;
			break;

		case T_CreateConversionStmt:
			evt->command = E_CreateConversion;
			break;

		case T_CreateCastStmt:
			evt->command = E_CreateCast;
			break;

		case T_CreateOpClassStmt:
			evt->command = E_CreateOperatorClass;
			break;

		case T_CreateOpFamilyStmt:
			evt->command = E_CreateOperatorFamily;
			break;

		case T_AlterOpFamilyStmt:
			evt->command = E_AlterOperatorFamily;
			break;

		case T_AlterTSDictionaryStmt:
			evt->command = E_AlterTextSearchDictionary;
			break;

		case T_AlterTSConfigurationStmt:
			evt->command = E_AlterTextSearchConfiguration;
			break;

		default:
			/* reaching that part of the code only means that we are not
			 * supporting command triggers for the given command, which still
			 * needs to execute.
			 */
			break;
	}
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
	if (ev_ctx == NULL || ev_ctx->command == E_UNKNOWN)
		return false;

	/* Make sure we have initialized at least once the cache */
	BuildEventTriggerCache(false);

	elog(ERROR, "Not Yet Implemented");
	return false;
}

/*
 * It's still interresting to avoid preparing the Command Context for AFTER
 * command triggers when we have none to Execute, so we provide this API too.
 */
bool
CommandFiresTriggersForEvent(EventContext ev_ctx, TrigEvent tev)
{
	uint32 anykey = (E_ANY << 16) + tev;
	uint32 cmdkey = (ev_ctx->command << 16) + tev;
	bool any = false, cmd = false;

	if (ev_ctx == NULL || ev_ctx->command == E_UNKNOWN)
		return false;

	BuildEventTriggerCache(false);

	hash_search(EventCommandTriggerCache, &anykey, HASH_FIND, &any);

	if (!any)
		hash_search(EventCommandTriggerCache, &cmdkey, HASH_FIND, &cmd);

	elog(NOTICE, "CommandFiresTriggersForEvent %d %d %s",
		 ev_ctx->command, tev, (any||cmd) ? "yes" : "no");

	return any||cmd;
}

void
ExecEventTriggers(EventContext ev_ctx, TrigEvent tev)
{
	uint32 anykey = (E_ANY << 16) + tev;
	uint32 cmdkey = (ev_ctx->command << 16) + tev;
	EventCommandTriggerEnt *hresult;
	ListCell *lc;

	if (ev_ctx == NULL || ev_ctx->command == E_UNKNOWN)
		return;

	BuildEventTriggerCache(false);

	/* ANY command triggers */
	hresult = (EventCommandTriggerEnt *)
		hash_search(EventCommandTriggerCache, &anykey, HASH_FIND, NULL);

	elog(NOTICE, "ExecEventTriggers: any %p", hresult);

	if (hresult != NULL)
	{
		foreach(lc, hresult->funcs)
		{
			RegProcedure proc = (RegProcedure) lfirst_oid(lc);

			call_event_trigger_procedure(ev_ctx, tev, proc);
		}
	}

	/* Specific command triggers */
	hresult = (EventCommandTriggerEnt *)
		hash_search(EventCommandTriggerCache, &cmdkey, HASH_FIND, NULL);

	elog(NOTICE, "ExecEventTriggers: cmd %p", hresult);

	if (hresult != NULL)
	{
		foreach(lc, hresult->funcs)
		{
			RegProcedure proc = (RegProcedure) lfirst_oid(lc);

			call_event_trigger_procedure(ev_ctx, tev, proc);
		}
	}
	return;
}
