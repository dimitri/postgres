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
#include "catalog/heap.h"
#include "catalog/index.h"
#include "commands/defrem.h"
#include "commands/event_trigger.h"
#include "commands/tablecmds.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "nodes/makefuncs.h"
#include "nodes/pg_list.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "parser/keywords.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "parser/parser.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
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
 *
 * It can be so that the resulting object (schema.name) does not exists, that
 * check didn't happen yet when at the "ddl_command_start" event. All we ca do
 * is play by the system's rule. The other option would be to capture and
 * expose the search_path to the event trigger functions. Then any trigger
 * function would have to duplicate the code here to extract the first schema
 * in the search_path anyway.
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
 * rewrite any_name: parser production
 */
static void _rwAnyName(StringInfo buf, List *name)
{
	bool first = true;
	ListCell *lc;

	foreach(lc, name)
	{
		char *member = (char *) lfirst(lc);

		_maybeAddSeparator(buf, ".", &first);
		appendStringInfo(buf, "%s", member);
	}
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
	appendStringInfo(&buf, "%s ", trigdata->ctag->tag);

	foreach(obj, node->objects)
	{
		List *objname = lfirst(obj);

		switch (node->removeType)
		{
			case OBJECT_INDEX:
			case OBJECT_SEQUENCE:
			case OBJECT_TABLE:
			case OBJECT_VIEW:
			case OBJECT_FOREIGN_TABLE:
			{
				RangeVar *rel = makeRangeVarFromNameList(objname);
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, RangeVarToString(rel));

				trigdata->schemaname = RangeVarGetNamespace(rel);
				trigdata->objectname = rel->relname;
				break;
			}

			case OBJECT_TYPE:
			case OBJECT_DOMAIN:
			{
				char	   *schema;
				char	   *typname;
				TypeName   *typeName = makeTypeNameFromNameList(objname);
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, TypeNameToString(typeName));

				/* deconstruct the name list */
				DeconstructQualifiedName(typeName->names, &schema, &typname);

				trigdata->schemaname = schema;
				trigdata->objectname = typname;
				break;
			}

			case OBJECT_COLLATION:
			case OBJECT_CONVERSION:
			{
				char	   *schemaname;
				char	   *name;

				DeconstructQualifiedName(objname, &schemaname, &name);

				trigdata->schemaname = schemaname;
				trigdata->objectname = name;
				break;
			}

			case OBJECT_SCHEMA:
			case OBJECT_EXTENSION:
			{
				/* see get_object_address_unqualified() */
				char *name = strVal(linitial(objname));
				_maybeAddSeparator(&buf, ", ", &first);
				appendStringInfoString(&buf, name);

				trigdata->schemaname = NULL;
				trigdata->objectname = name;
				break;
			}

			default:
				elog(ERROR, "unexpected object type: %d", node->removeType);
				break;
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

/*
 * Rewrite a OptTableSpace: grammar production
 */
static void
_rwOptTableSpace(StringInfo buf, const char *name)
{
	if (name != NULL)
		appendStringInfo(buf, " TABLESPACE %s", name);
}

/*
 * Rewrite a OptConsTableSpace: grammar production
 */
static void
_rwOptConsTableSpace(StringInfo buf, const char *name)
{
	if (name != NULL)
		appendStringInfo(buf, " USING INDEX TABLESPACE %s", name);
}

/*
 * Rewrite a generic def_arg: grammar production
 */
static void
_rwDefArg(StringInfo buf, Node *arg)
{
	appendStringInfoChar(buf, '?');
}

/*
 * Rewrite a generic definition: grammar production
 */
static void
_rwDefinition(StringInfo buf, List *definitions)
{
	/* FIXME: needs an option to print () when empty? */
	if (definitions != NULL)
	{
		ListCell *k;
		bool first = true;

		appendStringInfoChar(buf, '(');
		foreach(k, definitions)
		{
			List *def = (List *) lfirst(k);
			_maybeAddSeparator(buf, ",", &first);

			if (lsecond(def) == NULL)
			{
				/* ColLabel */
				appendStringInfo(buf, "%s", strVal(linitial(def)));
			}
			else
			{
				/* ColLabel '=' def_arg */
				appendStringInfo(buf, "%s = ", strVal(linitial(def)));
				_rwDefArg(buf, lsecond(def));
			}
		}
		appendStringInfoChar(buf, ')');
	}
}

/*
 * Rewrite the opt_column_list: grammar production
 */
static void
_rwOptColumnList(StringInfo buf, List *clist)
{
	if (clist != NULL)
	{
		ListCell *c;
		bool first = true;

		appendStringInfoChar(buf, '(');
		foreach(c, clist)
		{
			_maybeAddSeparator(buf, ",", &first);
			appendStringInfo(buf, "%s", strVal(lfirst(c)));
		}
		appendStringInfoChar(buf, ')');
	}
}

/*
 * Rewrite the opt_column_list: grammar production
 */
static void
_rwColumnList(StringInfo buf, List *clist)
{
	if (clist == NULL)
		appendStringInfo(buf, "()");
	else
		_rwOptColumnList(buf, clist);
}

/*
 * Rewrite the key_match: grammar production
 */
static void
_rwKeyMatch(StringInfo buf,  int matchtype)
{
	switch (matchtype)
	{
		case FKCONSTR_MATCH_FULL:
			appendStringInfo(buf, "MATCH FULL");
			break;

		case FKCONSTR_MATCH_PARTIAL:
			/* should not happen, not yet implemented */
			appendStringInfo(buf, "MATCH PARTIAL");
			break;

		case FKCONSTR_MATCH_SIMPLE:
		default:
			appendStringInfo(buf, "MATCH SIMPLE");
			break;
	}
}

/*
 * Rewrite the key_action: grammar production
 */
static void
_rwKeyAction(StringInfo buf, int action)
{
	switch (action)
	{
		case FKCONSTR_ACTION_NOACTION:
			appendStringInfo(buf, "NO ACTION");
			break;

		case FKCONSTR_ACTION_RESTRICT:
			appendStringInfo(buf, "RESTRICT");
			break;

		case FKCONSTR_ACTION_CASCADE:
			appendStringInfo(buf, "CASCADE");
			break;

		case FKCONSTR_ACTION_SETNULL:
			appendStringInfo(buf, "SET NULL");
			break;

		case FKCONSTR_ACTION_SETDEFAULT:
			appendStringInfo(buf, "SET DEFAULT");
			break;

		default:
			elog(ERROR, "Unexpected Foreign Key Action: %d", action);
			break;
	}
}

static void
_rwKeyActions(StringInfo buf, int upd_action, int del_action)
{
	appendStringInfo(buf, "ON UPDATE ");
	_rwKeyAction(buf, upd_action);

	appendStringInfo(buf, "ON DELETE ");
	_rwKeyAction(buf, del_action);
}

/*
 * Rewrite the ConstraintAttributeSpec: parser production
 */
static void
_rwConstraintAttributeSpec(StringInfo buf,
						   bool deferrable, bool initdeferred)
{
	if (deferrable)
		appendStringInfo(buf, " DEFERRABLE");
	else
		appendStringInfo(buf, " NOT DEFERRABLE");

	if (initdeferred)
		appendStringInfo(buf, " INITIALLY DEFERRED");
	else
		appendStringInfo(buf, " INITIALLY IMMEDIATE");
}

static void
_rwRelPersistence(StringInfo buf, int relpersistence)
{
	switch (relpersistence)
	{
		case RELPERSISTENCE_TEMP:
			appendStringInfo(buf, " TEMPORARY");
			break;

		case RELPERSISTENCE_UNLOGGED:
			appendStringInfo(buf, " UNLOGGED");
			break;

		case RELPERSISTENCE_PERMANENT:
		default:
			break;
	}
}

/*
 * rewrite the ColConstraintElem: grammar production
 *
 * Not all constraint type can be expected here, and some of them can be found
 * with other grammars as table level constraint attributes.
 */
static void
_rwColConstraintElem(StringInfo buf, List *constraints, RangeVar *relation)
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
				_rwOptConsTableSpace(buf, c->indexspace);
				break;

			case CONSTR_PRIMARY:
				appendStringInfo(buf, " PRIMARY KEY");
				_rwDefinition(buf, c->options);
				_rwOptConsTableSpace(buf, c->indexspace);
				break;

			case CONSTR_CHECK:
			{
				/*
				 * as in AddRelationNewConstraints: Create a dummy ParseState
				 * and insert the target relation as its sole rangetable entry.
				 * We need a ParseState for transformExpr.
				 */
				Node    *expr;
				char	*consrc;
				List	*dpcontext;
				ParseState *pstate = make_parsestate(NULL);
				RangeTblEntry *rte = addRangeTableEntry(pstate,
														relation,
														NULL, false, true);

				addRTEtoQuery(pstate, rte, true, true, true);

				/* deparse the constraint expression */
				expr = cookConstraint(pstate, c->raw_expr, relation->relname);

				dpcontext = deparse_context_for(relation->relname,
												EventTriggerTargetOid);
				consrc = deparse_expression(expr, dpcontext, false, false);

				appendStringInfo(buf, " CHECK (%s)", consrc);
				break;
			}

			case CONSTR_DEFAULT:
			{
				/* SERIAL columns will fill in an empty default */
				if (c->cooked_expr)
				{
					List		*dpcontext;
					Node		*expr = (Node *)stringToNode(c->cooked_expr);
					char		*consrc;

					dpcontext = deparse_context_for(relation->relname,
													EventTriggerTargetOid);
					consrc = deparse_expression(expr, dpcontext, false, false);

					appendStringInfo(buf, " DEFAULT %s", consrc);
				}
				else if (c->raw_expr)
				{
					Node    *expr;
					char	*consrc;
					List	*dpcontext;
					ParseState *pstate = make_parsestate(NULL);
					RangeTblEntry *rte = addRangeTableEntry(pstate,
															relation,
															NULL, false, true);

					addRTEtoQuery(pstate, rte, true, true, true);

					/* deparse the constraint expression */
					expr = cookDefault(pstate, c->raw_expr, InvalidOid, -1, NULL);

					dpcontext = deparse_context_for(relation->relname,
													EventTriggerTargetOid);
					consrc = deparse_expression(expr, dpcontext, false, false);

					appendStringInfo(buf, " DEFAULT %s", consrc);
				}
				break;
			}

			case CONSTR_FOREIGN:
			{
				appendStringInfo(buf, " REFERENCES %s",
								 RangeVarToString(c->pktable));
				_rwOptColumnList(buf, c->pk_attrs);
				_rwKeyMatch(buf, c->fk_matchtype);
				_rwKeyActions(buf, c->fk_upd_action, c->fk_del_action);
				break;
			}

			default:
				/* unexpected case, WARNING? */
				elog(WARNING, "Constraint %d is not a column constraint",
					 c->contype);
				break;
		}
	}
}

