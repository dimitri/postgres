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
#include "catalog/pg_collation.h"
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
#include "nodes/parsenodes.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/evtcache.h"
#include "utils/formatting.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "utils/syscache.h"

/*
 * EventTriggerCommandTags
 *
 * This array provides meta data allowing to parse and rewrite command tags
 * from the command and catalogs to the internal integers we use to have fast
 * lookups.
 *
 * Lookups have to be fast because they are done for each and every DDL as soon
 * as some Event Triggers are defined.
 */
typedef struct
{
	TrigEventCommand	 command; /* internal command value */
	char			 	*tag;	  /* command tag */
	NodeTag				 node;	  /* internal parser node tag */
	ObjectType			 type;	  /* internal object type */
} EventTriggerCommandTagsType;

/*
 * Hash table to cache the content of EventTriggerCommandTags, which is
 * searched by command tag when building the EventTriggerProcsCache, and by
 * NodeTag and ObjectType from ProcessUtility.
 *
 * In both cases we want to avoid to have to scan the whole array each time, so
 * we cache a dedicated hash table in the session's memory.
 */
static HTAB *EventTriggerCommandTagsCache = NULL;
static HTAB *EventTriggerCommandNodeCache = NULL;

/* entry for the Tags cache (key is NameData of NAMEDATALEN) */
typedef struct
{
	NameData			tag;
	TrigEventCommand	command;
} EventTriggerCommandTagsEntry;

/* key and entry for the Node cache */
typedef struct
{
	NodeTag				 node;	  /* internal parser node tag */
	ObjectType			 type;	  /* internal object type */
} EventTriggerCommandNodeKey;

typedef struct
{
	EventTriggerCommandNodeKey key; /* lookup key, must be first */
	TrigEventCommand	 command;	/* internal command value */
} EventTriggerCommandNodeEntry;

