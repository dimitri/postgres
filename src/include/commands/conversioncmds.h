/*-------------------------------------------------------------------------
 *
 * conversioncmds.h
 *	  prototypes for conversioncmds.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/conversioncmds.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef CONVERSIONCMDS_H
#define CONVERSIONCMDS_H

#include "nodes/parsenodes.h"

extern void CreateConversionCommand(CreateConversionStmt *parsetree);
extern void RenameConversion(List *name, const char *newname, CommandContext cmd);
extern void AlterConversionOwner(List *name, Oid newOwnerId, CommandContext cmd);
extern void AlterConversionOwner_oid(Oid conversionOid, Oid newOwnerId, CommandContext cmd);
extern void AlterConversionNamespace(List *name, const char *newschema,
									 CommandContext cmd);
extern Oid	AlterConversionNamespace_oid(Oid convOid, Oid newNspOid);

#endif   /* CONVERSIONCMDS_H */
