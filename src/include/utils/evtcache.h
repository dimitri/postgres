/*-------------------------------------------------------------------------
 *
 * evtcache.h
 *	  Attribute options cache.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/evtcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVTCACHE_H
#define EVTCACHE_H

#include "catalog/pg_event_trigger.h"


/*
 * Times at which an event trigger can be fired. These are the
 * possible values for pg_event_trigger.evtevent.
 *
 * As of now we only implement the command_start firing point, we intend on
 * adding more firing points later.
 */
typedef enum TrigEvent
{
	EVT_CommandStart       = 1,
} TrigEvent;

/*
 * Supported commands
 *
 * Those values are not going to disk, so can be shuffled around at will. We
 * keep the number organized so that it's easier to debug, should it be needed.
 *
 * See also EventTriggerCommandTags in src/backend/utils/cache/evtcache.c
 */
typedef enum TrigEventCommand
{
	ETC_UNSET = -1,
	ETC_UNKNOWN = 0,
	ETC_ANY = 1,

	ETC_AlterAggregate   = 100,
	ETC_AlterCast,
	ETC_AlterCollation,
	ETC_AlterConversion,
	ETC_AlterDomain,
	ETC_AlterExtension,
	ETC_AlterForeignDataWrapper,
	ETC_AlterForeignTable,
	ETC_AlterFunction,
	ETC_AlterIndex,
	ETC_AlterLanguage,
	ETC_AlterOperator,
	ETC_AlterOperatorClass,
	ETC_AlterOperatorFamily,
	ETC_AlterRule,
	ETC_AlterSchema,
	ETC_AlterSequence,
	ETC_AlterServer,
	ETC_AlterTable,
	ETC_AlterTextSearchParser,
	ETC_AlterTextSearchConfiguration,
	ETC_AlterTextSearchDictionary,
	ETC_AlterTextSearchTemplate,
	ETC_AlterTrigger,
	ETC_AlterType,
	ETC_AlterUserMapping,
	ETC_AlterView,

	ETC_Cluster = 300,
	ETC_Load,
	ETC_Reindex,
	ETC_SelectInto,
	ETC_Vacuum,

	ETC_CreateAggregate = 400,
	ETC_CreateCast,
	ETC_CreateCollation,
	ETC_CreateConversion,
	ETC_CreateDomain,
	ETC_CreateExtension,
	ETC_CreateForeignDataWrapper,
	ETC_CreateForeignTable,
	ETC_CreateFunction,
	ETC_CreateIndex,
	ETC_CreateLanguage,
	ETC_CreateOperator,
	ETC_CreateOperatorClass,
	ETC_CreateOperatorFamily,
	ETC_CreateRule,
	ETC_CreateSchema,
	ETC_CreateSequence,
	ETC_CreateServer,
	ETC_CreateTable,
	ETC_CreateTableAs,
	ETC_CreateTextSearchParser,
	ETC_CreateTextSearchConfiguration,
	ETC_CreateTextSearchDictionary,
	ETC_CreateTextSearchTemplate,
	ETC_CreateTrigger,
	ETC_CreateType,
	ETC_CreateUserMapping,
	ETC_CreateView,

	ETC_DropAggregate = 600,
	ETC_DropCast,
	ETC_DropCollation,
	ETC_DropConversion,
	ETC_DropDomain,
	ETC_DropExtension,
	ETC_DropForeignDataWrapper,
	ETC_DropForeignTable,
	ETC_DropFunction,
	ETC_DropIndex,
	ETC_DropLanguage,
	ETC_DropOperator,
	ETC_DropOperatorClass,
	ETC_DropOperatorFamily,
	ETC_DropRule,
	ETC_DropSchema,
	ETC_DropSequence,
	ETC_DropServer,
	ETC_DropTable,
	ETC_DropTextSearchParser,
	ETC_DropTextSearchConfiguration,
	ETC_DropTextSearchDictionary,
	ETC_DropTextSearchTemplate,
	ETC_DropTrigger,
	ETC_DropType,
	ETC_DropUserMapping,
	ETC_DropView
} TrigEventCommand;

/*
 * Event Triggers to fire for a given event and command, including ANY command
 * triggers.
 */
typedef struct EventCommandTriggers
{
	TrigEvent			event;
	TrigEventCommand	command;
	List               *procs;
} EventCommandTriggers;

void InitEventTriggerCache(void);
EventCommandTriggers *get_event_triggers(TrigEvent event,
										 TrigEventCommand command);

char * event_to_string(TrigEvent event);
TrigEvent parse_event_name(char *event);

char * command_to_string(TrigEventCommand command);
TrigEventCommand parse_event_tag(char *cmdtag, bool noerror);

TrigEventCommand get_command_from_nodetag(NodeTag node,
										  ObjectType type,
										  bool noerror);

char * objecttype_to_string(ObjectType type);

#endif   /* EVTCACHE_H */
