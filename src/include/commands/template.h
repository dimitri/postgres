/*-------------------------------------------------------------------------
 *
 * template.h
 *		Template management commands (create/drop template).
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/template.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TEMPLATE_H
#define TEMPLATE_H

#include "nodes/parsenodes.h"

extern Oid CreateTemplate(CreateTemplateStmt *stmt);
extern Oid CreateExtensionTemplate(CreateTemplateStmt *stmt);
extern Oid CreateExtensionUpdateTemplate(CreateTemplateStmt *stmt);

extern Oid AlterTemplate(AlterTemplateStmt *stmt);
extern Oid AlterExtensionTemplate(AlterTemplateStmt *stmt);
extern Oid AlterExtensionUpdateTemplate(AlterTemplateStmt *stmt);

extern Oid get_template_oid(const char *extname, const char *version,
							bool missing_ok);
extern bool can_create_extension_from_template(const char *extname,
											   bool missing_ok);
extern Oid get_uptmpl_oid(const char *extname,
						  const char *from, const char *to,
						  bool missing_ok);

extern void RemoveExtensionControlById(Oid extControlOid);
extern void RemoveExtensionTemplateById(Oid extTemplateOid);
extern void RemoveExtensionUpTmplById(Oid extUpTmplOid);

extern ExtensionControlFile *
find_pg_extension_control(const char *extname, const char *version);

extern ExtensionControlFile *
find_default_pg_extension_control(const char *extname, bool missing_ok);

#endif   /* TEMPLATE_H */
