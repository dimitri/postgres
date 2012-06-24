--
-- EVENT TRIGGERS
--
create or replace function snitch()
 returns event_trigger
 language plpgsql
as $$
begin
  -- can't output tg_objectid here that would break pg_regress
  raise notice 'snitch: % % %.%', tg_when, tg_tag, tg_schemaname, tg_objectname;
end;
$$;

--
-- TODO: REASSIGN OWNED and DROP OWNED
--

create event trigger any_t
              before command_start
   execute procedure snitch();

create event trigger foo_t
              before command_start
                when tag in ('alter collation',
                             'alter conversion',
                             'alter domain',
                             'alter function',
                             'alter operator',
                             'alter schema',
                             'alter sequence',
                             'alter table',
                             'alter trigger',
                             'alter type',
                             'alter view',
                             'create aggregate',
                             'create cast',
                             'create collation',
                             'create domain',
                             'create function',
                             'create operator class',
                             'create operator',
                             'create schema',
                             'create sequence',
                             'create table as',
                             'create table',
                             'create text search configuration',
                             'create text search dictionary',
                             'create text search parser',
                             'create text search template',
                             'create trigger',
                             'create type',
                             'create view',
                             'drop aggregate',
                             'drop domain',
                             'drop schema',
                             'drop table',
                             'drop text search configuration',
                             'drop text search dictionary',
                             'drop text search parser',
                             'drop text search template',
                             'drop trigger',
                             'reindex',
                             'select into',
                             'vacuum')
   execute procedure snitch();

alter event trigger foo_t disable;
alter event trigger foo_t enable;
alter event trigger foo_t rename to snitch;

create schema cmd;
create schema cmd2;
create role regbob;

create table cmd.foo(id bigserial primary key);
create view cmd.v as select * from cmd.foo;
alter table cmd.foo add column t text;

create table cmd.bar as select 1;
drop table cmd.bar;
select 1 into cmd.bar;
drop table cmd.bar;

create table test9 (id int, stuff text);
alter table test9 rename to test;
alter table test set schema cmd;
alter table cmd.test rename column stuff to things;
alter table cmd.test add column alpha text;
alter table cmd.test alter column alpha set data type varchar(300);
alter table cmd.test alter column alpha set default 'test';
alter table cmd.test alter column alpha drop default;
alter table cmd.test alter column alpha set statistics 78;
alter table cmd.test alter column alpha set storage plain;
alter table cmd.test alter column alpha set not null;
alter table cmd.test alter column alpha drop not null;
alter table cmd.test alter column alpha set (n_distinct = -0.78);
alter table cmd.test alter column alpha reset (n_distinct);
alter table cmd.test drop column alpha;
alter table cmd.test add check (id > 2) not valid;
alter table cmd.test add check (id < 800000);
alter table cmd.test set without cluster;
alter table cmd.test set with oids;
alter table cmd.test set without oids;

create sequence test_seq_;
alter sequence test_seq_ owner to regbob;
alter sequence test_seq_ rename to test_seq;
alter sequence test_seq set schema cmd;
alter sequence cmd.test_seq start with 3;
alter sequence cmd.test_seq restart with 4;
alter sequence cmd.test_seq minvalue 3;
alter sequence cmd.test_seq no minvalue;
alter sequence cmd.test_seq maxvalue 900000;
alter sequence cmd.test_seq no maxvalue;
alter sequence cmd.test_seq cache 876;
alter sequence cmd.test_seq cycle;
alter sequence cmd.test_seq no cycle;

create view view_test as select id, things from cmd.test;
alter view view_test owner to regbob;
alter view view_test rename to view_test2;
alter view view_test2 set schema cmd;
alter view cmd.view_test2 alter column id set default 9;
alter view cmd.view_test2 alter column id drop default;

cluster cmd.foo using foo_pkey;
vacuum cmd.foo;
vacuum;
reindex table cmd.foo;

set session_replication_role to replica;
create table cmd.bar();
reset session_replication_role;

create index idx_foo on cmd.foo(t);
reindex index cmd.idx_foo;
drop index cmd.idx_foo;

create function fun(int) returns text language sql
as $$ select t from cmd.foo where id = $1; $$;

alter function fun(int) strict;
alter function fun(int) rename to notfun;
alter function notfun(int) set schema cmd;
alter function cmd.notfun(int) owner to regbob;
alter function cmd.notfun(int) cost 77;
drop function cmd.notfun(int);

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

create type cmd.type_test AS (a integer, b integer, c text);
alter type cmd.type_test owner to regbob;
alter type cmd.type_test rename to type_test2;
alter type cmd.type_test2 set schema public;
alter type public.type_test2 rename attribute a to z;
alter type public.type_test2 add attribute alpha text;
alter type public.type_test2 alter attribute alpha set data type char(90);
alter type public.type_test2 drop attribute alpha;

drop type cmd.compfoo;
drop type public.type_test2;

create type cmd.bug_status as enum ('new', 'open', 'closed');
alter type cmd.bug_status add value 'wontfix';

create domain cmd.us_postal_code as text check(value ~ '^\d{5}$' or value ~ '^\d{5}-\d{4}$');
alter domain cmd.us_postal_code set not null;
alter domain cmd.us_postal_code set default 90210;
alter domain cmd.us_postal_code drop default;
alter domain cmd.us_postal_code drop not null;
alter domain cmd.us_postal_code add constraint dummy_constraint check (value ~ '^\d{8}$');
alter domain cmd.us_postal_code drop constraint dummy_constraint;
alter domain cmd.us_postal_code owner to regbob;
alter domain cmd.us_postal_code set schema cmd2;
drop domain cmd2.us_postal_code;

create function cmd.trigfunc() returns trigger language plpgsql as
$$ begin raise notice 'trigfunc';  end;$$;

create trigger footg before update on cmd.foo for each row execute procedure cmd.trigfunc();
alter trigger footg on cmd.foo rename to foo_trigger;
drop trigger foo_trigger on cmd.foo;

create conversion test for 'utf8' to 'sjis' from utf8_to_sjis;
create default conversion test2 for 'utf8' to 'sjis' from utf8_to_sjis;
alter conversion test2 rename to test3;
drop conversion test3;
drop conversion test;

create operator class test_op_class
   for type anyenum using hash as
   operator 1  =,
   function 1  hashenum(anyenum);

create text search configuration test (parser = "default");

create text search dictionary test_stem (
   template = snowball,
   language = 'english', stopwords = 'english'
);
alter text search dictionary test_stem (StopWords = dutch );

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

drop text search configuration test;
drop text search dictionary test_stem;
drop text search parser test_parser;
drop text search template test_template;

create function cmd.testcast(text) returns int4  language plpgsql as $$begin return 4::int4;end;$$;
create cast (text as int4) with function cmd.testcast(text) as assignment;

alter schema cmd rename to cmd1;

drop schema cmd1 cascade;
drop schema cmd2 cascade;
drop role regbob;

drop event trigger any_t;
drop event trigger snitch;
