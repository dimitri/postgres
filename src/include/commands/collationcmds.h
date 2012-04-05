/*-------------------------------------------------------------------------
 *
 * collationcmds.h
 *	  prototypes for collationcmds.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/collationcmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef COLLATIONCMDS_H
#define COLLATIONCMDS_H

#include "commands/event_trigger.h"
#include "nodes/parsenodes.h"

extern void DefineCollation(List *names, List *parameters, EventContext evt);
extern void RenameCollation(List *name, const char *newname, EventContext evt);
extern void AlterCollationOwner(List *name, Oid newOwnerId, EventContext evt);
extern void AlterCollationOwner_oid(Oid collationOid, Oid newOwnerId, EventContext evt);
extern void AlterCollationNamespace(List *name, const char *newschema,
										EventContext evt);
extern Oid	AlterCollationNamespace_oid(Oid collOid, Oid newNspOid,
											EventContext evt);

#endif   /* COLLATIONCMDS_H */
