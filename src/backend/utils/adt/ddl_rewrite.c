/*-------------------------------------------------------------------------
 *
 * ddl_rewrite.c
 *	  Functions to convert a utility command parsetree back to a command string
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/ddl_rewrite.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>
#include <fcntl.h>

#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "parser/analyze.h"
#include "parser/keywords.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/tqual.h"
#include "utils/typcache.h"

/*
 * Given a RangeVar, return the namespace name to use as schemaname. When
 * r->schemaname is NULL, returns the first schema name of the current
 * search_path.
 */
static char *
RangeVarGetNamespace(RangeVar *r)
{
	char       *schemaname;
	List	   *search_path = fetch_search_path(false);

	if (search_path == NIL) /* probably can't happen */
		schemaname = NULL;
	else
		schemaname = get_namespace_name(linitial_oid(search_path));

	list_free(search_path);

	return schemaname;
}

static char *
RangeVarToString(RangeVar *r)
{
	char       *schemaname = RangeVarGetNamespace(r);
	StringInfoData string;
	initStringInfo(&string);

	if (r->catalogname != NULL)
	{
		appendStringInfoString(&string, quote_identifier(r->catalogname));
		appendStringInfoChar(&string, '.');
	}
	if (schemaname != NULL)
	{
		appendStringInfoString(&string, quote_identifier(schemaname));
		appendStringInfoChar(&string, '.');
	}
	appendStringInfoString(&string, quote_identifier(r->relname));

	return string.data;
}

static const char *
relobjectkindToString(ObjectType relobjectkind)
{
	const char *objectkind;

	switch (relobjectkind)
	{
		case OBJECT_FOREIGN_TABLE:
			objectkind = "FOREIGN TABLE";
			break;

		case OBJECT_INDEX:
			objectkind = "INDEX";
			break;

		case OBJECT_SEQUENCE:
			objectkind = "SEQUENCE";
			break;

		case OBJECT_TABLE:
			objectkind = "TABLE";
			break;

		case OBJECT_VIEW:
			objectkind = "VIEW";
			break;

		default:
			elog(ERROR, "unrecognized relobjectkind: %d", relobjectkind);
			return NULL;				/* make compiler happy */
	}
	return objectkind;
}

static void
_maybeAddSeparator(StringInfo buf, const char *sep, bool *first)
{
	if (*first) *first = false;
	else        appendStringInfoString(buf, sep);
}

/*
 * The DROP statement is "generic" as in supporting multiple object types. The
 * specialized part is only finding the names of the objects dropped.
 *
 * Also the easiest way to get the command prefix is to use the command tag.
 */
static void
_rwDropStmt(EventTriggerData *trigdata)
{
	DropStmt *node = (DropStmt *)trigdata->parsetree;
	StringInfoData buf;
	ListCell *obj;
	bool first = true;

	initStringInfo(&buf);
	appendStringInfo(&buf, "%s ", trigdata->tag);

	foreach(obj, node->objects)
	{
		switch (node->removeType)
		{
			case OBJECT_TABLE:
			case OBJECT_SEQUENCE:
			case OBJECT_VIEW:
			case OBJECT_INDEX:
			case OBJECT_FOREIGN_TABLE:
			{
				RangeVar *rel = makeRangeVarFromNameList((List *) lfirst(obj));
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, RangeVarToString(rel));

				trigdata->schemaname = RangeVarGetNamespace(rel);
				trigdata->objectname = rel->relname;
				break;
			}

			case OBJECT_TYPE:
			case OBJECT_DOMAIN:
			{
				TypeName *typename = makeTypeNameFromNameList((List *) obj);
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, TypeNameToString(typename));

				if (list_nth((List *) obj, 1) == NIL)
				{
					trigdata->schemaname = NULL;
					trigdata->objectname = strVal(linitial((List *) obj));
				}
				else
				{
					trigdata->schemaname = strVal(list_nth((List *) obj, 0));
					trigdata->objectname = strVal(list_nth((List *) obj, 1));
				}
				break;
			}

			/* case OBJECT_COLLATION: */
			/* case OBJECT_CONVERSION: */
			/* case OBJECT_SCHEMA: */
			/* case OBJECT_EXTENSION: */
			default:
			{
				char *name = strVal(linitial((List *) obj));
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, name);

				trigdata->schemaname = NULL;
				trigdata->objectname = name;
				break;
			}
		}
	}
	appendStringInfo(&buf, "%s %s;",
					 node->missing_ok ? " IF EXISTS":"",
					 node->behavior == DROP_CASCADE ? "CASCADE" : "RESTRICT");

	trigdata->command = buf.data;
}

