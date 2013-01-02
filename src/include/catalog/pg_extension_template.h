/*-------------------------------------------------------------------------
 *
 * pg_extension_template.h
 *	  definition of the system "extension_template" relation
 *    (pg_extension_template) along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_extension_template.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EXTENSION_TEMPLATE_H
#define PG_EXTENSION_TEMPLATE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_extension_install_template definition.  cpp turns this into
 *		typedef struct FormData_pg_extension_template
 * ----------------
 */
#define ExtensionTemplateRelationId 3179

CATALOG(pg_extension_template,3179)
{
	NameData	tplname;		/* extension name */
	Oid			tplowner;		/* template owner */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		tplversion;		/* version to install with this template */
	text		tplscript;		/* extension's install script */
#endif
} FormData_pg_extension_template;

/* ----------------
 *		Form_pg_extension_template corresponds to a pointer to a tuple with the
 *		format of pg_extension_template relation.
 * ----------------
 */
typedef FormData_pg_extension_template *Form_pg_extension_template;

/* ----------------
 *		compiler constants for pg_extension_template
 * ----------------
 */

#define Natts_pg_extension_template				4
#define Anum_pg_extension_template_tplname		1
#define Anum_pg_extension_template_tplowner		2
#define Anum_pg_extension_template_tplversion	3
#define Anum_pg_extension_template_tplscript	4

/* ----------------
 *		pg_extension_template has no initial contents
 * ----------------
 */

#endif   /* PG_EXTENSION_TEMPLATE_H */
