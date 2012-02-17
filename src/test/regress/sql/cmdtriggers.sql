--
-- COMMAND TRIGGERS
--
create or replace function snitch
 (in tg_when text, in cmd_tag text, in objectid oid, in schemaname text, in objectname text)
 returns void language plpgsql
as $$
begin
  -- can't output the objectid here that would break pg_regress
  raise notice 'snitch: % % % %', tg_when, cmd_tag, schemaname, objectname;
end;
$$;

create trigger snitch_before before any command execute procedure snitch();
create trigger snitch_after  after  any command execute procedure snitch();

alter trigger snitch_before on any command set disable;
alter trigger snitch_before on any command set enable;

create trigger snitch_some_more
         after command create table, alter table, drop table,
	               create function, create collation,
		       alter operator, create domain, alter schema
       execute procedure snitch();

create trigger snitch_some_even_more
        before command create trigger, alter trigger, drop trigger,
	       	       create schema, drop schema,
	               create aggregate, alter collation, create operator,
		       alter domain, create type, alter type
       execute procedure snitch();

create schema cmd;
create table cmd.foo(id bigserial primary key);
create view cmd.v as select * from cmd.foo;
alter table cmd.foo add column t text;

create index idx_foo on cmd.foo(t);
drop index cmd.idx_foo;

create function cmd.fun(int) returns text language sql
as $$ select t from cmd.foo where id = $1; $$;

alter function cmd.fun(int) strict;
alter function cmd.fun(int) rename to notfun;
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
drop type cmd.compfoo;

create type cmd.bug_status as enum ('new', 'open', 'closed');
alter type cmd.bug_status add value 'wontfix';

create domain cmd.us_postal_code as text check(value ~ '^\d{5}$' or value ~ '^\d{5}-\d{4}$');
alter domain cmd.us_postal_code set not null;

create function cmd.trigfunc() returns trigger language plpgsql as
$$ begin raise notice 'trigfunc';  end;$$;

create trigger footg before update on cmd.foo for each row execute procedure cmd.trigfunc();
alter trigger footg on cmd.foo rename to foo_trigger;
drop trigger foo_trigger on cmd.foo;

alter schema cmd rename to cmd1;

drop schema cmd1 cascade;

drop trigger snitch_before on any command;
drop trigger snitch_after  on any command;
drop trigger snitch_some_more
  on command create table, alter table, drop table,
     	     create function, create collation,
	     alter operator, create domain, alter schema;

drop trigger snitch_some_even_more
  on command create trigger, alter trigger, drop trigger,
	     create schema, drop schema,
	     create aggregate, alter collation, create operator,
             alter domain, create type, alter type;

