/*-------------------------------------------------------------------------
 *
 * cmdtrigger.h
 *	  Declarations for command trigger handling.
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/cmdtrigger.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CMDTRIGGER_H
#define CMDTRIGGER_H

#include "nodes/execnodes.h"
#include "nodes/parsenodes.h"

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

extern Oid CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString);
extern void DropCmdTrigger(DropCmdTrigStmt *stmt);
extern void RemoveCmdTriggerById(Oid ctrigOid);
extern Oid	get_cmdtrigger_oid(const char *trigname, const char *name, bool missing_ok);

bool ExecBeforeCommandTriggers(Node *parsetree, const char *command);
int ExecInsteadOfCommandTriggers(Node *parsetree, const char *command);
void ExecAfterCommandTriggers(Node *parsetree, const char *command);

#endif   /* TRIGGER_H */
