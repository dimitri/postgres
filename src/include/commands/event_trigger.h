/*-------------------------------------------------------------------------
 *
 * event_trigger.h
 *	  Declarations for command trigger handling.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/event_trigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVENT_TRIGGER_H
#define EVENT_TRIGGER_H

#include "catalog/pg_event_trigger.h"
#include "nodes/parsenodes.h"

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
	TrigEventCommand command;	/* For command triggers */
	char		*toplevel;		/* TopLevel Command Tag */
	char		*tag;			/* Command Tag */
	Oid			 objectId;		/* oid of the existing object, if any */
	char		*schemaname;	/* schemaname or NULL if not relevant */
	char		*objectname;	/* objectname */
	Node		*parsetree;		/* command parsetree, given as an internal */
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
} EventTriggerData;

extern Oid CreateEventTrigger(CreateEventTrigStmt *stmt, const char *queryString);
extern void RemoveEventTriggerById(Oid ctrigOid);
extern Oid	get_event_trigger_oid(const char *trigname, bool missing_ok);

extern void AlterEventTrigger(AlterEventTrigStmt *stmt);
extern void RenameEventTrigger(const char* trigname, const char *newname);

extern void InitEventContext(EventContext evt, const Node *stmt);
extern void InitEventContextForCommand(EventContext evt, const Node *stmt,
									   TrigEventCommand command);

extern bool CommandFiresTriggers(EventContext ev_ctx);
extern bool CommandFiresTriggersForEvent(EventContext ev_ctx, TrigEvent tev);
extern void ExecEventTriggers(EventContext ev_ctx, TrigEvent tev);
extern void ExecEventTriggers(EventContext ev_ctx, TrigEvent tev);

#endif   /* EVENT_TRIGGER_H */