static EventTriggerCommandTagsType EventTriggerCommandTags[] =
{
	{
		ETC_CreateAggregate,
		"CREATE AGGREGATE",
		T_DefineStmt,
		OBJECT_AGGREGATE
	},
	{
		ETC_CreateCast,
		"CREATE CAST",
		T_CreateCastStmt,
		-1
	},
	{
		ETC_CreateCollation,
		"CREATE COLLATION",
		T_DefineStmt,
		OBJECT_COLLATION
	},
	{
		ETC_CreateConversion,
		"CREATE CONVERSION",
		T_CreateConversionStmt,
		-1
	},
	{
		ETC_CreateDomain,
		"CREATE DOMAIN",
		T_CreateDomainStmt,
		-1
	},
	{
		ETC_CreateExtension,
		"CREATE EXTENSION",
		T_CreateExtensionStmt,
		-1
	},
	{
		ETC_CreateForeignDataWrapper,
		"CREATE FOREIGN DATA WRAPPER",
		T_CreateFdwStmt,
		-1
	},
	{
		ETC_CreateForeignTable,
		"CREATE FOREIGN TABLE",
		T_CreateForeignTableStmt,
		-1
	},
	{
		ETC_CreateFunction,
		"CREATE FUNCTION",
		T_CreateFunctionStmt,
		-1
	},
	{
		ETC_CreateIndex,
		"CREATE INDEX",
		T_IndexStmt,
		-1
	},
	{
		ETC_CreateLanguage,
		"CREATE LANGUAGE",
		T_CreatePLangStmt,
		-1
	},
	{
		ETC_CreateOperator,
		"CREATE OPERATOR",
		T_DefineStmt,
		OBJECT_OPERATOR
	},
	{
		ETC_CreateOperatorClass,
		"CREATE OPERATOR CLASS",
		T_CreateOpClassStmt,
		-1
	},
	{
		ETC_CreateOperatorFamily,
		"CREATE OPERATOR FAMILY",
		T_CreateOpFamilyStmt,
		-1
	},
	{
		ETC_CreateRule,
		"CREATE RULE",
		T_RuleStmt,
		-1
	},
	{
		ETC_CreateSchema,
		"CREATE SCHEMA",
		T_CreateSchemaStmt,
		-1
	},
	{
		ETC_CreateSequence,
		"CREATE SEQUENCE",
		T_CreateSeqStmt,
		-1
	},
	{
		ETC_CreateServer,
		"CREATE SERVER",
		T_CreateForeignServerStmt,
		-1
	},
	{
		ETC_CreateTable,
		"CREATE TABLE",
		T_CreateStmt,
		-1
	},
	{
		ETC_CreateTableAs,
		"CREATE TABLE AS",
		T_CreateTableAsStmt,
		-1
	},
	{
		ETC_SelectInto,
		"SELECT INTO",
		T_CreateTableAsStmt,
		-1
	},
	{
		ETC_CreateTextSearchParser,
		"CREATE TEXT SEARCH PARSER",
		T_DefineStmt,
		OBJECT_TSPARSER
	},
	{
		ETC_CreateTextSearchConfiguration,
		"CREATE TEXT SEARCH CONFIGURATION",
		T_DefineStmt,
		OBJECT_TSCONFIGURATION
	},
	{
		ETC_CreateTextSearchDictionary,
		"CREATE TEXT SEARCH DICTIONARY",
		T_DefineStmt,
		OBJECT_TSDICTIONARY
	},
	{
		ETC_CreateTextSearchTemplate,
		"CREATE TEXT SEARCH TEMPLATE",
		T_DefineStmt,
		OBJECT_TSTEMPLATE
	},
	{
		ETC_CreateTrigger,
		"CREATE TRIGGER",
		T_CreateTrigStmt,
		-1
	},
	{
		ETC_CreateType,
		"CREATE TYPE",
		T_DefineStmt,
		OBJECT_TYPE
	},
	{
		ETC_CreateType,
		"CREATE TYPE",
		T_CompositeTypeStmt,
		-1
	},
	{
		ETC_CreateType,
		"CREATE TYPE",
		T_CreateEnumStmt,
		-1
	},
	{
		ETC_CreateType,
		"CREATE TYPE",
		T_CreateRangeStmt,
		-1
	},
	{
		ETC_CreateUserMapping,
		"CREATE USER MAPPING",
		T_CreateUserMappingStmt,
		-1
	},
	{
		ETC_CreateView,
		"CREATE VIEW",
		T_ViewStmt,
		-1
	},
	{
		ETC_AlterTable,
		"ALTER TABLE",
		T_AlterTableStmt,
		-1
	},
	{
		ETC_DropAggregate,
		"DROP AGGREGATE",
		T_DropStmt,
		OBJECT_AGGREGATE
	},
	{
		ETC_DropCast,
		"DROP CAST",
		T_DropStmt,
		OBJECT_CAST
	},
	{
		ETC_DropCollation,
		"DROP COLLATION",
		T_DropStmt,
		OBJECT_COLLATION
	},
	{
		ETC_DropConversion,
		"DROP CONVERSION",
		T_DropStmt,
		OBJECT_CONVERSION
	},
	{
		ETC_DropDomain,
		"DROP DOMAIN",
		T_DropStmt,
		OBJECT_DOMAIN
	},
	{
		ETC_DropExtension,
		"DROP EXTENSION",
		T_DropStmt,
		OBJECT_EXTENSION
	},
	{
		ETC_DropForeignDataWrapper,
		"DROP FOREIGN DATA WRAPPER",
		T_DropStmt,
		OBJECT_FDW
	},
	{
		ETC_DropForeignTable,
		"DROP FOREIGN TABLE",
		T_DropStmt,
		OBJECT_FOREIGN_TABLE
	},
	{
		ETC_DropFunction,
		"DROP FUNCTION",
		T_DropStmt,
		OBJECT_FUNCTION
	},
	{
		ETC_DropIndex,
		"DROP INDEX",
		T_DropStmt,
		OBJECT_INDEX
	},
	{
		ETC_DropLanguage,
		"DROP LANGUAGE",
		T_DropStmt,
		OBJECT_LANGUAGE
	},
	{
		ETC_DropOperator,
		"DROP OPERATOR",
		T_DropStmt,
		OBJECT_OPERATOR
	},
	{
		ETC_DropOperatorClass,
		"DROP OPERATOR CLASS",
		T_DropStmt,
		OBJECT_OPCLASS
	},
	{
		ETC_DropOperatorFamily,
		"DROP OPERATOR FAMILY",
		T_DropStmt,
		OBJECT_OPFAMILY
	},
	{
		ETC_DropRule,
		"DROP RULE",
		T_DropStmt,
		OBJECT_RULE
	},
	{
		ETC_DropSchema,
		"DROP SCHEMA",
		T_DropStmt,
		OBJECT_SCHEMA
	},
	{
		ETC_DropSequence,
		"DROP SEQUENCE",
		T_DropStmt,
		OBJECT_SEQUENCE
	},
	{
		ETC_DropServer,
		"DROP SERVER",
		T_DropStmt,
		OBJECT_FOREIGN_SERVER
	},
	{
		ETC_DropTable,
		"DROP TABLE",
		T_DropStmt,
		OBJECT_TABLE
	},
	{
		ETC_DropTextSearchParser,
		"DROP TEXT SEARCH PARSER",
		T_DropStmt,
		OBJECT_TSPARSER
	},
	{
		ETC_DropTextSearchConfiguration,
		"DROP TEXT SEARCH CONFIGURATION",
		T_DropStmt,
		OBJECT_TSCONFIGURATION
	},
	{
		ETC_DropTextSearchDictionary,
		"DROP TEXT SEARCH DICTIONARY",
		T_DropStmt,
		OBJECT_TSDICTIONARY
	},
	{
		ETC_DropTextSearchTemplate,
		"DROP TEXT SEARCH TEMPLATE",
		T_DropStmt,
		OBJECT_TSTEMPLATE
	},
	{
		ETC_DropTrigger,
		"DROP TRIGGER",
		T_DropStmt,
		OBJECT_TRIGGER
	},
	{
		ETC_DropType,
		"DROP TYPE",
		T_DropStmt,
		OBJECT_TYPE
	},
	{
		ETC_DropUserMapping,
		"DROP USER MAPPING",
		T_DropUserMappingStmt,
		-1
	},
	{
		ETC_DropView,
		"DROP VIEW",
		T_DropStmt,
		OBJECT_VIEW
	},
	{
		ETC_Vacuum,
		"VACUUM",
		T_VacuumStmt,
		-1
	},
	{
		ETC_Cluster,
		"CLUSTER",
		T_ClusterStmt,
		-1
	},
	{
		ETC_Load,
		"LOAD",
		T_LoadStmt,
		-1
	},
	{
		ETC_Reindex,
		"REINDEX",
		T_ReindexStmt,
		-1
	},
	{
		ETC_AlterSequence,
		"ALTER SEQUENCE",
		T_AlterSeqStmt,
		-1
	},
	{
		ETC_AlterUserMapping,
		"ALTER USER MAPPING",
		T_CreateUserMappingStmt,
		-1
	},
	{
		ETC_AlterFunction,
		"ALTER FUNCTION",
		T_AlterFunctionStmt,
		-1
	},
	{
		ETC_AlterDomain,
		"ALTER DOMAIN",
		T_AlterDomainStmt,
		-1
	},
	/* ALTER <OBJECT> name RENAME TO */
	{
		ETC_AlterAggregate,
		"ALTER AGGREGATE",
		T_RenameStmt,
		OBJECT_AGGREGATE
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_RenameStmt,
		OBJECT_ATTRIBUTE
	},
	{
		ETC_AlterCast,
		"ALTER CAST",
		T_RenameStmt,
		OBJECT_CAST
	},
	{
		ETC_AlterCollation,
		"ALTER COLLATION",
		T_RenameStmt,
		OBJECT_COLLATION
	},
	{
		ETC_AlterTable,
		"ALTER TABLE",
		T_RenameStmt,
		OBJECT_COLUMN
	},
	{
		ETC_AlterTable,
		"ALTER TABLE",
		T_RenameStmt,
		OBJECT_CONSTRAINT
	},
	{
		ETC_AlterConversion,
		"ALTER CONVERSION",
		T_RenameStmt,
		OBJECT_CONVERSION
	},
	{
		ETC_AlterDomain,
		"ALTER DOMAIN",
		OBJECT_DOMAIN,
		T_RenameStmt
	},
	{
		ETC_AlterExtension,
		"ALTER EXTENSION",
		T_RenameStmt,
		OBJECT_EXTENSION
	},
	{
		ETC_AlterForeignDataWrapper,
		"ALTER FOREIGN DATA WRAPPER",
		OBJECT_FDW,
		T_RenameStmt
	},
	{
		ETC_AlterServer,
		"ALTER SERVER",
		T_RenameStmt,
		OBJECT_FOREIGN_SERVER
	},
	{
		ETC_AlterForeignTable,
		"ALTER FOREIGN TABLE",
		T_RenameStmt,
		OBJECT_FOREIGN_TABLE
	},
	{
		ETC_AlterFunction,
		"ALTER FUNCTION",
		T_RenameStmt,
		OBJECT_FUNCTION
	},
	{
		ETC_AlterIndex,
		"ALTER INDEX",
		T_RenameStmt,
		OBJECT_INDEX
	},
	{
		ETC_AlterLanguage,
		"ALTER LANGUAGE",
		T_RenameStmt,
		OBJECT_LANGUAGE
	},
	{
		ETC_AlterOperator,
		"ALTER OPERATOR",
		T_RenameStmt,
		OBJECT_OPERATOR
	},
	{
		ETC_AlterOperatorClass,
		"ALTER OPERATOR CLASS",
		T_RenameStmt,
		OBJECT_OPCLASS
	},
	{
		ETC_AlterOperatorFamily,
		"ALTER OPERATOR FAMILY",
		T_RenameStmt,
		OBJECT_OPFAMILY
	},
	{
		ETC_AlterRule,
		"ALTER RULE",
		T_RenameStmt,
		OBJECT_RULE
	},
	{
		ETC_AlterSchema,
		"ALTER SCHEMA",
		T_RenameStmt,
		OBJECT_SCHEMA
	},
	{
		ETC_AlterSequence,
		"ALTER SEQUENCE",
		T_RenameStmt,
		OBJECT_SEQUENCE
	},
	{
		ETC_AlterTable,
		"ALTER TABLE",
		T_RenameStmt,
		OBJECT_TABLE
	},
	{
		ETC_AlterTrigger,
		"ALTER TRIGGER",
		T_RenameStmt,
		OBJECT_TRIGGER
	},
	{
		ETC_AlterTextSearchParser,
		"ALTER TEXT SEARCH PARSER",
		T_RenameStmt,
		OBJECT_TSPARSER
	},
	{
		ETC_AlterTextSearchConfiguration,
		"ALTER TEXT SEARCH CONFIGURATION",
		T_RenameStmt,
		OBJECT_TSCONFIGURATION
	},
	{
		ETC_AlterTextSearchDictionary,
		"ALTER TEXT SEARCH DICTIONARY",
		T_RenameStmt,
		OBJECT_TSDICTIONARY
	},
	{
		ETC_AlterTextSearchTemplate,
		"ALTER TEXT SEARCH TEMPLATE",
		T_RenameStmt,
		OBJECT_TSTEMPLATE
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_RenameStmt,
		OBJECT_TYPE
	},
	{
		ETC_AlterView,
		"ALTER VIEW",
		T_RenameStmt,
		OBJECT_VIEW
	},
	/* ALTER <OBJECT> name SET SCHEMA */
	{
		ETC_AlterAggregate,
		"ALTER AGGREGATE",
		T_AlterObjectSchemaStmt,
		OBJECT_AGGREGATE
	},
	{
		ETC_AlterCast,
		"ALTER CAST",
		T_AlterObjectSchemaStmt,
		OBJECT_CAST
	},
	{
		ETC_AlterCollation,
		"ALTER COLLATION",
		T_AlterObjectSchemaStmt,
		OBJECT_COLLATION
	},
	{
		ETC_AlterConversion,
		"ALTER CONVERSION",
		T_AlterObjectSchemaStmt,
		OBJECT_CONVERSION
	},
	{
		ETC_AlterDomain,
		"ALTER DOMAIN",
		T_AlterObjectSchemaStmt,
		OBJECT_DOMAIN
	},
	{
		ETC_AlterExtension,
		"ALTER EXTENSION",
		T_AlterObjectSchemaStmt,
		OBJECT_EXTENSION
	},
	{
		ETC_AlterForeignDataWrapper,
		"ALTER FOREIGN DATA WRAPPER",
		T_AlterObjectSchemaStmt,
		OBJECT_FDW
	},
	{
		ETC_AlterForeignTable,
		"ALTER FOREIGN TABLE",
		T_AlterObjectSchemaStmt,
		OBJECT_FOREIGN_TABLE
	},
	{
		ETC_AlterFunction,
		"ALTER FUNCTION",
		T_AlterObjectSchemaStmt,
		OBJECT_FUNCTION
	},
	{
		ETC_AlterIndex,
		"ALTER CAST",
		T_AlterObjectSchemaStmt,
		OBJECT_INDEX
	},
	{
		ETC_AlterLanguage,
		"ALTER LANGUAGE",
		T_AlterObjectSchemaStmt,
		OBJECT_LANGUAGE
	},
	{
		ETC_AlterOperator,
		"ALTER OPERATOR",
		T_AlterObjectSchemaStmt,
		OBJECT_OPERATOR
	},
	{
		ETC_AlterOperatorClass,
		"ALTER OPERATOR CLASS",
		T_AlterObjectSchemaStmt,
		OBJECT_OPCLASS
	},
	{
		ETC_AlterOperatorFamily,
		"ALTER OPERATOR FAMILY",
		T_AlterObjectSchemaStmt,
		OBJECT_OPFAMILY
	},
	{
		ETC_AlterSchema,
		"ALTER SCHEMA",
		T_AlterObjectSchemaStmt,
		OBJECT_SCHEMA
	},
	{
		ETC_AlterSequence,
		"ALTER SEQUENCE",
		T_AlterObjectSchemaStmt,
		OBJECT_SEQUENCE
	},
	{
		ETC_AlterServer,
		"ALTER SERVER",
		T_AlterObjectSchemaStmt,
		OBJECT_FOREIGN_SERVER
	},
	{
		ETC_AlterTable,
		"ALTER TABLE",
		T_AlterObjectSchemaStmt,
		OBJECT_TABLE
	},
	{
		ETC_AlterTextSearchParser,
		"ALTER TEXT SEARCH PARSER",
		T_AlterObjectSchemaStmt,
		OBJECT_TSPARSER
	},
	{
		ETC_AlterTextSearchConfiguration,
		"ALTER TEXT SEARCH CONFIGURATION",
		T_AlterObjectSchemaStmt,
		OBJECT_TSCONFIGURATION
	},
	{
		ETC_AlterTextSearchDictionary,
		"ALTER TEXT SEARCH DICTIONARY",
		T_AlterObjectSchemaStmt,
		OBJECT_TSDICTIONARY
	},
	{
		ETC_AlterTextSearchTemplate,
		"ALTER TEXT SEARCH TEMPLATE",
		T_AlterObjectSchemaStmt,
		OBJECT_TSTEMPLATE
	},
	{
		ETC_AlterTrigger,
		"ALTER TRIGGER",
		T_AlterObjectSchemaStmt,
		OBJECT_TRIGGER
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_AlterEnumStmt,
		-1
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_AlterObjectSchemaStmt,
		OBJECT_ATTRIBUTE
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_AlterObjectSchemaStmt,
		OBJECT_TYPE
	},
	{
		ETC_AlterView,
		"ALTER VIEW",
		T_AlterObjectSchemaStmt,
		OBJECT_VIEW
	},
	{
		ETC_AlterTextSearchDictionary,
		"ALTER TEXT SEARCH DICTIONARY",
		T_AlterTSDictionaryStmt,
		-1
	},
	/* ALTER <OBJECT> name OWNER TO */
	{
		ETC_AlterAggregate,
		"ALTER AGGREGATE",
		T_AlterOwnerStmt,
		OBJECT_AGGREGATE
	},
	{
		ETC_AlterCast,
		"ALTER CAST",
		T_AlterOwnerStmt,
		OBJECT_CAST
	},
	{
		ETC_AlterCollation,
		"ALTER COLLATION",
		T_AlterOwnerStmt,
		OBJECT_COLLATION
	},
	{
		ETC_AlterConversion,
		"ALTER CONVERSION",
		T_AlterOwnerStmt,
		OBJECT_CONVERSION
	},
	{
		ETC_AlterDomain,
		"ALTER DOMAIN",
		T_AlterOwnerStmt,
		OBJECT_DOMAIN
	},
	{
		ETC_AlterExtension,
		"ALTER EXTENSION",
		T_AlterOwnerStmt,
		OBJECT_EXTENSION
	},
	{
		ETC_AlterForeignDataWrapper,
		"ALTER FOREIGN DATA WRAPPER",
		T_AlterOwnerStmt,
		OBJECT_FDW
	},
	{
		ETC_AlterForeignTable,
		"ALTER FOREIGN TABLE",
		T_AlterOwnerStmt,
		OBJECT_FOREIGN_TABLE
	},
	{
		ETC_AlterFunction,
		"ALTER FUNCTION",
		T_AlterOwnerStmt,
		OBJECT_FUNCTION
	},
	{
		ETC_AlterIndex,
		"ALTER CAST",
		T_AlterOwnerStmt,
		OBJECT_INDEX
	},
	{
		ETC_AlterLanguage,
		"command_start",
		T_AlterOwnerStmt,
		OBJECT_LANGUAGE
	},
	{
		ETC_AlterOperator,
		"ALTER OPERATOR",
		T_AlterOwnerStmt,
		OBJECT_OPERATOR
	},
	{
		ETC_AlterOperatorClass,
		"ALTER OPERATOR CLASS",
		T_AlterOwnerStmt,
		OBJECT_OPCLASS
	},
	{
		ETC_AlterOperatorFamily,
		"ALTER OPERATOR FAMILY",
		T_AlterOwnerStmt,
		OBJECT_OPFAMILY
	},
	{
		ETC_AlterSchema,
		"ALTER SCHEMA",
		T_AlterOwnerStmt,
		OBJECT_SCHEMA
	},
	{
		ETC_AlterSequence,
		"ALTER SEQUENCE",
		T_AlterOwnerStmt,
		OBJECT_SEQUENCE
	},
	{
		ETC_AlterServer,
		"ALTER SERVER",
		T_AlterOwnerStmt,
		OBJECT_FOREIGN_SERVER
	},
	{
		ETC_AlterTextSearchParser,
		"ALTER TEXT SEARCH PARSER",
		T_AlterOwnerStmt,
		OBJECT_TSPARSER
	},
	{
		ETC_AlterTextSearchConfiguration,
		"ALTER TEXT SEARCH CONFIGURATION",
		T_AlterOwnerStmt,
		OBJECT_TSCONFIGURATION
	},
	{
		ETC_AlterTextSearchDictionary,
		"ALTER TEXT SEARCH DICTIONARY",
		T_AlterOwnerStmt,
		OBJECT_TSDICTIONARY
	},
	{
		ETC_AlterTextSearchTemplate,
		"ALTER TEXT SEARCH TEMPLATE",
		T_AlterOwnerStmt,
		OBJECT_TSTEMPLATE
	},
	{
		ETC_AlterTrigger,
		"ALTER TRIGGER",
		T_AlterOwnerStmt,
		OBJECT_TRIGGER
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_AlterOwnerStmt,
		OBJECT_ATTRIBUTE
	},
	{
		ETC_AlterType,
		"ALTER TYPE",
		T_AlterOwnerStmt,
		OBJECT_TYPE
	},
	{
		ETC_AlterView,
		"ALTER VIEW",
		T_AlterOwnerStmt,
		OBJECT_VIEW
	}
};

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
static HTAB *EventTriggerProcsCache = NULL;