/*
 * rewrite a list of TableConstraint: grammar production
 */
static void
_rwTableConstraint(StringInfo buf, List *constraints, RangeVar *relation)
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
			case CONSTR_CHECK:
			{
				Node    *expr;
				char	*consrc;
				List	*dpcontext;
				ParseState *pstate = make_parsestate(NULL);
				RangeTblEntry *rte = addRangeTableEntry(pstate,
														relation,
														NULL, false, true);

				addRTEtoQuery(pstate, rte, true, true, true);

				/* deparse the constraint expression */
				expr = cookConstraint(pstate, c->raw_expr, relation->relname);

				dpcontext = deparse_context_for(relation->relname,
												EventTriggerTargetOid);
				consrc = deparse_expression(expr, dpcontext, false, false);

				appendStringInfo(buf, " CHECK (%s)", consrc);
				break;
			}

			case CONSTR_UNIQUE:
				appendStringInfo(buf, " UNIQUE");

				if (c->keys)
				{
					/* unique (column, list) */
					_rwColumnList(buf, c->keys);
					_rwDefinition(buf, c->options);
					_rwConstraintAttributeSpec(buf,
											   c->deferrable, c->initdeferred);
					_rwOptConsTableSpace(buf, c->indexspace);
				}
				else
				{
					/* unique using index */
					appendStringInfo(buf, " USING INDEX %s", c->indexname);
					_rwConstraintAttributeSpec(buf,
											   c->deferrable, c->initdeferred);
				}
				break;

			case CONSTR_PRIMARY:
				appendStringInfo(buf, " PRIMARY KEY");

				if (c->keys)
				{
					/* primary key (column, list) */
					_rwColumnList(buf, c->keys);
					_rwDefinition(buf, c->options);
					_rwOptConsTableSpace(buf, c->indexspace);
					_rwConstraintAttributeSpec(buf,
											   c->deferrable, c->initdeferred);
				}
				else
				{
					/* primary key using index */
					appendStringInfo(buf, " USING INDEX %s", c->indexname);
					_rwConstraintAttributeSpec(buf,
											   c->deferrable, c->initdeferred);
				}
				break;

			case CONSTR_EXCLUSION:
				appendStringInfo(buf, " EXCLUDE %s ", c->access_method);

				if (c->exclusions == NULL)
					appendStringInfo(buf, "()");
				else
				{
					/* ExclustionConstraintList */
					ListCell *e;
					bool first = true;

					appendStringInfoChar(buf, '(');
					foreach(e, c->exclusions)
					{
						List *ec = (List *)lfirst(e);

						_maybeAddSeparator(buf, ",", &first);

						/* ExclustionConstraintElem */
						appendStringInfo(buf, "%s WITH OPERATOR(%s)",
										 strVal(linitial(ec)),
										 strVal(lsecond(ec)));
					}
					appendStringInfoChar(buf, ')');
				}
				_rwDefinition(buf, c->options);
				_rwOptConsTableSpace(buf, c->indexspace);

				/* ExclusionWhereClause: */
				if (c->where_clause)
					/* FIXME: Add support for raw expressions */
					appendStringInfo(buf, " WHERE ( %s )", "?");

				_rwConstraintAttributeSpec(buf,
										   c->deferrable, c->initdeferred);
				break;

			case CONSTR_FOREIGN:
				appendStringInfo(buf, " FOREIGN KEY");

				_rwColumnList(buf, c->fk_attrs);

				appendStringInfo(buf, " REFERENCES %s",
								 RangeVarToString(c->pktable));

				_rwOptColumnList(buf, c->pk_attrs);

				_rwKeyMatch(buf, c->fk_matchtype);
				_rwKeyActions(buf, c->fk_upd_action, c->fk_del_action);

				_rwConstraintAttributeSpec(buf,
										   c->deferrable, c->initdeferred);

				if (c->skip_validation)
					appendStringInfo(buf, " NOT VALID");
				break;

			default:
				/* unexpected case, WARNING? */
				elog(WARNING, "Constraint %d is not a column constraint",
					 c->contype);
				break;
		}
	}
}

