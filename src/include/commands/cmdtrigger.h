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

#include "nodes/parsenodes.h"

/*
 * Command Trigger Procedure are passed 4 arguments which are maintained in a
 * global (bachend private) variable, command_context. That allows each command
 * implementation to fill in the context then call the Exec...CommandTriggers
 * API functions.
 */
typedef struct CommandContextData
{
	char		*tag;			/* Command Tag */
	Oid			 objectId;		/* oid of the existing object, if any */
	char		*schemaname;	/* schemaname or NULL if not relevant */
	char		*objectname;	/* objectname */
	Node		*parsetree;		/* command parsetree, given as an internal */
	List		*before;		/* procedures to call before the command */
	List		*after;			/* procedures to call after the command */
	List		*before_any;	/* procedures to call before any command */
	List		*after_any;		/* procedures to call after any command */
	MemoryContext oldmctx;	/* Memory Context to switch back to */
	MemoryContext cmdmctx;	/* Memory Context for the command triggers */
} CommandContextData;

typedef struct CommandContextData *CommandContext;

/*
 * CommandTriggerData is the node type that is passed as fmgr "context" info
 * when a function is called by the command trigger manager.
 */
#define CALLED_AS_COMMAND_TRIGGER(fcinfo) \
	((fcinfo)->context != NULL && IsA((fcinfo)->context, CommandTriggerData))

typedef struct CommandTriggerData
{
	NodeTag		 type;
	char        *when;			/* Either BEFORE or AFTER */
	char		*tag;			/* Command Tag */
	Oid			 objectId;		/* oid of the existing object, if any */
	char		*schemaname;	/* schemaname or NULL if not relevant */
	char		*objectname;	/* objectname */
	Node		*parsetree;		/* command parsetree, given as an internal */
} CommandTriggerData;

extern Oid CreateCmdTrigger(CreateCmdTrigStmt *stmt, const char *queryString);
extern void RemoveCmdTriggerById(Oid ctrigOid);
extern Oid	get_cmdtrigger_oid(const char *trigname, bool missing_ok);
extern void AlterCmdTrigger(AlterCmdTrigStmt *stmt);
extern void RenameCmdTrigger(List *name, const char *newname);

extern void InitCommandContext(CommandContext cmd, const Node *stmt);
extern bool CommandFiresTriggers(CommandContext cmd);
extern bool CommandFiresAfterTriggers(CommandContext cmd);
extern void ExecBeforeCommandTriggers(CommandContext cmd);
extern void ExecBeforeAnyCommandTriggers(CommandContext cmd);
extern void ExecAfterCommandTriggers(CommandContext cmd);
extern void ExecAfterAnyCommandTriggers(CommandContext cmd);

#endif   /* CMD_TRIGGER_H */
