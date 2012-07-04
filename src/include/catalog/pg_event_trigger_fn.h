/*-------------------------------------------------------------------------
 *
 * pg_event_trigger_fn.h
 *	 prototypes for functions in catalog/pg_event_trigger.c
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_event_trigger_fn.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EVENT_TRIGGER_FN_H
#define PG_EVENT_TRIGGER_FN_H

#include "catalog/pg_event_trigger.h"

TrigEventCommand parse_event_tag(char *command, bool noerror);

#endif   /* PG_EVENT_TRIGGER_FN_H */