/*
 * rewrite TableLikeOptionList: parser production
 */
static void
_rwTableLikeOptionList(StringInfo buf, bits32 options)
{
	if (options == CREATE_TABLE_LIKE_ALL)
		appendStringInfo(buf, " INCLUDING ALL");
	else
	{
		if (options & CREATE_TABLE_LIKE_DEFAULTS)
			appendStringInfo(buf, " INCLUDING DEFAULTS");

		if (options & CREATE_TABLE_LIKE_CONSTRAINTS)
			appendStringInfo(buf, " INCLUDING CONSTRAINTS");

		if (options & CREATE_TABLE_LIKE_INDEXES)
			appendStringInfo(buf, " INCLUDING INDEXES");

		if (options & CREATE_TABLE_LIKE_STORAGE)
			appendStringInfo(buf, " INCLUDING STORAGE");

		if (options & CREATE_TABLE_LIKE_COMMENTS)
			appendStringInfo(buf, " INCLUDING COMMENTS");
	}
}

/*
 * rewrite OptTableElementList: parser production
 */
static void
_rwOptTableElementList(StringInfo buf, List *tableElts, RangeVar *relation)
{
	bool        first = true;
	ListCell   *e;

	appendStringInfoChar(buf, '(');

	foreach(e, tableElts)
	{
		Node *elmt = (Node *) lfirst(e);

		_maybeAddSeparator(buf, ", ", &first);

		switch (nodeTag(elmt))
		{
			case T_ColumnDef:
			{
				ColumnDef  *c = (ColumnDef *) elmt;
				appendStringInfo(buf, "%s %s",
								 c->colname,
								 TypeNameToString(c->typeName));
				_rwColConstraintElem(buf, c->constraints, relation);
				break;
			}
			case T_TableLikeClause:
			{
				TableLikeClause *like = (TableLikeClause *) elmt;
				appendStringInfo(buf, "LIKE %s",
								 RangeVarToString(like->relation));
				_rwTableLikeOptionList(buf, like->options);
				break;
			}
			case T_Constraint:
			{
				Constraint  *c = (Constraint *) elmt;
				_rwTableConstraint(buf, list_make1(c), relation);
				break;
			}
			default:
				/* Many nodeTags are not OptTableElementList */
				break;
		}
	}
	appendStringInfoChar(buf, ')');
}

