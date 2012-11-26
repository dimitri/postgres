/*-------------------------------------------------------------------------
 *
 * ddl_rewrite.h
 *	  Rewrite PostgreSQL DDL commands, from transformed parsetree.
 *
 * That facility is exposed to the user in the Event Trigger support, where the
 * user defined function can use the TG_COMMAND magic variable.
 *
 * Note: we expect other internal modules to get to use the facility here,
 * namely the Bi-Directional Replication project.
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/ddl_rewrite.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef DDL_REWRITE_H
#define DDL_REWRITE_H

extern void get_event_trigger_data(EventTriggerData *trigdata);

#endif DDL_REWRITE_H
