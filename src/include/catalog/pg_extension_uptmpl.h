/*-------------------------------------------------------------------------
 *
 * pg_extension_uptmpl.h
 *	  definition of the system "extension_uptmpl" relation
 *    (pg_extension_uptmpl) along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_extension_uptmpl.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EXTENSION_UPTMPL_H
#define PG_EXTENSION_UPTMPL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_extension_install_uptmpl definition.  cpp turns this into
 *		typedef struct FormData_pg_extension_uptmpl
 * ----------------
 */
#define ExtensionUpTmplRelationId 3279

CATALOG(pg_extension_uptmpl,3279)
{
	NameData	uptname;		/* extension name */
	Oid			uptowner;		/* template owner */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		uptfrom;		/* version this template updates from */
	text		uptto;			/* version this template updated to */
	text		uptscript;		/* extension's update script */
#endif
} FormData_pg_extension_uptmpl;

/* ----------------
 *		Form_pg_extension_uptmpl corresponds to a pointer to a tuple with the
 *		format of pg_extension_uptmpl relation.
 * ----------------
 */
typedef FormData_pg_extension_uptmpl *Form_pg_extension_uptmpl;

/* ----------------
 *		compiler constants for pg_extension_uptmpl
 * ----------------
 */

#define Natts_pg_extension_uptmpl			5
#define Anum_pg_extension_uptmpl_uptname	1
#define Anum_pg_extension_uptmpl_uptowner	2
#define Anum_pg_extension_uptmpl_uptfrom	3
#define Anum_pg_extension_uptmpl_uptto		3
#define Anum_pg_extension_uptmpl_uptscript	4

/* ----------------
 *		pg_extension_uptmpl has no initial contents
 * ----------------
 */

#endif   /* PG_EXTENSION_UPTMPL_H */