static void
_rwCreateExtensionStmt(EventTriggerData *trigdata)
{
	CreateExtensionStmt *node = (CreateExtensionStmt *)trigdata->parsetree;
	StringInfoData buf;
	ListCell   *lc;

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE EXTENSION%s %s",
					 node->if_not_exists ? " IF NOT EXISTS" : "",
					 node->extname);

	foreach(lc, node->options)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
			appendStringInfo(&buf, " SCHEMA %s", strVal(defel->arg));

		else if (strcmp(defel->defname, "new_version") == 0)
			appendStringInfo(&buf, " VERSION %s", strVal(defel->arg));

		else if (strcmp(defel->defname, "old_version") == 0)
			appendStringInfo(&buf, " FROM %s", strVal(defel->arg));
	}
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = NULL;
	trigdata->objectname = node->extname;
}

static void
_rwViewStmt(EventTriggerData *trigdata)
{
	ViewStmt *node = (ViewStmt *)trigdata->parsetree;
	StringInfoData buf;
	Query	   *viewParse;

	initStringInfo(&buf);
	viewParse = parse_analyze((Node *) copyObject(node->query),
							  "(unavailable source text)", NULL, 0);

	appendStringInfo(&buf, "CREATE %sVIEW %s AS ",
					 node->replace? "OR REPLACE": "",
					 RangeVarToString(node->view));

	get_query_def(viewParse, &buf, NIL, NULL, 0, -1, 1);
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->view);
	trigdata->objectname = node->view->relname;
}

static void
_rwColQualList(StringInfo buf, List *constraints, const char *relname)
{
	ListCell   *lc;

	foreach(lc, constraints)
	{
		Constraint *c = (Constraint *) lfirst(lc);
		Assert(IsA(c, Constraint));

		if (c->conname != NULL)
			appendStringInfo(buf, " CONSTRAINT %s", c->conname);

		switch (c->contype)
		{
			case CONSTR_NOTNULL:
				appendStringInfo(buf, " NOT NULL");
				break;

			case CONSTR_NULL:
				appendStringInfo(buf, " NULL");
				break;

			case CONSTR_UNIQUE:
				appendStringInfo(buf, " UNIQUE");
				if (c->indexspace != NULL)
					appendStringInfo(buf, " USING INDEX TABLESPACE %s", c->indexspace);
				break;

			case CONSTR_PRIMARY:
				appendStringInfo(buf, " PRIMARY KEY");
				if (c->keys != NULL)
				{
					ListCell *k;
					bool first = true;

					appendStringInfoChar(buf, '(');
					foreach(k, c->keys)
					{
						_maybeAddSeparator(buf, ",", &first);
						appendStringInfo(buf, "%s", strVal(lfirst(k)));
					}
					appendStringInfoChar(buf, ')');
				}
				if (c->indexspace != NULL)
					appendStringInfo(buf, " USING INDEX TABLESPACE %s", c->indexspace);
				break;

			case CONSTR_CHECK:
			{
				char	   *consrc;
				consrc = deparse_expression(c->raw_expr, NIL, false, false);
				appendStringInfo(buf, " CHECK (%s)", consrc);
				break;
			}

			case CONSTR_DEFAULT:
			{
				char	   *consrc;
				consrc = deparse_expression(c->raw_expr, NIL, false, false);
				appendStringInfo(buf, " DEFAULT %s", consrc);
				break;
			}

			case CONSTR_EXCLUSION:
				appendStringInfo(buf, " EXCLUDE %s () ", c->access_method);
				if (c->indexspace != NULL)
					appendStringInfo(buf, " USING INDEX TABLESPACE %s", c->indexspace);
				break;

			case CONSTR_FOREIGN:
				appendStringInfo(buf, " REFERENCES %s()", RangeVarToString(c->pktable));
				break;

			case CONSTR_ATTR_DEFERRABLE:
				appendStringInfo(buf, " DEFERRABLE");
				break;

			case CONSTR_ATTR_NOT_DEFERRABLE:
				appendStringInfo(buf, " NOT DEFERRABLE");
				break;

			case CONSTR_ATTR_DEFERRED:
				appendStringInfo(buf, " INITIALLY DEFERRED");
				break;

			case CONSTR_ATTR_IMMEDIATE:
				appendStringInfo(buf, " INITIALLY IMMEDIATE");
				break;
		}
	}
}

