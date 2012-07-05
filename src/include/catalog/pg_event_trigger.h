/*-------------------------------------------------------------------------
 *
 * pg_event_trigger.h
 *	  definition of the system "event trigger" relation (pg_event_trigger)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_event_trigger.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EVENT_TRIGGER_H
#define PG_EVENT_TRIGGER_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_event_trigger definition.	cpp turns this into
 *		typedef struct FormData_pg_event_trigger
 * ----------------
 */
#define EventTriggerRelationId  3466

CATALOG(pg_event_trigger,3466)
{
	NameData	evtname;		/* trigger's name */
	NameData	evtevent;		/* trigger's event */
	Oid			evtowner;		/* trigger's owner */
	Oid			evtfoid;		/* OID of function to be called */
	char		evtenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */
#ifdef CATALOG_VARLEN
	text        evttags[1];		/* command TAGs this event trigger targets */
#endif
} FormData_pg_event_trigger;

/* ----------------
 *		Form_pg_event_trigger corresponds to a pointer to a tuple with
 *		the format of pg_event_trigger relation.
 * ----------------
 */
typedef FormData_pg_event_trigger *Form_pg_event_trigger;

/* ----------------
 *		compiler constants for pg_event_trigger
 * ----------------
 */
#define Natts_pg_event_trigger					6
#define Anum_pg_event_trigger_evtname			1
#define Anum_pg_event_trigger_evtevent			2
#define Anum_pg_event_trigger_evtowner			3
#define Anum_pg_event_trigger_evtfoid			4
#define Anum_pg_event_trigger_evtenabled		5
#define Anum_pg_event_trigger_evttags			6

/*
 * Times at which an event trigger can be fired. These are the
 * possible values for pg_event_trigger.evtevent.
 *
 * As of now we only implement the command_start firing point, we intend on
 * adding more firing points later.
 */
typedef enum TrigEvent
{
	E_CommandStart       = 1,
} TrigEvent;

/*
 * Supported commands
 */
typedef enum TrigEventCommand
{
	E_UNKNOWN = 0,
	E_ANY = 1,

	E_AlterAggregate   = 100,
	E_AlterCast,
	E_AlterCollation,
	E_AlterConversion,
	E_AlterDomain,
	E_AlterExtension,
	E_AlterForeignDataWrapper,
	E_AlterForeignTable,
	E_AlterFunction,
	E_AlterIndex,
	E_AlterLanguage,
	E_AlterOperator,
	E_AlterOperatorClass,
	E_AlterOperatorFamily,
	E_AlterSchema,
	E_AlterSequence,
	E_AlterServer,
	E_AlterTable,
	E_AlterTextSearchParser,
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
	E_CreateTextSearchParser,
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
	E_DropTextSearchParser,
	E_DropTextSearchConfiguration,
	E_DropTextSearchDictionary,
	E_DropTextSearchTemplate,
	E_DropTrigger,
	E_DropType,
	E_DropUserMapping,
	E_DropView
} TrigEventCommand;

/* implemented in src/backend/catalog/pg_event_trigger.c */
char * event_to_string(TrigEvent event);
TrigEvent parse_event_name(char *event);

char * command_to_string(TrigEventCommand command);
TrigEventCommand parse_event_tag(char *command, bool noerror);

#endif   /* PG_EVENT_TRIGGER_H */
