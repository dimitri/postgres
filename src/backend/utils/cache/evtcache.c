/*-------------------------------------------------------------------------
 *
 * evtcache.c
 *	  Per Command Event Trigger cache management.
 *
 * Event trigger command cache is maintained separately from the event name
 * catalog cache.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/cache/evtcache.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_type.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "utils/syscache.h"

/*
 * Cache the event triggers in a format that's suitable to finding which
 * function to call at "hook" points in the code. The catalogs are not helpful
 * at search time, because we can't both edit a single catalog entry per each
 * command, have a user friendly syntax and find what we need in a single index
 * scan.
 *
 * This cache is indexed by Event id then Event Command id (see
 * pg_event_trigger.h). It's containing a list of function oid.
 */
static HTAB *EventCommandTriggerCache = NULL;

/* event and command form the lookup key, and must appear first */
typedef struct
{
	TrigEvent			event;
	TrigEventCommand	command;
} EventCommandTriggerCacheKey;


/* entry for command event trigger lookup hashtable */
typedef struct
{
	EventCommandTriggerCacheKey key; /* lookup key, must be first */
	List *names;					 /* list of names of the triggers to call */
	List *procs;					 /* list of triggers to call */
} EventCommandTriggerCacheEntry;

/*
 * Add a new function to EventCommandTriggerCache for given command and event,
 * creating a new hash table entry when necessary.
 *
 * Returns the new hash entry value.
 */
static EventCommandTriggerCacheEntry *
add_funcall_to_command_event(TrigEvent event,
							 TrigEventCommand command,
							 NameData evtname,
							 Oid proc)
{
	bool found;
	EventCommandTriggerCacheKey key;
	EventCommandTriggerCacheEntry *hresult;
	MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

	memset(&key, 0, sizeof(key));
	key.event = event;
	key.command = command;

	hresult = (EventCommandTriggerCacheEntry *)
		hash_search(EventCommandTriggerCache, (void *)&key, HASH_ENTER, &found);

	if (found)
	{
		hresult->names = lappend(hresult->names, pstrdup(NameStr(evtname)));
		hresult->procs = lappend_oid(hresult->procs, proc);
	}
	else
	{
		hresult->names = list_make1(pstrdup(NameStr(evtname)));
		hresult->procs = list_make1_oid(proc);
	}

	MemoryContextSwitchTo(old);
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
static void
BuildEventTriggerCache()
{
	HASHCTL		info;
	Relation	rel, irel;
	IndexScanDesc indexScan;
	HeapTuple	tuple;

	/* build the new hash table */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(EventCommandTriggerCacheKey);
	info.entrysize = sizeof(EventCommandTriggerCacheEntry);
	info.hash = tag_hash;
	info.hcxt = CacheMemoryContext;

	/* Create the hash table holding our cache */
	EventCommandTriggerCache =
		hash_create("Event Trigger Command Cache",
					1024,
					&info,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	/* fill in the cache from the catalogs */
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
		NameData    name;
		Oid         proc;

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
		name = form->evtname;
		proc = form->evtfoid;

		adatum = heap_getattr(tuple, Anum_pg_event_trigger_evttags,
							  RelationGetDescr(rel), &isNull);

		if (isNull)
		{
			/* event triggers created without WHEN clause are targetting all
			 * commands (ANY command trigger)
			 */
			add_funcall_to_command_event(event, E_ANY, name, proc);
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
				 add_funcall_to_command_event(event, command, name, proc);
			}
		}
	}
	index_endscan(indexScan);
	index_close(irel, AccessShareLock);
	heap_close(rel, AccessShareLock);
}

/*
 * InvalidateEvtTriggerCacheCallback
 *		Flush all cache entries when pg_event_trigger is updated.
 *
 */
static void
InvalidateEvtTriggerCommandCacheCallback(Datum arg,
										 int cacheid, uint32 hashvalue)
{
	hash_destroy(EventCommandTriggerCache);
	EventCommandTriggerCache = NULL;
}

