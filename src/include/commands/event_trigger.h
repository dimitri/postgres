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
 * Several steps of event trigger processing need to see through the command
 * tag in order to decide what to do next. The first part of the command tag is
 * CREATE, ALTER, DROP or some other specialized command, and the second part
 * is the name of the object kind we're dealing with.
 */
typedef enum {
	COMMAND_TAG_ALTER,
	COMMAND_TAG_CREATE,
	COMMAND_TAG_DROP,
	COMMAND_TAG_OTHER
} CommandTagOperation;

/*
 * This structure is meant to ease access to the object type's name and
 * operation, not as something to edit on the fly. See function
 * split_command_tag which should be the only thing editing those pieces of
 * information.
 */
typedef struct {
	const char			*tag;
	const char			*opname;
	CommandTagOperation	 operation;
	const char			*obtypename;
} CommandTag;


typedef struct EventTriggerData
{
	NodeTag		type;
	char	   *event;				/* event name */
	Node	   *parsetree;			/* parse tree */
	CommandTag *ctag;				/* command tag */
	char	   *schemaname;			/* schema name of the object */
	char	   *objectname;			/* object name */
	char	   *command;			/* deparsed command string */
} EventTriggerData;

/*
 * EventTriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the event trigger manager.
 */
#define CALLED_AS_EVENT_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, EventTriggerData))

extern void CreateEventTrigger(CreateEventTrigStmt *stmt);
extern void RemoveEventTriggerById(Oid ctrigOid);
extern Oid	get_event_trigger_oid(const char *trigname, bool missing_ok);

extern void AlterEventTrigger(AlterEventTrigStmt *stmt);
extern Oid RenameEventTrigger(const char* trigname, const char *newname);
extern Oid AlterEventTriggerOwner(const char *name, Oid newOwnerId);
extern void AlterEventTriggerOwner_oid(Oid, Oid newOwnerId);

extern bool EventTriggerSupportsObjectType(ObjectType obtype);
extern void EventTriggerDDLCommandStart(bool isCompleteQuery, Node *parsetree);
extern void EventTriggerDDLCommandEnd(bool isCompleteQuery, Node *parsetree);

#endif   /* EVENT_TRIGGER_H */
