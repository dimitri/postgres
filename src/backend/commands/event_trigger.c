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
#include "utils/evtcache.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/tqual.h"
#include "utils/syscache.h"
#include "tcop/utility.h"

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
 * Insert the new pg_event_trigger row, and return the OID assigned to the new
 * row.
 */
static Oid
InsertEventTriggerTuple(char *trigname, TrigEvent event,
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
			if (cmd == E_UNKNOWN || cmdstr == NULL)
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
									  funcoid, stmt->cmdlist);

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
 * ALTER EVENT TRIGGER foo ON COMMAND ... ENABLE|DISABLE|ENABLE ALWAYS|REPLICA
 */
void
AlterEventTrigger(AlterEventTrigStmt *stmt)
{
	Relation	tgrel;
	HeapTuple	tup;
	Form_pg_event_trigger evtForm;
	char        tgenabled = stmt->tgenabled;

	CheckEventTriggerPrivileges();

	tgrel = heap_open(EventTriggerRelationId, RowExclusiveLock);

	tup = SearchSysCacheCopy1(EVENTTRIGGERNAME, CStringGetDatum(stmt->trigname));
	if (!HeapTupleIsValid(tup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("event trigger \"%s\" does not exist", stmt->trigname)));

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
 * Rename command trigger
 */
void
RenameEventTrigger(const char *trigname, const char *newname)
{
	HeapTuple	tup;
	Relation	rel;
	Form_pg_event_trigger evtForm;

	CheckEventTriggerPrivileges();

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

	evtForm = (Form_pg_event_trigger) GETSTRUCT(tup);

	/* tuple is a copy, so we can rename it now */
	namestrcpy(&(evtForm->evtname), newname);
	simple_heap_update(rel, &tup->t_self, tup);
	CatalogUpdateIndexes(rel, tup);

	heap_freetuple(tup);
	heap_close(rel, RowExclusiveLock);
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
	trigdata.when       = pstrdup(event_to_string(tev));

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
 * InitEventContext() must have been called first. When
 * CommandFiresTriggersForEvent() returns false, the EventContext structure
 * needs not be initialized further.
 */
bool
CommandFiresTriggersForEvent(EventContext ev_ctx, TrigEvent tev)
{
	EventCommandTriggers *triggers;

	if (ev_ctx == NULL || ev_ctx->command == E_UNKNOWN)
		return false;

	triggers = get_event_triggers(tev, ev_ctx->command);

	return triggers->procs != NIL;
}

/*
 * Actually run command triggers of a specific command. We first run ANY
 * command triggers.
 */
void
ExecEventTriggers(EventContext ev_ctx, TrigEvent tev)
{
	EventCommandTriggers *triggers;
	ListCell *lc;

	if (ev_ctx == NULL || ev_ctx->command == E_UNKNOWN)
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