/*
 * rewrite OptTypedTableElementList: parser production
 */
static void
_rwOptTypedTableElementList(StringInfo buf, List *tableElts, RangeVar *relation)
{
	bool        first = true;
	ListCell   *e;

	foreach(e, tableElts)
	{
		Node *elmt = (Node *) lfirst(e);

		_maybeAddSeparator(buf, ", ", &first);

		switch (nodeTag(elmt))
		{
			case T_ColumnDef:
			{
				ColumnDef  *c = (ColumnDef *) elmt;
				appendStringInfo(buf, "%s WITH OPTIONS", c->colname);
				_rwColConstraintElem(buf, c->constraints, relation);
				break;
			}
			case T_Constraint:
			{
				Constraint  *c = (Constraint *) elmt;
				_rwTableConstraint(buf, list_make1(c), relation);
				break;
			}
			default:
				/* Many nodeTags are not OptTableElementList */
				break;
		}
	}
}

/*
 * rewrite OptInherit: parser production
 */
static void
_rwOptInherit(StringInfo buf, List *inhRelations)
{
	if (inhRelations)
	{
		ListCell *inher;
		bool first = true;

		appendStringInfo(buf, " INHERITS (");
		foreach(inher, inhRelations)
		{
			RangeVar   *inh = (RangeVar *) lfirst(inher);

			_maybeAddSeparator(buf, ",", &first);
			appendStringInfo(buf, "%s", RangeVarToString(inh));
		}
		appendStringInfoChar(buf, ')');
	}
}

/*
 * rewrite reloptions: parser production
 */
static void
_rwRelOptions(StringInfo buf, List *options, bool null_is_true)
{
	bool        first = true;
	ListCell   *lc;

	foreach(lc, options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *value;

		_maybeAddSeparator(buf, ", ", &first);

		if (def->arg != NULL)
		{
			/* ALTER TABLE ... SET (defname = value) */
			value = defGetString(def);
			appendStringInfo(buf, "%s=%s", def->defname, value);
		}
		else
		{
			if (null_is_true)
				/* ALTER TABLE ... SET (defname = true) */
				appendStringInfo(buf, "%s=true", def->defname);
			else
				/* ALTER TABLE ... RESET (defname) */
				appendStringInfo(buf, "%s", def->defname);
		}
	}
}

/*
 * rewrite OptWith: parser production
 */
static void
_rwOptWith(StringInfo buf, List *options)
{
	if (options)
	{
		appendStringInfoString(buf, " WITH (");
		_rwRelOptions(buf, options, true);
		appendStringInfoString(buf, " )");
	}
}

/*
 * rewrite OptCommitOption: parser production
 */
static void
_rwOnCommitOption(StringInfo buf, int oncommit)
{
	switch (oncommit)
	{
		case ONCOMMIT_DROP:
			appendStringInfo(buf, " ON COMMIT DROP");
			break;

		case ONCOMMIT_DELETE_ROWS:
			appendStringInfo(buf, " ON COMMIT DELETE ROWS");
			break;

		case ONCOMMIT_PRESERVE_ROWS:
			appendStringInfo(buf, " ON COMMIT PRESERVE ROWS");
			break;

		case ONCOMMIT_NOOP:
			/* EMPTY */
			break;

	}
}

/*
 * rewrite CreateStmt: parser production
 */
