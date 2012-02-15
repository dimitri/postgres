/*-------------------------------------------------------------------------
 *
 * typecmds.h
 *	  prototypes for typecmds.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/typecmds.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TYPECMDS_H
#define TYPECMDS_H

#include "commands/cmdtrigger.h"
#include "utils/lsyscache.h"
#include "nodes/parsenodes.h"


#define DEFAULT_TYPDELIM		','

extern void DefineType(List *names, List *parameters, CommandContext cmd);
extern void RemoveTypeById(Oid typeOid);
extern void DefineDomain(CreateDomainStmt *stmt);
extern void DefineEnum(CreateEnumStmt *stmt);
extern void DefineRange(CreateRangeStmt *stmt);
extern void AlterEnum(AlterEnumStmt *stmt);
extern Oid	DefineCompositeType(const RangeVar *typevar, List *coldeflist, CommandContext cmd);
extern Oid	AssignTypeArrayOid(void);

extern void AlterDomainDefault(List *names, Node *defaultRaw);
extern void AlterDomainNotNull(List *names, bool notNull);
extern void AlterDomainAddConstraint(List *names, Node *constr);
extern void AlterDomainValidateConstraint(List *names, char *constrName);
extern void AlterDomainDropConstraint(List *names, const char *constrName,
									  DropBehavior behavior, bool missing_ok);

extern List *GetDomainConstraints(Oid typeOid);

extern void RenameType(RenameStmt *stmt, CommandContext cmd);
extern void AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype,
							   CommandContext cmd);
extern void AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId,
					   bool hasDependEntry);
extern void AlterTypeNamespace(List *names, const char *newschema,
								   ObjectType objecttype, CommandContext cmd);
extern Oid	AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, CommandContext cmd);
extern Oid AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool errorOnTableType,
						   CommandContext cmd);

#endif   /* TYPECMDS_H */
