/*-------------------------------------------------------------------------
 *
 * createas.h
 *	  prototypes for createas.c.
 *
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/createas.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CREATEAS_H
#define CREATEAS_H

#include "commands/cmdtrigger.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "tcop/dest.h"


extern void ExecCreateTableAs(CreateTableAsStmt *stmt, const char *queryString,
							  ParamListInfo params, char *completionTag);

extern int	GetIntoRelEFlags(IntoClause *intoClause);

extern DestReceiver *CreateIntoRelDestReceiver(IntoClause *intoClause,
											   CommandContext cmd);

#endif   /* CREATEAS_H */