static void
_rwCreateStmt(EventTriggerData *trigdata)
{
	CreateStmt *node = (CreateStmt *)trigdata->parsetree;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE ");
	_rwRelPersistence(&buf, node->relation->relpersistence);
	appendStringInfo(&buf, "TABLE %s %s",
					 RangeVarToString(node->relation),
					 node->if_not_exists ? " IF NOT EXISTS" : "");

	if (node->ofTypename)
	{
		appendStringInfo(&buf, "OF %s",
						 TypeNameToString(node->ofTypename));
		_rwOptTypedTableElementList(&buf, node->tableElts, node->relation);
		_rwOptWith(&buf, node->options);
		_rwOnCommitOption(&buf, node->oncommit);
		_rwOptTableSpace(&buf, node->tablespacename);
	}
	else
	{
		List *elts = list_concat(node->tableElts, node->constraints);
		_rwOptTableElementList(&buf, elts, node->relation);
		_rwOptInherit(&buf, node->inhRelations);
		_rwOptWith(&buf, node->options);
		_rwOnCommitOption(&buf, node->oncommit);
		_rwOptTableSpace(&buf, node->tablespacename);
	}
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->relation);
	trigdata->objectname = node->relation->relname;
}

/*
 * rewrite AlterTableCmd as produced by the AlterTable implementation in the
 * preparation step.
 */