static void
_rwCreateStmt(EventTriggerData *trigdata)
{
	CreateStmt *node = (CreateStmt *)trigdata->parsetree;
	ListCell   *lcmd;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE TABLE %s %s",
					 RangeVarToString(node->relation),
					 node->if_not_exists ? " IF NOT EXISTS" : "");

	appendStringInfoChar(&buf, '(');

	foreach(lcmd, node->tableElts)
	{
		Node *elmt = (Node *) lfirst(lcmd);

		switch (nodeTag(elmt))
		{
			case T_ColumnDef:
			{
				ColumnDef  *c = (ColumnDef *) elmt;
				appendStringInfo(&buf, "%s %s",
								 c->colname,
								 TypeNameToString(c->typeName));
				_rwColQualList(&buf, c->constraints, node->relation->relname);
				break;
			}
			case T_TableLikeClause:
			{
				TableLikeClause *r = (TableLikeClause *) elmt;
				appendStringInfo(&buf, "%s", RangeVarToString(r->relation));
				break;
			}
			case T_Constraint:
			{
				Constraint  *c = (Constraint *) elmt;
				_rwColQualList(&buf, list_make1(c), node->relation->relname);
				break;
			}
			default:
				/* Many nodeTags are not interesting as an
				 * OptTableElementList
				 */
				break;
		}
		if (lnext(lcmd) != NULL)
			appendStringInfoChar(&buf, ',');
	}
	appendStringInfoChar(&buf, ')');
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->relation);
	trigdata->objectname = node->relation->relname;
}

