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


#endif   /* EVTCACHE_H */