static void
_rwAlterTableCmd(StringInfo buf, AlterTableCmd *cmd, RangeVar *relation)
{
	switch (cmd->subtype)
	{
		case AT_AddColumn:				/* add column */
		case AT_AddColumnToView:
		case AT_AddColumnRecurse:
		{
			ColumnDef  *def = (ColumnDef *) cmd->def;

			appendStringInfo(buf, " ADD COLUMN %s %s",
							 def->colname,
							 TypeNameToString(def->typeName));

			if (def->is_not_null)
				appendStringInfoString(buf, " NOT NULL");
			break;
		}

		case AT_ColumnDefault:			/* alter column default */
		{
			ColumnDef  *def = (ColumnDef *) cmd->def;

			if (def == NULL)
				appendStringInfo(buf, " ALTER %s DROP DEFAULT",
								 cmd->name);
			else
			{
				char *str =
					deparse_expression_pretty(cmd->def, NIL, false, false, 0, 0);

				appendStringInfo(buf, " ALTER %s SET DEFAULT %s",
								 cmd->name, str);
			}
			break;
		}

		case AT_DropNotNull:			/* alter column drop not null */
			appendStringInfo(buf, " ALTER %s DROP NOT NULL", cmd->name);
			break;

		case AT_SetNotNull:				/* alter column set not null */
			appendStringInfo(buf, " ALTER %s SET NOT NULL", cmd->name);
			break;

		case AT_SetStatistics:			/* alter column set statistics */
			appendStringInfo(buf, " ALTER %s SET STATISTICS %ld",
							 cmd->name,
							 (long) intVal((Value *)(cmd->def)));
			break;

		case AT_SetOptions:				/* alter column set ( options ) */
			_rwRelOptions(buf, (List *)cmd->def, true);
			break;

		case AT_ResetOptions:			/* alter column reset ( options ) */
			_rwRelOptions(buf, (List *)cmd->def, true);
			break;

		case AT_SetStorage:				/* alter column set storage */
			appendStringInfo(buf, " ALTER %s SET STORAGE %s",
							 cmd->name,
							 strVal((Value *)(cmd->def)));
			break;

		case AT_DropColumn:				/* drop column */
		case AT_DropColumnRecurse:
			appendStringInfo(buf, " %s %s%s",
							 cmd->missing_ok? "DROP IF EXISTS": "DROP",
							 cmd->name,
							 cmd->behavior == DROP_CASCADE? " CASCADE": "");
			break;

		case AT_AddIndex:				/* add index */
		case AT_ReAddIndex:
			/* can not be parsed by gram.y, apparently */
			break;

		case AT_AddConstraint:			/* add constraint */
		case AT_AddConstraintRecurse:
		case AT_ReAddConstraint:
		{
			Constraint  *constraint = (Constraint *) cmd->def;

			elog(NOTICE, "AT_AddConstraint: %s", constraint->conname);

			appendStringInfo(buf, " ADD");
			_rwTableConstraint(buf, list_make1(constraint), relation);
			break;
		}

		case AT_ValidateConstraint:		/* validate constraint */
			appendStringInfo(buf, " VALIDATE CONSTRAINT %s", cmd->name);
			break;

		case AT_AddIndexConstraint:		/* add constraint using existing index */
		{
			Constraint  *constraint = (Constraint *) cmd->def;

			elog(NOTICE, "AT_AddIndexConstraint: %s", constraint->conname);

			appendStringInfo(buf, " ADD ");
			_rwTableConstraint(buf, list_make1(constraint), relation);
			break;
		}
			break;

		case AT_DropConstraint:			/* drop constraint */
		case AT_DropConstraintRecurse:
			appendStringInfo(buf, " DROP CONSTRAINT%s %s %s",
							 cmd->missing_ok? " IF EXISTS": "",
							 cmd->name,
							 cmd->behavior == DROP_CASCADE? " CASCADE": "");
			break;

		case AT_AlterColumnType:		/* alter column type */
		{
			ColumnDef  *def = (ColumnDef *) cmd->def;

			appendStringInfo(buf, " ALTER %s TYPE %s",
							 cmd->name,
							 TypeNameToString(def->typeName));
			if (def->raw_default != NULL)
			{
				char *str =
					deparse_expression_pretty(def->raw_default,
											  NIL, false, false, 0, 0);
				appendStringInfo(buf, " USING %s", str);
			}
			break;
		}

		case AT_AlterColumnGenericOptions:	/* alter column OPTIONS (...) */
			/* FIXME */
			appendStringInfo(buf, " OPTIONS (");
			appendStringInfoChar(buf, ')');
			break;

		case AT_ChangeOwner:			/* change owner */
			appendStringInfo(buf, " OWNER TO %s", cmd->name);
			break;

		case AT_ClusterOn:				/* CLUSTER ON */
			appendStringInfo(buf, " CLUSTER ON %s", cmd->name);
			break;

		case AT_DropCluster:			/* SET WITHOUT CLUSTER */
			appendStringInfo(buf, " SET WITHOUT CLUSTER");
			break;

		case AT_AddOids:			/* SET WITH OIDS */
		case AT_AddOidsRecurse:
			appendStringInfo(buf, " SET WITH OIDS");
			break;

		case AT_DropOids:			/* SET WITHOUT OIDS */
			appendStringInfo(buf, " SET WITHOUT OIDS");
			break;

		case AT_SetTableSpace:			/* SET TABLESPACE */
			appendStringInfo(buf, " SET TABLESPACE %s", cmd->name);
			break;

		case AT_SetRelOptions:			/* SET (...) -- AM specific parameters */
			appendStringInfo(buf, " SET (");
			_rwRelOptions(buf, (List *)cmd->def, true);
			appendStringInfoChar(buf, ')');
			break;

		case AT_ResetRelOptions:		/* RESET (...) -- AM specific parameters */
			appendStringInfo(buf, " RESET (");
			_rwRelOptions(buf, (List *)cmd->def, false);
			appendStringInfoChar(buf, ')');
			break;

		case AT_EnableTrig:				/* ENABLE TRIGGER name */
			appendStringInfo(buf, " ENABLE TRIGGER %s", cmd->name);
			break;

		case AT_EnableAlwaysTrig:		/* ENABLE ALWAYS TRIGGER name */
			appendStringInfo(buf, " ENABLE ALWAYS TRIGGER %s", cmd->name);
			break;

		case AT_EnableReplicaTrig:		/* ENABLE REPLICA TRIGGER name */
			appendStringInfo(buf, " ENABLE REPLICA TRIGGER %s", cmd->name);
			break;

		case AT_DisableTrig:			/* DISABLE TRIGGER name */
			appendStringInfo(buf, " DISABLE TRIGGER %s", cmd->name);
			break;

		case AT_EnableTrigAll:			/* ENABLE TRIGGER ALL */
			appendStringInfo(buf, " ENABLE TRIGGER ALL");
			break;

		case AT_DisableTrigAll:			/* DISABLE TRIGGER ALL */
			appendStringInfo(buf, " DISABLE TRIGGER ALL");
			break;

		case AT_EnableTrigUser:			/* ENABLE TRIGGER USER */
			appendStringInfo(buf, " ENABLE TRIGGER USER");
			break;

		case AT_DisableTrigUser:		/* DISABLE TRIGGER USER */
			appendStringInfo(buf, " DISABLE TRIGGER USER");
			break;

		case AT_EnableRule:				/* ENABLE RULE name */
			appendStringInfo(buf, " ENABLE RULE %s", cmd->name);
			break;

		case AT_EnableAlwaysRule:		/* ENABLE ALWAYS RULE name */
			appendStringInfo(buf, " ENABLE ALWAYS RULE %s", cmd->name);
			break;

		case AT_EnableReplicaRule:		/* ENABLE REPLICA RULE name */
			appendStringInfo(buf, " ENABLE REPLICA RULE %s", cmd->name);
			break;

		case AT_DisableRule:			/* DISABLE RULE name */
			appendStringInfo(buf, " DISABLE RULE %s", cmd->name);
			break;

		case AT_AddInherit:				/* INHERIT parent */
			appendStringInfo(buf, " INHERIT %s",
							 RangeVarToString((RangeVar *) cmd->def));
			break;

		case AT_DropInherit:			/* NO INHERIT parent */
			appendStringInfo(buf, " NO INHERIT %s",
							 RangeVarToString((RangeVar *) cmd->def));
			break;

		case AT_AddOf:					/* OF <type_name> */
		{
			ColumnDef  *def = (ColumnDef *) cmd->def;

			appendStringInfo(buf, " OF %s", TypeNameToString(def->typeName));
			break;
		}

		case AT_DropOf:					/* NOT OF */
			appendStringInfo(buf, " NOT OF");
			break;

		case AT_GenericOptions:			/* OPTIONS (...) */
			/* FIXME */
			break;

		default:
			break;
	}
}

/*
 * rewrite AlterTableStmt: parser production
 */
static void
_rwAlterTableStmt(EventTriggerData *trigdata)
{
	AlterTableStmt		*node = (AlterTableStmt *)trigdata->parsetree;
	StringInfoData		 buf;

	/*
	 * in ProcessUtility we trick the first entry of the cmds to be an
	 * AlterTable work queue.
	 */
	List		*wqueue = node->cmds;
	ListCell	*ltab;
	bool		 first	= true;
	int			 pass;

	initStringInfo(&buf);
	appendStringInfo(&buf, "ALTER %s %s",
					 relobjectkindToString(node->relkind),
					 RangeVarToString(node->relation));

	for (pass = 0; pass < AT_NUM_PASSES; pass++)
	{
		/* Go through each table that needs to be processed */
		foreach(ltab, wqueue)
		{
			AlteredTableInfo *tab = (AlteredTableInfo *) lfirst(ltab);
			List	   *subcmds = tab->subcmds[pass];
			ListCell   *lcmd;

			if (subcmds == NIL)
				continue;

			foreach(lcmd, subcmds)
			{
				_maybeAddSeparator(&buf, ",", &first);
				_rwAlterTableCmd(&buf,
								 (AlterTableCmd *)lfirst(lcmd),
								 node->relation);
			}
		}
	}
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->relation);
	trigdata->objectname = node->relation->relname;
}