static void
_rwAlterTableStmt(EventTriggerData *trigdata)
{
	AlterTableStmt *node = (AlterTableStmt *)trigdata->parsetree;
	StringInfoData buf;
	ListCell   *lcmd;
	bool        first = true;

	initStringInfo(&buf);
	appendStringInfo(&buf, "ALTER %s %s",
					 relobjectkindToString(node->relkind),
					 RangeVarToString(node->relation));

	foreach(lcmd, node->cmds)
	{
		AlterTableCmd *cmd = (AlterTableCmd *) lfirst(lcmd);
		ColumnDef  *def = (ColumnDef *) cmd->def;

		_maybeAddSeparator(&buf, ", ", &first);

		switch (cmd->subtype)
		{
			case AT_AddColumn:				/* add column */
				appendStringInfo(&buf, " ADD COLUMN %s %s",
								 def->colname,
								 TypeNameToString(def->typeName));

				if (def->is_not_null)
					appendStringInfoString(&buf, " NOT NULL");
				break;

			case AT_ColumnDefault:			/* alter column default */
				if (def == NULL)
					appendStringInfo(&buf, " ALTER %s DROP DEFAULT",
									 cmd->name);
				else
				{
					char *str =
						deparse_expression_pretty(cmd->def, NIL, false, false, 0, 0);

					appendStringInfo(&buf, " ALTER %s SET DEFAULT %s",
									 cmd->name, str);
				}
				break;

			case AT_DropNotNull:			/* alter column drop not null */
				appendStringInfo(&buf, " ALTER %s DROP NOT NULL", cmd->name);
				break;

			case AT_SetNotNull:				/* alter column set not null */
				appendStringInfo(&buf, " ALTER %s SET NOT NULL", cmd->name);
				break;

			case AT_SetStatistics:			/* alter column set statistics */
				appendStringInfo(&buf, " ALTER %s SET STATISTICS %ld",
								 cmd->name,
								 (long) intVal((Value *)(cmd->def)));
				break;

			case AT_SetOptions:				/* alter column set ( options ) */
				break;

			case AT_ResetOptions:			/* alter column reset ( options ) */
				break;

			case AT_SetStorage:				/* alter column set storage */
				appendStringInfo(&buf, " ALTER %s SET STORAGE %s",
								 cmd->name,
								 strVal((Value *)(cmd->def)));
				break;

			case AT_DropColumn:				/* drop column */
				appendStringInfo(&buf, " %s %s%s",
								 cmd->missing_ok? "DROP IF EXISTS": "DROP",
								 cmd->name,
								 cmd->behavior == DROP_CASCADE? " CASCADE": "");
				break;

			case AT_AddIndex:				/* add index */
				break;

			case AT_AddConstraint:			/* add constraint */
				break;

			case AT_ValidateConstraint:		/* validate constraint */
				appendStringInfo(&buf, " VALIDATE CONSTRAINT %s", cmd->name);
				break;

			case AT_AddIndexConstraint:		/* add constraint using existing index */
				break;

			case AT_DropConstraint:			/* drop constraint */
				appendStringInfo(&buf, " DROP CONSTRAINT%s %s %s",
								 cmd->missing_ok? " IF EXISTS": "",
								 cmd->name,
								 cmd->behavior == DROP_CASCADE? " CASCADE": "");
				break;

			case AT_AlterColumnType:		/* alter column type */
				appendStringInfo(&buf, " ALTER %s TYPE %s",
								 cmd->name,
								 TypeNameToString(def->typeName));
				if (def->raw_default != NULL)
				{
					char *str =
						deparse_expression_pretty(def->raw_default,
												  NIL, false, false, 0, 0);
					appendStringInfo(&buf, " USING %s", str);
				}
				break;

			case AT_AlterColumnGenericOptions:	/* alter column OPTIONS (...) */
				break;

			case AT_ChangeOwner:			/* change owner */
				appendStringInfo(&buf, " OWNER TO %s", cmd->name);
				break;

			case AT_ClusterOn:				/* CLUSTER ON */
				appendStringInfo(&buf, " CLUSTER ON %s", cmd->name);
				break;

			case AT_DropCluster:			/* SET WITHOUT CLUSTER */
				appendStringInfo(&buf, " SET WITHOUT CLUSTER");
				break;

			case AT_SetTableSpace:			/* SET TABLESPACE */
				appendStringInfo(&buf, " SET TABLESPACE %s", cmd->name);
				break;

			case AT_SetRelOptions:			/* SET (...) -- AM specific parameters */
				break;

			case AT_ResetRelOptions:		/* RESET (...) -- AM specific parameters */
				break;

			case AT_EnableTrig:				/* ENABLE TRIGGER name */
				appendStringInfo(&buf, " ENABLE TRIGGER %s", cmd->name);
				break;

			case AT_EnableAlwaysTrig:		/* ENABLE ALWAYS TRIGGER name */
				appendStringInfo(&buf, " ENABLE ALWAYS TRIGGER %s", cmd->name);
				break;

			case AT_EnableReplicaTrig:		/* ENABLE REPLICA TRIGGER name */
				appendStringInfo(&buf, " ENABLE REPLICA TRIGGER %s", cmd->name);
				break;

			case AT_DisableTrig:			/* DISABLE TRIGGER name */
				appendStringInfo(&buf, " DISABLE TRIGGER %s", cmd->name);
				break;

			case AT_EnableTrigAll:			/* ENABLE TRIGGER ALL */
				appendStringInfo(&buf, " ENABLE TRIGGER ALL");
				break;

			case AT_DisableTrigAll:			/* DISABLE TRIGGER ALL */
				appendStringInfo(&buf, " DISABLE TRIGGER ALL");
				break;

			case AT_EnableTrigUser:			/* ENABLE TRIGGER USER */
				appendStringInfo(&buf, " ENABLE TRIGGER USER");
				break;

			case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
				appendStringInfo(&buf, " DISABLE TRIGGER USER");
				break;

			case AT_EnableRule:				/* ENABLE RULE name */
				appendStringInfo(&buf, " ENABLE RULE %s", cmd->name);
				break;

			case AT_EnableAlwaysRule:		/* ENABLE ALWAYS RULE name */
				appendStringInfo(&buf, " ENABLE ALWAYS RULE %s", cmd->name);
				break;

			case AT_EnableReplicaRule:		/* ENABLE REPLICA RULE name */
				appendStringInfo(&buf, " ENABLE REPLICA RULE %s", cmd->name);
				break;

			case AT_DisableRule:			/* DISABLE RULE name */
				appendStringInfo(&buf, " DISABLE RULE %s", cmd->name);
				break;

			case AT_AddInherit:				/* INHERIT parent */
				appendStringInfo(&buf, " INHERIT %s",
								 RangeVarToString((RangeVar *) cmd->def));
				break;

			case AT_DropInherit:			/* NO INHERIT parent */
				appendStringInfo(&buf, " NO INHERIT %s",
								 RangeVarToString((RangeVar *) cmd->def));
				break;

			case AT_AddOf:					/* OF <type_name> */
				appendStringInfo(&buf, " OF %s", TypeNameToString(def->typeName));
				break;

			case AT_DropOf:					/* NOT OF */
				appendStringInfo(&buf, " NOT OF");
				break;

			case AT_GenericOptions:			/* OPTIONS (...) */
				break;

			default:
				break;
		}
	}
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->relation);
	trigdata->objectname = node->relation->relname;
}

