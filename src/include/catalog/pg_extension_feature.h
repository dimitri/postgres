/*-------------------------------------------------------------------------
 *
 * pg_extension_feature.h
 *	  definition of the system "extension feature" relation
 *	  (pg_extension_features), that traks what features an extension provides
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_extension_feature.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EXTENSION_FEATURE_H
#define PG_EXTENSION_FEATURE_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_extension_feature definition.  cpp turns this into
 *		typedef struct FormData_pg_extension_feature
 * ----------------
 */
#define ExtensionFeatureRelationId 3179

CATALOG(pg_extension_feature,3179)
{
	Oid			extoid;		/* extension Oid */
	NameData	extfeature;		/* extension feature */
} FormData_pg_extension_feature;

/* ----------------
 *		Form_pg_extension_feature corresponds to a pointer to a tuple with the
 *		format of pg_extension_feature relation.
 * ----------------
 */
typedef FormData_pg_extension_feature *Form_pg_extension_feature;
/* ----------------
 *		compiler constants for pg_extension_feature
 * ----------------
 */

#define Natts_pg_extension_feature				2
#define Anum_pg_extension_feature_extoid		1
#define Anum_pg_extension_feature_extfeature	2

/* ----------------
 *		pg_extension_feature has no initial contents
 * ----------------
 */

#endif   /* PG_EXTENSION_FEATURE_H */
