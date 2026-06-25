/*-------------------------------------------------------------------------
 *
 * sqlquery.c
 *	  I/O and comparison functions for the sql_query data type.
 *
 * sql_query stores a fully analyzed SQL query tree in nodeToString() format
 * (same wire representation as pg_node_tree columns such as pg_rewrite.ev_action).
 * The external text representation is canonical SQL produced by pg_get_querydef().
 *
 * Key property: because the stored datum holds an analyzed Query node with
 * relation OIDs rather than unqualified names, deparsing in a context with an
 * empty search_path (e.g. pg_restore's REFRESH MATERIALIZED VIEW security
 * context) automatically emits schema-qualified names.  This fixes the failure
 * of ts_stat() inside materialized views during pg_restore.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/backend/utils/adt/sqlquery.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/hash.h"
#include "catalog/pg_collation.h"
#include "libpq/pqformat.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "parser/parser.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/ruleutils.h"
#include "utils/varlena.h"

PG_FUNCTION_INFO_V1(sql_query_in);
PG_FUNCTION_INFO_V1(sql_query_out);
PG_FUNCTION_INFO_V1(sql_query_recv);
PG_FUNCTION_INFO_V1(sql_query_send);
PG_FUNCTION_INFO_V1(sql_query_eq);
PG_FUNCTION_INFO_V1(sql_query_ne);
PG_FUNCTION_INFO_V1(sql_query_lt);
PG_FUNCTION_INFO_V1(sql_query_le);
PG_FUNCTION_INFO_V1(sql_query_gt);
PG_FUNCTION_INFO_V1(sql_query_ge);
PG_FUNCTION_INFO_V1(sql_query_cmp);
PG_FUNCTION_INFO_V1(sql_query_hash);
PG_FUNCTION_INFO_V1(sql_query_hash_extended);


/*
 * sql_query_parse_and_analyze
 *
 * Parse and semantically analyze a SQL string, returning a palloc'd
 * nodeToString() serialization of the resulting Query node.  Errors on
 * invalid SQL or on anything other than exactly one statement.
 */
static char *
sql_query_parse_and_analyze(const char *str)
{
	List	   *raw_list;
	RawStmt    *rawstmt;
	List	   *query_list;
	Query	   *query;

	raw_list = raw_parser(str, RAW_PARSE_DEFAULT);

	if (list_length(raw_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("sql_query value must contain exactly one SQL statement")));

	rawstmt = linitial_node(RawStmt, raw_list);

	/*
	 * Analyze with no parameter types.  Parameterized queries ($1, $2, …)
	 * are unsupported because the sql_query type carries no parameter type
	 * information.
	 */
	query_list = pg_analyze_and_rewrite_fixedparams(rawstmt, str, NULL, 0, NULL);

	if (list_length(query_list) != 1)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unexpected result from query analysis")));

	query = linitial_node(Query, query_list);

	return nodeToString(query);
}


/*
 * sql_query_in - input function
 *
 * Parse and analyze the SQL string; store the analyzed Query as nodeToString.
 */
Datum
sql_query_in(PG_FUNCTION_ARGS)
{
	char	   *str = PG_GETARG_CSTRING(0);
	char	   *nodestr;

	nodestr = sql_query_parse_and_analyze(str);
	PG_RETURN_TEXT_P(cstring_to_text(nodestr));
}


/*
 * sql_query_out - output function
 *
 * Deparse the stored Query node back to canonical SQL text.
 * generate_relation_name() is search_path-sensitive: when called in a context
 * with an empty search_path (e.g. pg_restore's REFRESH), it emits
 * schema-qualified names, making the deparsed SQL safe to re-execute.
 */
Datum
sql_query_out(PG_FUNCTION_ARGS)
{
	text	   *val = PG_GETARG_TEXT_PP(0);
	char	   *nodestr = text_to_cstring(val);
	Query	   *query;
	char	   *result;

	query = castNode(Query, stringToNode(nodestr));
	result = pg_get_querydef(query, false);

	PG_RETURN_CSTRING(result);
}


