/*-------------------------------------------------------------------------
 *
 * event_trigger.h
 *	  Declarations for command trigger handling.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/event_trigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EVENT_TRIGGER_H
#define EVENT_TRIGGER_H

#include "catalog/dependency.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_event_trigger.h"
#include "nodes/parsenodes.h"

/*
 * Global objects that we need to keep track of for benefits of Event Triggers.
 *
 * The EventTriggerSQLDropList is a list of ObjectAddress filled in from
 * dependency.c doDeletion() function. Only objects that are supported as in
 * EventTriggerSupportsObjectType() get appended here. ProcessUtility is
 * responsible for resetting this list to NIL at the beginning of any DROP
 * operation.
 */
extern bool EventTriggerSQLDropInProgress;
extern ObjectAddresses *EventTriggerSQLDropList;

typedef struct EventTriggerData
{
	NodeTag		type;
	char	   *event;				/* event name */
	Node	   *parsetree;			/* parse tree */
	const char *tag;				/* command tag */
} EventTriggerData;

/*
 * EventTriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the event trigger manager.
 */
#define CALLED_AS_EVENT_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, EventTriggerData))

extern Oid CreateEventTrigger(CreateEventTrigStmt *stmt);
extern void RemoveEventTriggerById(Oid ctrigOid);
extern Oid	get_event_trigger_oid(const char *trigname, bool missing_ok);

extern Oid AlterEventTrigger(AlterEventTrigStmt *stmt);
extern Oid AlterEventTriggerOwner(const char *name, Oid newOwnerId);
extern void AlterEventTriggerOwner_oid(Oid, Oid newOwnerId);

extern bool EventTriggerSupportsObjectType(ObjectType obtype);
extern void EventTriggerDDLCommandStart(Node *parsetree);
extern void EventTriggerDDLCommandEnd(Node *parsetree);

extern void EventTriggerInitDropList(void);
extern List *EventTriggerAppendToDropList(ObjectAddress *object);
extern void EventTriggerSQLDrop(Node *parsetree);

extern void AtEOXact_EventTrigger(bool isCommit);


#endif   /* EVENT_TRIGGER_H */
