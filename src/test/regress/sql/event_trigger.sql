-- should fail, return type mismatch
create event trigger regress_event_trigger
   on ddl_command_start
   execute procedure pg_backend_pid();

-- OK
create function test_event_trigger() returns event_trigger as $$
BEGIN
    RAISE NOTICE 'test_event_trigger: % % %', tg_event, tg_context, tg_tag;
    RAISE NOTICE 'test_event_trigger: %, %', tg_operation, tg_obtypename;
    RAISE NOTICE 'test_event_trigger: %', tg_command;
END
$$ language plpgsql;

-- should fail, no elephant_bootstrap entry point
create event trigger regress_event_trigger on elephant_bootstrap
   execute procedure test_event_trigger();

-- OK
create event trigger regress_event_trigger_start on ddl_command_start
   execute procedure test_event_trigger();

create event trigger regress_event_trigger_trace on ddl_command_trace
   when tag in ('create table')
   execute procedure test_event_trigger();

-- test creating an event trigger on ddl_command_end
create event trigger regress_event_trigger_end on ddl_command_end
   when context in ('toplevel', 'generated')
   execute procedure test_event_trigger();

-- but we won't actually use it
drop event trigger regress_event_trigger_end;

-- should fail, food is not a valid filter variable
create event trigger regress_event_trigger2 on ddl_command_start
   when food in ('sandwhich')
   execute procedure test_event_trigger();

-- should fail, sandwhich is not a valid command tag
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('sandwhich')
   execute procedure test_event_trigger();

-- should fail, sandwhich is not a valid command context either
create event trigger regress_event_trigger2 on ddl_command_start
   when context in ('sandwhich')
   execute procedure test_event_trigger();

-- should fail, create skunkcabbage is not a valid comand tag
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table', 'create skunkcabbage')
   execute procedure test_event_trigger();

-- should fail, skunkcabbage is not a valid comand context
create event trigger regress_event_trigger2 on ddl_command_start
   when context in ('toplevel', 'skunkcabbage')
   execute procedure test_event_trigger();

-- should fail, can't have event triggers on event triggers
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('DROP EVENT TRIGGER')
   execute procedure test_event_trigger();

-- should fail, can't have same filter variable twice
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table') and tag in ('CREATE FUNCTION')
   execute procedure test_event_trigger();

-- OK
create event trigger regress_event_trigger2 on ddl_command_start
   when tag in ('create table', 'CREATE FUNCTION')
    and context in ('toplevel', 'generated', 'query', 'subcommand')
   execute procedure test_event_trigger();

-- OK
comment on event trigger regress_event_trigger_start is 'test comment';

-- should fail, event triggers are not schema objects
comment on event trigger wrong.regress_event_trigger_start is 'test comment';

-- drop as non-superuser should fail
create role regression_bob;
set role regression_bob;
create event trigger regress_event_trigger_noperms on ddl_command_start
   execute procedure test_event_trigger();
reset role;

-- all OK
alter event trigger regress_event_trigger_start enable replica;
alter event trigger regress_event_trigger_start enable always;
alter event trigger regress_event_trigger_start enable;
alter event trigger regress_event_trigger_start disable;

-- regress_event_trigger2 should fire, but not regress_event_trigger
create table event_trigger_fire1 (a int);

-- but nothing should fire here
drop table event_trigger_fire1;

-- alter owner to non-superuser should fail
alter event trigger regress_event_trigger_start owner to regression_bob;

-- alter owner to superuser should work
alter role regression_bob superuser;
alter event trigger regress_event_trigger_start owner to regression_bob;

-- should fail, name collision
alter event trigger regress_event_trigger_start rename to regress_event_trigger2;

-- OK
alter event trigger regress_event_trigger_start rename to regress_event_trigger3;

-- should fail, doesn't exist any more
drop event trigger regress_event_trigger;

-- should fail, regression_bob owns regress_event_trigger2/3
drop role regression_bob;

-- these are all OK; the second one should emit a NOTICE
drop event trigger if exists regress_event_trigger2;
drop event trigger if exists regress_event_trigger2;
drop event trigger regress_event_trigger3;
drop event trigger regress_event_trigger_trace;
drop function test_event_trigger();
drop role regression_bob;