/*
 * InitializeEvtTriggerCommandCache
 *		Initialize the event trigger command cache.
 *
 * That routime is called from postinit.c and must not do any database access.
 */
void
InitEventTriggerCache(void)
{
	/* Make sure we've initialized CacheMemoryContext. */
	if (!CacheMemoryContext)
		CreateCacheMemoryContext();

	EventCommandTriggerCache = NULL;

	/* Watch for invalidation events. */
	CacheRegisterSyscacheCallback(EVENTTRIGGERNAME,
								  InvalidateEvtTriggerCommandCacheCallback,
								  (Datum) 0);
}

/*
 * public API to list triggers to call for a given event and command
 */
EventCommandTriggers *
get_event_triggers(TrigEvent event, TrigEventCommand command)
{
	EventCommandTriggers *triggers =
		(EventCommandTriggers *) palloc(sizeof(EventCommandTriggers));
	EventCommandTriggerCacheKey anykey, cmdkey;
	EventCommandTriggerCacheEntry *any, *cmd;

	triggers->event = event;
	triggers->command = command;
	triggers->procs = NIL;

	/* Find existing cache entry, if any. */
	if (!EventCommandTriggerCache)
		BuildEventTriggerCache();

	/* ANY command triggers */
	memset(&anykey, 0, sizeof(anykey));
	anykey.event = event;
	anykey.command = E_ANY;
	any = (EventCommandTriggerCacheEntry *)
		hash_search(EventCommandTriggerCache, (void *)&anykey, HASH_FIND, NULL);

	/* Specific command triggers */
	memset(&cmdkey, 0, sizeof(cmdkey));
	cmdkey.event = event;
	cmdkey.command = command;
	cmd = (EventCommandTriggerCacheEntry *)
		hash_search(EventCommandTriggerCache, (void *)&cmdkey, HASH_FIND, NULL);

	if (any == NULL && cmd == NULL)
		return triggers;
	else if (any == NULL)
		triggers->procs = cmd->procs;
	else if (cmd == NULL)
		triggers->procs = any->procs;
	else
	{
		/* merge join the two lists keeping the ordering by name */
		ListCell *lc_any_procs, *lc_any_names, *lc_cmd_procs, *lc_cmd_names;
		char *current_any_name, *current_cmd_name;

		lc_any_names = list_head(any->names);
		lc_any_procs = list_head(any->procs);

		lc_cmd_names = list_head(cmd->names);
		lc_cmd_procs = list_head(cmd->procs);

		do
		{
			current_cmd_name = (char *) lfirst(lc_cmd_names);

			/* append all elements from ANY list named before those from CMD */
			while (lc_any_procs != NULL
				   && strcmp((current_any_name = (char *) lfirst(lc_any_names)),
							 current_cmd_name) < 0)
			{
				if (triggers->procs == NULL)
					triggers->procs = list_make1_oid(lfirst_oid(lc_any_procs));
				else
					triggers->procs = lappend_oid(triggers->procs,
												  lfirst_oid(lc_any_procs));

				lc_any_names = lnext(lc_any_names);
				lc_any_procs = lnext(lc_any_procs);
			}

			/* now append as many elements from CMD list named before next ANY
			 * entry
			 */
			do
			{
				if (triggers->procs == NULL)
					triggers->procs = list_make1_oid(lfirst_oid(lc_cmd_procs));
				else
					triggers->procs = lappend_oid(triggers->procs,
												  lfirst_oid(lc_cmd_procs));

				lc_cmd_names = lnext(lc_cmd_names);
				lc_cmd_procs = lnext(lc_cmd_procs);

				if (lc_cmd_names != NULL)
					current_cmd_name = (char *) lfirst(lc_cmd_names);
			}
			while (lc_cmd_names != NULL
				   && strcmp(current_cmd_name, current_any_name) < 0);
		}
		while( lc_cmd_names != NULL && lc_any_names != NULL );
	}
	return triggers;
}
