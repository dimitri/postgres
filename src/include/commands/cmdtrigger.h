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
 * Command Trigger Procedure are passed 4 arguments which are maintained in a
 * global (bachend private) variable, command_context. That allows each command
 * implementation to fill in the context then call the Exec...CommandTriggers
 * API functions.
 */
typedef struct CommandContextData
{
	char *tag;					/* Command Tag */
	Oid   objectId;				/* oid of the existing object, if any */
	char *schemaname;			/* schemaname or NULL if not relevant */
	char *objectname;			/* objectname */
	Node *parsetree;			/* command parsetree, given as an internal */
} CommandContextData;

typedef struct CommandContextData *CommandContext;

extern CommandContext command_context;

extern void CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString);
extern void DropCmdTrigger(DropCmdTrigStmt *stmt);
extern void RemoveCmdTriggerById(Oid ctrigOid);
extern Oid	get_cmdtrigger_oid(const char *command, const char *trigname, bool missing_ok);
extern void AlterCmdTrigger(AlterCmdTrigStmt *stmt);
extern void RenameCmdTrigger(List *command, const char *trigname, const char *newname);

extern bool ExecBeforeOrInsteadOfCommandTriggers(CommandContext cmd);
extern bool ExecBeforeOrInsteadOfAnyCommandTriggers(CommandContext cmd);
extern void ExecAfterCommandTriggers(CommandContext cmd);
extern void ExecAfterAnyCommandTriggers(CommandContext cmd);

#endif   /* CMD_TRIGGER_H */