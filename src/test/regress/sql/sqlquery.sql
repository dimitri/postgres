--
-- Tests for the sql_query data type
--
-- sql_query stores a fully analyzed SQL query in nodeToString() format.
-- Its key property is that deparsing under an empty search_path emits
-- schema-qualified names, fixing pg_restore's REFRESH MATERIALIZED VIEW
-- failure when ts_stat() references unqualified table names.
--

-- Verify type and operator catalog entries

SELECT typname, typcategory, typinput::regproc, typoutput::regproc
FROM pg_type WHERE typname = 'sql_query';

SELECT oprname, oprleft::regtype, oprright::regtype, oprresult::regtype
FROM pg_operator WHERE oprleft = 'sql_query'::regtype
ORDER BY oprname;

SELECT amopstrategy, amoplefttype::regtype, amoprighttype::regtype
FROM pg_amop
WHERE amopfamily = (
  SELECT oid FROM pg_opfamily
  WHERE opfmethod = (SELECT oid FROM pg_am WHERE amname = 'btree')
    AND opfname = 'sql_query_ops'
)
ORDER BY amopstrategy;

-- I/O: canonical SQL produced by pg_get_querydef()

SELECT 'SELECT 1 + 1'::sql_query;
SELECT 'SELECT 1+1'::sql_query;            -- normalised to same form
SELECT 'select 1+1'::sql_query;            -- case-insensitive SQL keyword

-- Multiple-statement input is rejected

SELECT 'SELECT 1; SELECT 2'::sql_query;

-- Invalid SQL is rejected

SELECT 'NOT VALID SQL'::sql_query;

-- Non-SELECT statements are accepted (any fully-parseable statement)

SELECT 'UPDATE pg_class SET relname = relname WHERE false'::sql_query;

-- Arrays of sql_query

SELECT ARRAY['SELECT 1'::sql_query, 'SELECT 2'::sql_query];

-- Equality and comparison operators

SELECT 'SELECT 1 + 1'::sql_query = 'SELECT 1+1'::sql_query AS equal;
SELECT 'SELECT 1'::sql_query = 'SELECT 2'::sql_query AS not_equal;
SELECT 'SELECT 1'::sql_query < 'SELECT 2'::sql_query AS less_than;
SELECT 'SELECT 2'::sql_query > 'SELECT 1'::sql_query AS greater_than;

-- ORDER BY uses the B-tree opclass

SELECT q FROM
  (VALUES ('SELECT 3'::sql_query), ('SELECT 1'::sql_query), ('SELECT 2'::sql_query)) v(q)
ORDER BY q;

-- DISTINCT: synonymous queries collapse to one row

SELECT DISTINCT q FROM
  (VALUES ('SELECT 1+1'::sql_query), ('SELECT 1 + 1'::sql_query), ('SELECT 2'::sql_query)) v(q)
ORDER BY q;

-- text ↔ sql_query casts: both directions are implicit

SELECT 'SELECT 1'::sql_query::text;        -- sql_query → text (implicit)
SELECT CAST('SELECT 1' AS sql_query);      -- text → sql_query (implicit)

-- ts_stat with sql_query
--
-- String literals are untyped; with only ts_stat(sql_query) available,
-- function type resolution coerces them to sql_query automatically via
-- sql_query_in().  No explicit cast is needed, preserving backward
-- compatibility with existing ts_stat('...') call sites.

DROP TABLE IF EXISTS sqlquery_articles;
CREATE TABLE sqlquery_articles (body text);
INSERT INTO sqlquery_articles VALUES
  ('the cat sat on the mat'),
  ('cats are great');

SELECT word, ndoc, nentry
FROM ts_stat(
  $$ SELECT to_tsvector('english', body) FROM sqlquery_articles $$
)
ORDER BY ndoc DESC, word;

-- Weights filter: 'abcd' includes all weights including the default 'D'

SELECT word, ndoc, nentry
FROM ts_stat(
  $$ SELECT to_tsvector('english', body) FROM sqlquery_articles $$,
  'abcd'
)
ORDER BY ndoc DESC, word;

-- Materialized view using ts_stat(sql_query) and its dependencies
--
-- After the matview is created, the dependency tracker must have recorded
-- sqlquery_articles as a dependency of the rewrite rule (via the sql_query
-- constant in the ts_stat() argument).

CREATE MATERIALIZED VIEW sqlquery_word_stats AS
  SELECT word, ndoc, nentry
  FROM ts_stat(
    $$ SELECT to_tsvector('english', body) FROM sqlquery_articles $$
  )
  ORDER BY ndoc DESC, word;

SELECT * FROM sqlquery_word_stats;

-- Dependency must include sqlquery_articles

SELECT d.refobjid::regclass AS dep
FROM pg_rewrite r
JOIN pg_depend d ON d.classid = 'pg_rewrite'::regclass AND d.objid = r.oid
WHERE r.ev_class = 'sqlquery_word_stats'::regclass
  AND d.refclassid = 'pg_class'::regclass
  AND d.refobjid <> 'sqlquery_word_stats'::regclass
ORDER BY dep;

-- Primary fix: REFRESH under empty search_path must succeed.
--
-- pg_restore uses an empty search_path when it runs REFRESH MATERIALIZED VIEW
-- for security.  With the old text-based ts_stat, the unqualified table name
-- "sqlquery_articles" cannot be resolved in that context.  With sql_query,
-- pg_get_querydef() emits the schema-qualified name "public.sqlquery_articles"
-- so SPI_prepare() succeeds.

SET search_path = '';
REFRESH MATERIALIZED VIEW public.sqlquery_word_stats;
RESET search_path;

SELECT * FROM sqlquery_word_stats;

-- Cleanup

DROP MATERIALIZED VIEW sqlquery_word_stats;
DROP TABLE sqlquery_articles;