/*
 * rewrite OptSeqOptList: parser production
 */
static void
_rwOptSeqOptList(StringInfo buf, List *options)
{
	ListCell *opt;

	foreach(opt, options)
	{
		DefElem    *defel = (DefElem *) lfirst(opt);

		if (strcmp(defel->defname, "cache") == 0)
		{
			char		num[100];

			snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
			appendStringInfo(buf, " CACHE %s", num);
		}
		else if (strcmp(defel->defname, "cycle") == 0)
		{
			if (intVal(defel))
				appendStringInfo(buf, " CYCLE");
			else
				appendStringInfo(buf, " NO CYCLE");
		}
		else if (strcmp(defel->defname, "increment") == 0)
		{
			char		num[100];

			snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
			appendStringInfo(buf, " INCREMENT BY %s", num);
		}
		else if (strcmp(defel->defname, "maxvalue") == 0)
		{
			if (defel->arg)
			{
				char		num[100];

				snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
				appendStringInfo(buf, " MAXVALUE %s", num);
			}
			else
				appendStringInfo(buf, " NO MAXVALUE");
		}
		else if (strcmp(defel->defname, "minvalue") == 0)
		{
			if (defel->arg)
			{
				char		num[100];

				snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
				appendStringInfo(buf, " MINVALUE %s", num);
			}
			else
				appendStringInfo(buf, " NO MINVALUE");
		}
		else if (strcmp(defel->defname, "owned_by") == 0)
		{
			List       *owned_by = defGetQualifiedName(defel);
			int         nnames = list_length(owned_by);
			List	   *relname;
			char	   *attrname;
			RangeVar   *rel;

			relname = list_truncate(list_copy(owned_by), nnames - 1);
			attrname = strVal(lfirst(list_tail(owned_by)));
			rel = makeRangeVarFromNameList(relname);

			appendStringInfo(buf, " OWNED BY %s.%s",
							 RangeVarToString(rel), attrname);
		}
		else if (strcmp(defel->defname, "start") == 0)
		{
			char		num[100];

			snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
			appendStringInfo(buf, " START WITH %s", num);
		}
		else if (strcmp(defel->defname, "restart") == 0)
		{
			if (defel->arg)
			{
				char		num[100];

				snprintf(num, sizeof(num), INT64_FORMAT, defGetInt64(defel));
				appendStringInfo(buf, " RESTART WITH %s", num);
			}
			else
				appendStringInfo(buf, " RESTART");
		}
	}
}

/*
 * rewrite CreateSeqStmt: parser production
 */
static void
_rwCreateSeqStmt(EventTriggerData *trigdata)
{
	CreateSeqStmt *node = (CreateSeqStmt *)trigdata->parsetree;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE ");
	_rwRelPersistence(&buf, node->sequence->relpersistence);
	appendStringInfo(&buf, "SEQUENCE %s", RangeVarToString(node->sequence));
	_rwOptSeqOptList(&buf, node->options);
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->sequence);
	trigdata->objectname = node->sequence->relname;
}

/*
 * rewrite AlterSeqStmt: parser production
 */
static void
_rwAlterSeqStmt(EventTriggerData *trigdata)
{
	AlterSeqStmt *node = (AlterSeqStmt *)trigdata->parsetree;
	StringInfoData buf;

	initStringInfo(&buf);
	appendStringInfo(&buf, "ALTER SEQUENCE%s %s",
					 node->missing_ok? " IF EXISTS": "",
					 RangeVarToString(node->sequence));
	_rwOptSeqOptList(&buf, node->options);
	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->sequence);
	trigdata->objectname = node->sequence->relname;
}

/*
 * rewrite index_elem: parser production
 */
static void
_rwIndexElem(StringInfo buf, IndexElem *e, List *context)
{
	if (e->name)
		appendStringInfo(buf, "%s", e->name);
	else
	{
		char *str =
			deparse_expression_pretty(e->expr, context, false, false, false, 0);

		/* Need parens if it's not a bare function call */
		if (IsA(e->expr, FuncExpr) &&
				 ((FuncExpr *) e->expr)->funcformat == COERCE_EXPLICIT_CALL)
			appendStringInfo(buf, "%s", str);
		else
			appendStringInfo(buf, "(%s)", str);
	}

	if (e->collation)
	{
		appendStringInfo(buf, " COLLATE");
		_rwAnyName(buf, e->collation);
	}

	if (e->opclass)
	{
		appendStringInfo(buf, " USING ");
		_rwAnyName(buf, e->opclass);
	}

	/* defensive coding, so that the compiler hints us into updating those bits
	 * if needs be */
	switch (e->ordering)
	{
		/* using unexpected in create index */
		case SORTBY_DEFAULT:
		case SORTBY_USING:
			break;

		case SORTBY_ASC:
			appendStringInfo(buf, " ASC");
			break;

		case SORTBY_DESC:
			appendStringInfo(buf, " DESC");
			break;
	}
	switch (e->nulls_ordering)
	{
		case SORTBY_NULLS_DEFAULT:
			break;

		case SORTBY_NULLS_FIRST:
			appendStringInfo(buf, " NULLS FIRST");
			break;

		case SORTBY_NULLS_LAST:
			appendStringInfo(buf, " NULLS LAST");
			break;
	}
}

