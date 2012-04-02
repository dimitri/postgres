/*-------------------------------------------------------------------------
 *
 * cmdtrigger.h
 *	  Declarations for command trigger handling.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/cmdtrigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVENT_TRIGGER_H
#define EVENT_TRIGGER_H

#include "nodes/parsenodes.h"

/*
 * Cache the event triggers in a format that's suitable to finding which
 * function to call at "hook" points in the code. The catalogs are not helpful
 * at search time, because we can't both edit a single catalog entry per each
 * command, have a user friendly syntax and find what we need in a single index
 * scan.
 *
 * This cache is indexed by Event Command id (see pg_event_trigger.h) then
 * Event Id. It's containing a list of function oid.
 */
extern List *EventCommandTriggerCache[][];

/*
 * To be able to call user defined function on event triggers, the places in
 * the code that support that have to fill-in an EventContextData structure
 * containing some information about what's happening.
 *
 * Some more information is filled in by InitEventContext(), which will search
 * for functions to run in the catalogs, depending on which event triggers are
 * defined and the event being prepared (command, subcommand, etc).
 */
typedef struct EventContextData
{
	char		*tag;			/* Command Tag */
	Oid			 objectId;		/* oid of the existing object, if any */
	char		*schemaname;	/* schemaname or NULL if not relevant */
	char		*objectname;	/* objectname */
	Node		*parsetree;		/* command parsetree, given as an internal */
	List		*before;		/* procedures to call before the command */
	List		*after;			/* procedures to call after the command */
	List		*before_any;	/* procedures to call before any command */
	List		*after_any;		/* procedures to call after any command */
	MemoryContext oldmctx;	/* Memory Context to switch back to */
	MemoryContext cmdmctx;	/* Memory Context for the command triggers */
} EventContextData;

typedef struct EventContextData *EventContext;

/*
 * CommandTriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the command trigger manager.
 */
#define CALLED_AS_EVENT_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, EventTriggerData))

typedef struct EventTriggerData
{
	NodeTag		 type;
	char        *when;			/* Either BEFORE or AFTER */
	char		*toplevel;		/* TopLevel Command Tag */
	char		*tag;			/* Command Tag */
	Oid			 objectId;		/* oid of the existing object, if any */
	char		*schemaname;	/* schemaname or NULL if not relevant */
	char		*objectname;	/* objectname */
	Node		*parsetree;		/* command parsetree, given as an internal */
} CommandTriggerData;

/*
 * Times at which an event trigger can be fired. These are the
 * possible values for pg_event_trigger.evtevent.
 *
 * We don't use 0 here because of how we manage the backend local caching of
 * event triggers. That cache is an array of commands that contains an array of
 * events that contains a list of function OIDs.
 *
 * To iterate over each function oid to call at a specific event timing for a
 * given command, it's then as easy as:
 *
 *  foreach(cell, EventCommandTriggerCache[TrigEventCommand][TrigEvent])
 *
 * The ordering here is important, in order to be able to refuse some cases
 * (late trigger are not possible in commands implementing their own
 * transaction control behavior, such as CLUSTER or CREATE INDEX CONCURRENTLY.
 */
typedef enum TrigEvent
{
	E_CommandStart       = 1,
	E_SecurityCheck      = 100,
	E_ConsistencyCheck   = 200,
	E_NameLookup         = 300,
	E_CommandEnd         = 500,
} TrigEvent;

/*
 * Supported commands
 */
typedef enum TrigEventCommand
{
	E_ANY = 1,

	E_AlterAggregate   = 100,
	E_AlterCollation,
	E_AlterConversion,
	E_AlterDomain,
	E_AlterExtension,
	E_AlterForeignDataWrapper,
	E_AlterForeignTable,
	E_AlterFunction,
	E_AlterLanguage,
	E_AlterOperator,
	E_AlterOperatorClass,
	E_AlterOperatorFamily,
	E_AlterSchema,
	E_AlterSequence,
	E_AlterServer,
	E_AlterTable,
	E_AlterTextSearcharser,
	E_AlterTextSearchConfiguration,
	E_AlterTextSearchDictionary,
	E_AlterTextSearchTemplate,
	E_AlterTrigger,
	E_AlterType,
	E_AlterUserMapping,
	E_AlterView,

	E_Cluster = 300,
	E_Load,
	E_Reindex,
	E_SelectInto,
	E_Vacuum,

	E_CreateAggregate = 400,
	E_CreateCast,
	E_CreateCollation,
	E_CreateConversion,
	E_CreateDomain,
	E_CreateExtension,
	E_CreateForeignDataWrapper,
	E_CreateForeignTable,
	E_CreateFunction,
	E_CreateIndex,
	E_CreateLanguage,
	E_CreateOperator,
	E_CreateOperatorClass,
	E_CreateOperatorFamily,
	E_CreateRule,
	E_CreateSchema,
	E_CreateSequence,
	E_CreateServer,
	E_CreateTable,
	E_CreateTableAs,
	E_CreateTextSearcharser,
	E_CreateTextSearchConfiguration,
	E_CreateTextSearchDictionary,
	E_CreateTextSearchTemplate,
	E_CreateTrigger,
	E_CreateType,
	E_CreateUserMapping,
	E_CreateView,

	E_DropAggregate = 600,
	E_DropCast,
	E_DropCollation,
	E_DropConversion,
	E_DropDomain,
	E_DropExtension,
	E_DropForeignDataWrapper,
	E_DropForeignTable,
	E_DropFunction,
	E_DropIndex,
	E_DropLanguage,
	E_DropOperator,
	E_DropOperatorClass,
	E_DropOperatorFamily,
	E_DropRule,
	E_DropSchema,
	E_DropSequence,
	E_DropServer,
	E_DropTable,
	E_DropTextSearcharser,
	E_DropTextSearchConfiguration,
	E_DropTextSearchDictionary,
	E_DropTextSearchTemplate,
	E_DropTrigger,
	E_DropType,
	E_DropUserMapping,
	E_DropView
} TrigEventCommand;

extern Oid CreateEventTrigger(CreateEventTrigStmt *stmt, const char *queryString);
extern void RemoveEventTriggerById(Oid ctrigOid);
extern Oid	get_event_trigger_oid(const char *trigname, bool missing_ok);
extern void AlterEventTrigger(AlterEventTrigStmt *stmt);
extern void RenameEventTrigger(List *name, const char *newname);

extern void InitCommandContext(CommandContext cmd, const Node *stmt);
extern bool CommandFiresTriggers(CommandContext cmd);
extern bool CommandFiresAfterTriggers(CommandContext cmd);
extern void ExecBeforeCommandTriggers(CommandContext cmd);
extern void ExecBeforeAnyCommandTriggers(CommandContext cmd);
extern void ExecAfterCommandTriggers(CommandContext cmd);
extern void ExecAfterAnyCommandTriggers(CommandContext cmd);

extern void InitEventContext(EventContext ev_ctx, const Node *stmt);
extern bool CommandFiresTriggers(EventContext ev_ctx, TrigEvent tev);
extern void ExecEventTriggers(EventContext ev_ctx, TrigEvent tev);

#endif   /* EVENT_TRIGGER_H */
