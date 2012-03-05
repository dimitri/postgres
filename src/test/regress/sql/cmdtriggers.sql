--
-- COMMAND TRIGGERS
--
create or replace function any_snitch
 (in tg_when text, in cmd_tag text, in objectid oid, in schemaname text, in objectname text)
 returns void language plpgsql
as $$
begin
  -- can't output the objectid here that would break pg_regress
  -- don't output objectname and schemaname, NULL in an ANY command trigger
  raise notice 'snitch: % any %', tg_when, cmd_tag;
end;
$$;

create or replace function snitch
 (in tg_when text, in cmd_tag text, in objectid oid, in schemaname text, in objectname text)
 returns void language plpgsql
as $$
begin
  -- can't output the objectid here that would break pg_regress
  raise notice 'snitch: % % % %', tg_when, cmd_tag, schemaname, objectname;
end;
$$;

create command trigger snitch_before before any command execute procedure any_snitch();
create command trigger snitch_after_ after  any command execute procedure any_snitch();

alter command trigger snitch_before set disable;
alter command trigger snitch_before set enable;
alter command trigger snitch_after_ rename to snitch_after;

create command trigger snitch_create_table after create table execute procedure snitch();
create command trigger snitch_create_view after create view execute procedure snitch();
create command trigger snitch_alter_table after alter table execute procedure snitch();
create command trigger snitch_drop_table after drop table execute procedure snitch();
create command trigger snitch_create_function after create function execute procedure snitch();
create command trigger snitch_create_collation after create collation execute procedure snitch();
create command trigger snitch_alter_operator after alter operator execute procedure snitch();
create command trigger snitch_create_domain after create domain execute procedure snitch();
create command trigger snitch_alter_schema after alter schema execute procedure snitch();
create command trigger snitch_create_tsconfig after create text search configuration execute procedure snitch();
create command trigger snitch_create_tsdict after create text search dictionary execute procedure snitch();
create command trigger snitch_create_tsparser after create text search parser execute procedure snitch();
create command trigger snitch_create_tstmpl after create text search template execute procedure snitch();
create command trigger snitch_after_alter_function after alter function execute procedure snitch();
create command trigger snitch_create_cast after create cast execute procedure snitch();

create command trigger snitch_create_trigger before create trigger execute procedure snitch();
create command trigger snitch_alter_trigger before alter trigger execute procedure snitch();
create command trigger snitch_drop_trigger before drop trigger execute procedure snitch();
create command trigger snitch_create_schema before create schema execute procedure snitch();
create command trigger snitch_drop_schema before drop schema execute procedure snitch();
create command trigger snitch_create_aggregate before create aggregate execute procedure snitch();
create command trigger snitch_alter_collation before alter collation execute procedure snitch();
create command trigger snitch_create_operator before create operator execute procedure snitch();
create command trigger snitch_alter_domain before alter domain execute procedure snitch();
create command trigger snitch_create_type before create type execute procedure snitch();
create command trigger snitch_alter_type before alter type execute procedure snitch();
create command trigger snitch_before_alter_function before alter function execute procedure snitch();

create schema cmd;
create schema cmd2;
create role regbob;

create table cmd.foo(id bigserial primary key);
create view cmd.v as select * from cmd.foo;
alter table cmd.foo add column t text;

cluster cmd.foo using foo_pkey;
vacuum cmd.foo;
vacuum;

set session_replication_role to replica;
create table cmd.bar();
reset session_replication_role;

create index idx_foo on cmd.foo(t);
drop index cmd.idx_foo;

create function cmd.fun(int) returns text language sql
as $$ select t from cmd.foo where id = $1; $$;

alter function cmd.fun(int) strict;
alter function cmd.fun(int) rename to notfun;
alter function cmd.notfun(int) set schema public;
drop function public.notfun(int);

create function cmd.plus1(int) returns bigint language sql
as $$ select $1::bigint + 1; $$;

create operator cmd.+!(procedure = cmd.plus1, leftarg = int);
alter operator cmd.+!(int, NONE) set schema public;
drop operator public.+!(int, NONE);

create aggregate cmd.avg (float8)
(
    sfunc = float8_accum,
    stype = float8[],
    finalfunc = float8_avg,
    initcond = '{0,0,0}'
);
alter aggregate cmd.avg(float8) set schema public;
drop aggregate public.avg(float8);

create collation cmd.french (LOCALE = 'fr_FR');
alter collation cmd.french rename to francais;

create type cmd.compfoo AS (f1 int, f2 text);
alter type cmd.compfoo add attribute f3 text;
drop type cmd.compfoo;

create type cmd.bug_status as enum ('new', 'open', 'closed');
alter type cmd.bug_status add value 'wontfix';

create domain cmd.us_postal_code as text check(value ~ '^\d{5}$' or value ~ '^\d{5}-\d{4}$');
alter domain cmd.us_postal_code set not null;
alter domain cmd.us_postal_code set default 90210;
alter domain cmd.us_postal_code drop default;
alter domain cmd.us_postal_code set not null;
alter domain cmd.us_postal_code drop not null;
alter domain cmd.us_postal_code add constraint dummy_constraint check (value ~ '^\d{8}$');
alter domain cmd.us_postal_code drop constraint dummy_constraint;
alter domain cmd.us_postal_code owner to regbob;
alter domain cmd.us_postal_code set schema cmd2;

create function cmd.trigfunc() returns trigger language plpgsql as
$$ begin raise notice 'trigfunc';  end;$$;

create trigger footg before update on cmd.foo for each row execute procedure cmd.trigfunc();
alter trigger footg on cmd.foo rename to foo_trigger;
drop trigger foo_trigger on cmd.foo;

create text search configuration test (parser = "default");

create text search dictionary test_stem (
   template = snowball,
   language = 'english', stopwords = 'english'
);

create text search parser test_parser (
  start = prsd_start,
  gettoken = prsd_nexttoken,
  end = prsd_end,
  lextypes = prsd_lextype,
  headline = prsd_headline
);

create text search template test_template (
  init = dsimple_init,
  lexize = dsimple_lexize
);

create function cmd.testcast(text) returns int4  language plpgsql as $$begin return 4::int4;end;$$;
create cast (text as int4) with function cmd.testcast(text) as assignment;

alter schema cmd rename to cmd1;

drop schema cmd1 cascade;
drop schema cmd2 cascade;
drop role regbob;

drop command trigger snitch_before;
drop command trigger snitch_after;

drop command trigger snitch_create_table;
drop command trigger snitch_create_view;
drop command trigger snitch_alter_table;
drop command trigger snitch_drop_table;
drop command trigger snitch_create_function;
drop command trigger snitch_create_collation;
drop command trigger snitch_alter_operator;
drop command trigger snitch_create_domain;
drop command trigger snitch_alter_schema;
drop command trigger snitch_create_tsconfig;
drop command trigger snitch_create_tsdict;
drop command trigger snitch_create_tsparser;
drop command trigger snitch_create_tstmpl;
drop command trigger snitch_after_alter_function;
drop command trigger snitch_create_cast;

drop command trigger snitch_create_trigger;
drop command trigger snitch_alter_trigger;
drop command trigger snitch_drop_trigger;
drop command trigger snitch_create_schema;
drop command trigger snitch_drop_schema;
drop command trigger snitch_create_aggregate;
drop command trigger snitch_alter_collation;
drop command trigger snitch_create_operator;
drop command trigger snitch_alter_domain;
drop command trigger snitch_create_type;
drop command trigger snitch_alter_type;
drop command trigger snitch_before_alter_function;
