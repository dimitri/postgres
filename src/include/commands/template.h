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
extern Oid CreateUpdateTemplate(CreateTemplateStmt *stmt);

extern Oid get_template_oid(const char *extname, const char *version,
							bool missing_ok);
extern bool can_create_extension_from_template(const char *extname,
											   bool missing_ok);
extern Oid get_uptmpl_oid(const char *extname,
						  const char *from, const char *to,
						  bool missing_ok);


#endif   /* TEMPLATE_H */
