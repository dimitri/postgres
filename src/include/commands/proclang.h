/*
 * src/include/commands/proclang.h
 *
 *-------------------------------------------------------------------------
 *
 * proclang.h
 *	  prototypes for proclang.c.
 *
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCLANG_H
#define PROCLANG_H

#include "commands/event_trigger.h"
#include "nodes/parsenodes.h"

extern void CreateProceduralLanguage(CreatePLangStmt *stmt);
extern void DropProceduralLanguageById(Oid langOid);
extern void RenameLanguage(const char *oldname, const char *newname, EventContext evt);
extern void AlterLanguageOwner(const char *name, Oid newOwnerId, EventContext evt);
extern void AlterLanguageOwner_oid(Oid oid, Oid newOwnerId);
extern bool PLTemplateExists(const char *languageName);
extern Oid	get_language_oid(const char *langname, bool missing_ok);

#endif   /* PROCLANG_H */