/*
 * rewrite IndexStmt: parser production
 */
static void
_rwCreateIndexStmt(EventTriggerData *trigdata)
{
	IndexStmt			*node  = (IndexStmt *)trigdata->parsetree;
	StringInfoData		 buf;
	Oid					 relId;
	bool				 first = true;
	ListCell			*lc;
	List				*context;

	initStringInfo(&buf);
	appendStringInfo(&buf, "CREATE%s INDEX", node->unique ? " UNIQUE" : "");

	if (node->concurrent)
		appendStringInfo(&buf, " CONCURRENTLY");

	if (node->idxname)
		appendStringInfo(&buf, " %s", node->idxname);

	appendStringInfo(&buf, " ON %s USING %s (",
					 RangeVarToString(node->relation),
					 node->accessMethod);

	/*
	 * we could find a way to only do that when we know we have to deal with
	 * column expressions, but it's simpler this way.
	 *
	 * We use get_rel_name() without checking the return value here, on the
	 * grounds that it's safe to do so when still in the transaction that just
	 * created the index.
	 */
	relId = IndexGetRelation(EventTriggerTargetOid, false);
	context = deparse_context_for(get_rel_name(relId), relId);

	foreach(lc, node->indexParams)
	{
		IndexElem *e = (IndexElem *) lfirst(lc);

		_maybeAddSeparator(&buf, ", ", &first);
		_rwIndexElem(&buf, e, context);
	}
	appendStringInfoChar(&buf, ')');

	_rwOptWith(&buf, node->options);
	_rwOptConsTableSpace(&buf, node->tableSpace);

	/* FIXME: handle where_clause: production */
	if (node->whereClause)
	{
		char *str =
			deparse_expression_pretty(node->whereClause, context,
									  false, false, false, 0);
		appendStringInfo(&buf, " WHERE (%s)", str);
	}

	appendStringInfoChar(&buf, ';');

	trigdata->command = buf.data;
	trigdata->schemaname = RangeVarGetNamespace(node->relation);
	trigdata->objectname = node->idxname;
}

/* get the work done */
static void
normalize_command_string(EventTriggerData *trigdata)
{
	/*
	 * we need the big'o'switch here, and calling a specialized function per
	 * utility statement nodetag.
	 */
	switch (nodeTag(trigdata->parsetree))
	{
		case T_DropStmt:
			_rwDropStmt(trigdata);
			break;

		case T_CreateStmt:
			_rwCreateStmt(trigdata);
			break;

		case T_AlterTableStmt:
			_rwAlterTableStmt(trigdata);
			break;

		case T_AlterSeqStmt:
			_rwAlterSeqStmt(trigdata);
			break;

		case T_ViewStmt:
			_rwViewStmt(trigdata);
			break;

		case T_CreateExtensionStmt:
			_rwCreateExtensionStmt(trigdata);
			break;

		case T_CreateSeqStmt:
			_rwCreateSeqStmt(trigdata);
			break;

		case T_IndexStmt:
			_rwCreateIndexStmt(trigdata);
			break;

		default:
			elog(DEBUG1, "unrecognized node type: %d",
				 (int) nodeTag(trigdata->parsetree));
	}
}

/*
 * get_event_trigger_data
 *
 * Event Triggers deparse utility
 *
 * Utility statements are not planned thus won't get into a Query *, we get to
 * work from the parsetree directly, that would be query->utilityStmt which is
 * of type Node *. We declare that a void * to avoid incompatible pointer type
 * warnings.
 *
 * This function sets the command context object name, type, the operation, the
 * schema name and the command string.
 *
 */
void
get_event_trigger_data(EventTriggerData *trigdata)
{
	/*
	 * Only attempt to deparse the command string when we have enough context
	 * to do so. That means ddl_command_start for DROP operations and
	 * ddl_command_end for CREATE and ALTER operations.
	 */
	bool rewrite =
		((trigdata->ctag->operation == COMMAND_TAG_CREATE
		  || trigdata->ctag->operation == COMMAND_TAG_ALTER)
		 && strcmp(trigdata->event, "ddl_command_end") == 0)
		||
		(trigdata->ctag->operation == COMMAND_TAG_DROP
		 && strcmp(trigdata->event, "ddl_command_start") == 0);

	/* initialize yet unknown pieces of information */
	trigdata->command	 = NULL;
	trigdata->schemaname = NULL;
	trigdata->objectname = NULL;

	/* TODO: add the main target object's OID */

	if (rewrite)
	{
		/* only add the objectid when we have it */
		trigdata->objectid  = EventTriggerTargetOid;

		/* that will also fill in schemaname and objectname */
		normalize_command_string(trigdata);
	}
}
