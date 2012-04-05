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

#include "commands/event_trigger.h"
#include "utils/lsyscache.h"
#include "nodes/parsenodes.h"


#define DEFAULT_TYPDELIM		','

extern void DefineType(List *names, List *parameters, EventContext evt);
extern void RemoveTypeById(Oid typeOid);
extern void DefineDomain(CreateDomainStmt *stmt);
extern void DefineEnum(CreateEnumStmt *stmt);
extern void DefineRange(CreateRangeStmt *stmt);
extern void AlterEnum(AlterEnumStmt *stmt);
extern Oid	DefineCompositeType(RangeVar *typevar, List *coldeflist, EventContext evt);
extern Oid	AssignTypeArrayOid(void);

extern void AlterDomainDefault(List *names, Node *defaultRaw, EventContext evt);
extern void AlterDomainNotNull(List *names, bool notNull, EventContext evt);
extern void AlterDomainAddConstraint(List *names, Node *constr, EventContext evt);
extern void AlterDomainValidateConstraint(List *names, char *constrName, EventContext evt);
extern void AlterDomainDropConstraint(List *names, const char *constrName,
									  DropBehavior behavior, bool missing_ok,
										  EventContext evt);

extern List *GetDomainConstraints(Oid typeOid);

extern void RenameType(RenameStmt *stmt, EventContext evt);
extern void AlterTypeOwner(List *names, Oid newOwnerId, ObjectType objecttype,
							   EventContext evt);
extern void AlterTypeOwnerInternal(Oid typeOid, Oid newOwnerId,
					   bool hasDependEntry);
extern void AlterTypeNamespace(List *names, const char *newschema,
								   ObjectType objecttype, EventContext evt);
extern Oid	AlterTypeNamespace_oid(Oid typeOid, Oid nspOid, EventContext evt);
extern Oid AlterTypeNamespaceInternal(Oid typeOid, Oid nspOid,
						   bool isImplicitArray,
						   bool errorOnTableType,
						   EventContext evt);

#endif   /* TYPECMDS_H */