/* event and command form the lookup key, and must appear first */
typedef struct
{
	TrigEvent			event;
	TrigEventCommand	command;
} EventTriggerProcsCacheKey;


/* entry for command event trigger lookup hashtable */
typedef struct
{
	EventTriggerProcsCacheKey key; /* lookup key, must be first */
	List *names;					 /* list of names of the triggers to call */
	List *procs;					 /* list of triggers to call */
} EventTriggerProcsCacheEntry;

/*
 * Add a new function to EventTriggerProcsCache for given command and event,
 * creating a new hash table entry when necessary.
 *
 * Returns the new hash entry value.
 */
static EventTriggerProcsCacheEntry *
add_funcall_to_command_event(TrigEvent event,
							 TrigEventCommand command,
							 NameData evtname,
							 Oid proc)
{
	bool found;
	EventTriggerProcsCacheKey key;
	EventTriggerProcsCacheEntry *hresult;
	MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

	memset(&key, 0, sizeof(key));
	key.event = event;
	key.command = command;

	hresult = (EventTriggerProcsCacheEntry *)
		hash_search(EventTriggerProcsCache, (void *)&key, HASH_ENTER, &found);

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
 *  foreach(cell, EventTriggerProcsCache[TrigEventCommand][TrigEvent])
 */
static void
BuildEventTriggerCache(void)
{
	HASHCTL		info;
	Relation	rel, irel;
	IndexScanDesc indexScan;
	HeapTuple	tuple;

	/* build the new hash table */
	MemSet(&info, 0, sizeof(info));
	info.keysize = sizeof(EventTriggerProcsCacheKey);
	info.entrysize = sizeof(EventTriggerProcsCacheEntry);
	info.hash = tag_hash;
	info.hcxt = CacheMemoryContext;

	/* Create the hash table holding our cache */
	EventTriggerProcsCache =
		hash_create("Event Trigger Command Cache",
					1024,
					&info,
					HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

	/* fill in the cache from the catalogs */
	rel = heap_open(EventTriggerRelationId, AccessShareLock);
	irel = index_open(EventTriggerNameIndexId, AccessShareLock);

	indexScan = index_beginscan(rel, irel, SnapshotNow, 0, 0);
	index_rescan(indexScan, NULL, 0, NULL, 0);

	/*
	 * We use a full indexscan to guarantee that we see event triggers ordered
	 * by name. This way, we only even have to append the trigger's function Oid
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

		event = parse_event_name(NameStr(form->evtevent));
		name = form->evtname;
		proc = form->evtfoid;

		adatum = heap_getattr(tuple, Anum_pg_event_trigger_evttags,
							  RelationGetDescr(rel), &isNull);

		if (isNull)
		{
			/* event triggers created without WHEN clause are targetting all
			 * commands (ANY command trigger)
			 */
			add_funcall_to_command_event(event, ETC_ANY, name, proc);
		}
		else
		{
			ArrayType	*arr;
			Datum		*tags;
			int			 i;

			arr = DatumGetArrayTypeP(adatum);		/* ensure not toasted */
			numkeys = ARR_DIMS(arr)[0];

			if (ARR_NDIM(arr) != 1 ||
				numkeys < 0 ||
				ARR_HASNULL(arr) ||
				ARR_ELEMTYPE(arr) != TEXTOID)
				elog(ERROR, "evttags is not a 1-D text array");

			deconstruct_array(arr, TEXTOID, -1, false, 'i',
							  &tags, NULL, &numkeys);

			for (i = 0; i < numkeys; i++)
			{
				char *cmdstr = TextDatumGetCString(tags[i]);
				command = parse_event_tag(cmdstr, false);
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
	hash_destroy(EventTriggerProcsCache);
	EventTriggerProcsCache = NULL;
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

	EventTriggerProcsCache = NULL;

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
	EventTriggerProcsCacheKey anykey, cmdkey;
	EventTriggerProcsCacheEntry *any, *cmd;

	triggers->event = event;
	triggers->command = command;
	triggers->procs = NIL;

	/* Find existing cache entry, if any. */
	if (!EventTriggerProcsCache)
		BuildEventTriggerCache();

	/* ANY command triggers */
	memset(&anykey, 0, sizeof(anykey));
	anykey.event = event;
	anykey.command = ETC_ANY;
	any = (EventTriggerProcsCacheEntry *)
		hash_search(EventTriggerProcsCache, (void *)&anykey, HASH_FIND, NULL);

	/* Specific command triggers */
	memset(&cmdkey, 0, sizeof(cmdkey));
	cmdkey.event = event;
	cmdkey.command = command;
	cmd = (EventTriggerProcsCacheEntry *)
		hash_search(EventTriggerProcsCache, (void *)&cmdkey, HASH_FIND, NULL);

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
		char *current_any_name = NULL, *current_cmd_name;

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

			/*
			 * now append as many elements from CMD list named before next ANY
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
				   && (current_any_name == NULL
					   || strcmp(current_cmd_name, current_any_name) < 0));
		}
		while( lc_cmd_names != NULL && lc_any_names != NULL );
	}
	return triggers;
}

char *
event_to_string(TrigEvent event)
{
	switch (event)
	{
		case EVT_CommandStart:
			return "command_start";
	}
	return NULL;
}

TrigEvent
parse_event_name(char *event)
{
	if (pg_strcasecmp(event, "command_start") == 0)
		return EVT_CommandStart;
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unrecognized event \"%s\"", event)));

	/* make compiler happy */
	return -1;
}

TrigEventCommand
parse_event_tag(char *cmdtag, bool noerror)
{
	char *uctag;
	NameData key;
	EventTriggerCommandTagsEntry *entry;

	if (EventTriggerCommandTagsCache == NULL)
	{
		int			index;
		HASHCTL		info;
		MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

		/* build the new hash table */
		MemSet(&info, 0, sizeof(info));
		info.keysize = NAMEDATALEN;
		info.entrysize = sizeof(EventTriggerCommandTagsEntry);
		info.hash = tag_hash;
		info.hcxt = CacheMemoryContext;

		/* Create the hash table holding our cache */
		EventTriggerCommandTagsCache =
			hash_create("Event Trigger Command Tags Cache",
						1024,
						&info,
						HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

		for (index = 0; index < lengthof(EventTriggerCommandTags); index++)
		{
			bool found;
			char *tag = EventTriggerCommandTags[index].tag;
			NameData key;
			EventTriggerCommandTagsEntry *hresult;

			memset(&key, 0, NAMEDATALEN);
			key = *(DatumGetName(DirectFunctionCall1(namein,
													 CStringGetDatum(tag))));

			hresult = (EventTriggerCommandTagsEntry *)
				hash_search(EventTriggerCommandTagsCache,
							(void *)&key, HASH_ENTER, &found);

			hresult->command = EventTriggerCommandTags[index].command;
		}
		MemoryContextSwitchTo(old);
	}

	memset(&key, 0, NAMEDATALEN);
	uctag = str_toupper(cmdtag, strlen(cmdtag), DEFAULT_COLLATION_OID);
	key = *(DatumGetName(
				DirectFunctionCall1(namein, CStringGetDatum(uctag))));

	entry = (EventTriggerCommandTagsEntry *)
		hash_search(EventTriggerCommandTagsCache,
					(void *)&key, HASH_FIND, NULL);

	if (entry == NULL)
	{
		if (!noerror)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized command \"%s\"", cmdtag)));
		return 	ETC_UNKNOWN;
	}
	return entry->command;
}

char *
command_to_string(TrigEventCommand command)
{
	int			index;
	for (index = 0; index < lengthof(EventTriggerCommandTags); index++)
	{
		if (command == EventTriggerCommandTags[index].command)
			return EventTriggerCommandTags[index].tag;
	}
	return NULL;
}

/*
 * Cache lookup support for ProcessUtility
 */
TrigEventCommand
get_command_from_nodetag(NodeTag node, ObjectType type, bool noerror)
{
	EventTriggerCommandNodeKey key;
	EventTriggerCommandNodeEntry *entry;

	if (EventTriggerCommandNodeCache == NULL)
	{
		int			index;
		HASHCTL		info;
		MemoryContext old = MemoryContextSwitchTo(CacheMemoryContext);

		/* build the new hash table */
		MemSet(&info, 0, sizeof(info));
		info.keysize = sizeof(EventTriggerCommandNodeKey);
		info.entrysize = sizeof(EventTriggerCommandNodeEntry);
		info.hash = tag_hash;
		info.hcxt = CacheMemoryContext;

		/* Create the hash table holding our cache */
		EventTriggerCommandNodeCache =
			hash_create("Event Trigger Command Node Cache",
						1024,
						&info,
						HASH_ELEM | HASH_FUNCTION | HASH_CONTEXT);

		for (index = 0; index < lengthof(EventTriggerCommandTags); index++)
		{
			bool found;
			EventTriggerCommandNodeKey key;
			EventTriggerCommandNodeEntry *hresult;

			memset(&key, 0, sizeof(key));
			key.node = EventTriggerCommandTags[index].node;
			key.type = EventTriggerCommandTags[index].type;

			hresult = (EventTriggerCommandNodeEntry *)
				hash_search(EventTriggerCommandNodeCache,
							(void *)&key, HASH_ENTER, &found);

			hresult->command = EventTriggerCommandTags[index].command;
		}
		MemoryContextSwitchTo(old);
	}

	memset(&key, 0, sizeof(key));
	key.node = node;
	key.type = type;

	entry = (EventTriggerCommandNodeEntry *)
		hash_search(EventTriggerCommandNodeCache,
					(void *)&key, HASH_FIND, NULL);

	if (entry == NULL)
	{
		if (!noerror)
			/* fixme: should not happen, use elog? */
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized node %d and object %d", node, type)));
		return 	ETC_UNKNOWN;
	}
	return entry->command;
}
