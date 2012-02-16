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

create trigger snitch_some_more
         after command create table, alter table, create function
       execute procedure snitch();

create schema cmd;
create table cmd.foo(id bigserial primary key);
create view cmd.v as select * from cmd.foo;
alter table cmd.foo add column t text;
create index on cmd.foo(t);

create function cmd.fun(int) returns text language sql
as $$ select t from cmd.foo where id = $1; $$;

drop schema cmd cascade;

drop trigger snitch_before on any command;
drop trigger snitch_after  on any command;
drop trigger snitch_some_more on command create table, alter table, create function;
