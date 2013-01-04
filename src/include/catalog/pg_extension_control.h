/*-------------------------------------------------------------------------
 *
 * pg_extension_control.h
 *	  definition of the system "extension_control" relation
 *    (pg_extension_control) along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_extension_control.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EXTENSION_CONTROL_H
#define PG_EXTENSION_CONTROL_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_extension_install_control definition.  cpp turns this into
 *		typedef struct FormData_pg_extension_control
 * ----------------
 */
#define ExtensionControlRelationId 3379

CATALOG(pg_extension_control,3379)
{
	NameData	ctlname;		/* extension name */
	Oid			ctlowner;		/* control owner */
	bool		ctldefault;		/* this version is the extension's default? */
	bool		ctlrelocatable;	/* extension is relocatable? */
	bool		ctlsuperuser;	/* extension is superuser only? */
	NameData	ctlnamespace;	/* namespace of contained objects */

#ifdef CATALOG_VARLEN			/* variable-length fields start here */
	text		ctlversion;		/* version to install with this control */
	text 		ctlrequires;	/* extension dependency list */
#endif
} FormData_pg_extension_control;

/* ----------------
 *		Form_pg_extension_control corresponds to a pointer to a tuple with the
 *		format of pg_extension_control relation.
 * ----------------
 */
typedef FormData_pg_extension_control *Form_pg_extension_control;

/* ----------------
 *		compiler constants for pg_extension_control
 * ----------------
 */

#define Natts_pg_extension_control					8
#define Anum_pg_extension_control_ctlname			1
#define Anum_pg_extension_control_ctlowner			2
#define Anum_pg_extension_control_ctldefault		3
#define Anum_pg_extension_control_ctlrelocatable	4
#define Anum_pg_extension_control_ctlsuperuser		5
#define Anum_pg_extension_control_ctlnamespace		6
#define Anum_pg_extension_control_ctlversion		7
#define Anum_pg_extension_control_ctlrequires		8

/* ----------------
 *		pg_extension_control has no initial contents
 * ----------------
 */

#endif   /* PG_EXTENSION_CONTROL_H */
