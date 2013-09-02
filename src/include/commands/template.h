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
#include "extension.h"

extern Oid CreateTemplate(CreateExtTemplateStmt *stmt);
extern Oid CreateExtensionTemplate(CreateExtTemplateStmt *stmt);
extern Oid CreateExtensionUpdateTemplate(CreateExtTemplateStmt *stmt);

extern char *get_extension_control_name(Oid ctrlOid);
extern char *get_extension_template_name(Oid tmplOid);
extern char *get_extension_uptmpl_name(Oid tmplOid);

extern Oid AlterExtensionTemplateOwner(const char *extname, Oid newowner);
extern Oid AlterExtensionTemplateRename(const char *extname,
										const char *newname);

extern Oid AlterTemplate(AlterExtTemplateStmt *stmt);
extern Oid AlterExtensionTemplate(AlterExtTemplateStmt *stmt);
extern Oid AlterExtensionUpdateTemplate(AlterExtTemplateStmt *stmt);

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

extern ExtensionControl *find_pg_extension_control(const char *extname,
												   const char *version,
												   bool missing_ok);

extern ExtensionControl *find_default_pg_extension_control(const char *extname,
														   bool missing_ok);

extern char *read_pg_extension_template_script(const char *extname,
											   const char *version);
extern char *read_pg_extension_uptmpl_script(const char *extname,
											 const char *from_version,
											 const char *version);
extern char *read_extension_template_script(const char *extname,
											const char *from_version,
											const char *version);

extern List *list_pg_extension_template_versions(const char *extname);
extern List *list_pg_extension_update_versions(const char *extname);
extern List *pg_extension_default_controls(void);
extern List *pg_extension_controls(void);
extern List *pg_extension_templates(void);

extern List *list_pg_extension_control_oids_for(const char *extname);
extern List *list_pg_extension_template_oids_for(const char *extname);
extern List *list_pg_extension_uptmpl_oids_for(const char *extname);

#endif   /* TEMPLATE_H */
