/*-------------------------------------------------------------------------
 *
 * pg_cmdtrigger.h
 *	  definition of the system "command trigger" relation (pg_cmdtrigger)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_trigger.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_CMDTRIGGER_H
#define PG_CMDTRIGGER_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_cmdtrigger definition.	cpp turns this into
 *		typedef struct FormData_pg_cmdtrigger
 * ----------------
 */
#define CmdTriggerRelationId  3466

CATALOG(pg_cmdtrigger,3466)
{
	NameData    ctgcommand;		/* trigger's command */
	NameData	ctgname;		/* trigger's name */
	Oid			ctgfoid;		/* OID of function to be called */
	char		ctgtype;		/* BEFORE/AFTER/INSTEAD */
	char		ctgenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */
} FormData_pg_cmdtrigger;

/* ----------------
 *		Form_pg_cmdtrigger corresponds to a pointer to a tuple with
 *		the format of pg_cmdtrigger relation.
 * ----------------
 */
typedef FormData_pg_cmdtrigger *Form_pg_cmdtrigger;

/* ----------------
 *		compiler constants for pg_cmdtrigger
 * ----------------
 */
#define Natts_pg_cmdtrigger					5
#define Anum_pg_cmdtrigger_ctgcommand		1
#define Anum_pg_cmdtrigger_ctgname			2
#define Anum_pg_cmdtrigger_ctgfoid			3
#define Anum_pg_cmdtrigger_ctgtype			4
#define Anum_pg_cmdtrigger_ctgenabled		5

/*
 * Times at which a command trigger can be fired. These are the
 * possible values for pg_cmdtrigger.ctgtype.
 *
 * pg_trigger is using binary mask tricks to make it super fast, but we don't
 * need to be that tricky here: we're talking about commands, not data editing,
 * and we don't have so many conditions, only type and enabled.
 */
#define CMD_TRIGGER_FIRED_BEFORE			'B'
#define CMD_TRIGGER_FIRED_AFTER				'A'
#define CMD_TRIGGER_FIRED_INSTEAD			'I'

#endif   /* PG_CMDTRIGGER_H */