/*
 * sql_query_recv - binary input function
 *
 * Read a SQL text string from the wire; parse and analyze it.
 */
Datum
sql_query_recv(PG_FUNCTION_ARGS)
{
	StringInfo	buf = (StringInfo) PG_GETARG_POINTER(0);
	char	   *str;
	int			nbytes;
	char	   *nodestr;

	str = pq_getmsgtext(buf, buf->len - buf->cursor, &nbytes);
	nodestr = sql_query_parse_and_analyze(str);
	pfree(str);

	PG_RETURN_TEXT_P(cstring_to_text(nodestr));
}


/*
 * sql_query_send - binary output function
 *
 * Send the canonical SQL text representation (search_path-aware deparsing).
 */
Datum
sql_query_send(PG_FUNCTION_ARGS)
{
	text	   *val = PG_GETARG_TEXT_PP(0);
	char	   *nodestr = text_to_cstring(val);
	Query	   *query;
	char	   *sql;
	StringInfoData buf;

	query = castNode(Query, stringToNode(nodestr));
	sql = pg_get_querydef(query, false);

	pq_begintypsend(&buf);
	pq_sendtext(&buf, sql, strlen(sql));
	PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}


/*
 * sql_query_cmp - comparison function
 *
 * Compares the nodeToString representations of two sql_query values.
 * Two queries that parse and analyze to the same Query node are equal.
 * The ordering is consistent but not guaranteed to be human-meaningful.
 */
Datum
sql_query_cmp(PG_FUNCTION_ARGS)
{
	text	   *a = PG_GETARG_TEXT_PP(0);
	text	   *b = PG_GETARG_TEXT_PP(1);
	int			result;

	/*
	 * Compare the nodeToString representations byte-for-byte using C
	 * collation.  The stored format is always ASCII, and equality means
	 * identical serialized Query trees, so locale-sensitive ordering would be
	 * both meaningless and potentially inconsistent with the hash functions.
	 */
	result = varstr_cmp(VARDATA_ANY(a), VARSIZE_ANY_EXHDR(a),
						VARDATA_ANY(b), VARSIZE_ANY_EXHDR(b),
						C_COLLATION_OID);

	PG_FREE_IF_COPY(a, 0);
	PG_FREE_IF_COPY(b, 1);

	PG_RETURN_INT32(result);
}

Datum
sql_query_eq(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) == 0);
}

Datum
sql_query_ne(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) != 0);
}

Datum
sql_query_lt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) < 0);
}

Datum
sql_query_le(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) <= 0);
}

Datum
sql_query_gt(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) > 0);
}

Datum
sql_query_ge(PG_FUNCTION_ARGS)
{
	PG_RETURN_BOOL(DatumGetInt32(DirectFunctionCall2(sql_query_cmp,
													  PG_GETARG_DATUM(0),
													  PG_GETARG_DATUM(1))) >= 0);
}

/*
 * sql_query_hash / sql_query_hash_extended
 *
 * Hash support functions for the hash opclass.  We hash the raw bytes of the
 * nodeToString representation, consistent with the C-collation comparison
 * used by sql_query_cmp.
 */
Datum
sql_query_hash(PG_FUNCTION_ARGS)
{
	text	   *val = PG_GETARG_TEXT_PP(0);
	Datum		result;

	result = hash_any((unsigned char *) VARDATA_ANY(val),
					  VARSIZE_ANY_EXHDR(val));
	PG_FREE_IF_COPY(val, 0);
	PG_RETURN_DATUM(result);
}

Datum
sql_query_hash_extended(PG_FUNCTION_ARGS)
{
	text	   *val = PG_GETARG_TEXT_PP(0);
	uint64		seed = PG_GETARG_INT64(1);
	Datum		result;

	result = hash_any_extended((unsigned char *) VARDATA_ANY(val),
							   VARSIZE_ANY_EXHDR(val), seed);
	PG_FREE_IF_COPY(val, 0);
	PG_RETURN_DATUM(result);
}