/*
 * get_event_trigger_data
 *
 * Event Triggers de parse utility
 *
 * Utility statements are not planned thus won't get into a Query *, we get to
 * work from the parsetree directly, that would be query->utilityStmt which is
 * of type Node *. We declare that a void * to avoid incompatible pointer type
 * warnings.
 *
 * This function sets the command context object id, name, type, the operation,
 * the schema name and the command string.
 *
 */
void
get_event_trigger_data(EventTriggerData *trigdata)
{
	/*
	 * we need the big'o'switch here, and calling a specialized function per
	 * utility statement nodetag. Also, we could have a trigger on ANY command
	 * firing, in that case we need to avoid trying to fill the CommandContext
	 * for command we don't know how to back parse.
	 */
	trigdata->command = NULL;
	trigdata->objectname = NULL;
	trigdata->schemaname = NULL;

	switch (nodeTag(trigdata->parsetree))
	{
		case T_DropStmt:
			trigdata->objectkind = "TABLE";
			trigdata->operation = "DROP";
			_rwDropStmt(trigdata);
			break;

		case T_CreateStmt:
			trigdata->objectkind = "TABLE";
			trigdata->operation = "CREATE";
			_rwCreateStmt(trigdata);
			break;

		case T_AlterTableStmt:
			trigdata->objectkind = "TABLE";
			trigdata->operation = "ALTER";
			_rwAlterTableStmt(trigdata);
			break;

		case T_ViewStmt:
			trigdata->objectkind = "VIEW";
			trigdata->operation = "CREATE";
			_rwViewStmt(trigdata);
			break;

		case T_CreateExtensionStmt:
			trigdata->objectkind = "EXTENSION";
			trigdata->operation = "CREATE";
			_rwCreateExtensionStmt(trigdata);
			break;

		default:
			elog(DEBUG1, "unrecognized node type: %d",
				 (int) nodeTag(trigdata->parsetree));
	}
}
