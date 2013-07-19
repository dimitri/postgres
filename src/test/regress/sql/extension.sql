-- test case without an explicit schema
create template for extension myextension version '1.0'
    as $$ create table foobar(i int4) $$;

create extension myextension;

-- check that it went to 'public'
select nspname
  from pg_class c join pg_namespace n on n.oid = c.relnamespace
 where relname ~ 'foobar';

-- cleanup
drop extension myextension;
drop template for extension myextension version '1.0';

-- test case without an explicit schema in an upgrade path
create template for extension test version 'abc' with (nosuperuser) as $$
  create function f1(i int) returns int language sql as $_$ select 1; $_$;
$$;

create template for extension test for update from 'abc' to 'xyz' with (nosuperuser) as $$
  create function f2(i int) returns int language sql as $_$ select 1; $_$;
$$;

create template for extension test for update from 'xyz' to '123' with (nosuperuser) as $$
  create function f3(i int) returns int language sql as $_$ select 1; $_$;
$$;

create extension test version '123';

\dx+ test

-- cleanup
drop extension test;
drop template for extension test for update from 'xyz' to '123';
drop template for extension test for update from 'abc' to 'xyz';
drop template for extension test version 'abc';

-- testing dependency in between template and instanciated extensions
create template for extension deps version 'a' as '';
create template for extension deps for update from 'a' to 'b' as '';
alter template for extension deps set default version 'b';
create extension deps;
\dx
-- that should be an error
drop template for extension deps version 'a';
-- that too should be an error
drop template for extension deps for update from 'a' to 'b';

-- check that we can add a new template for directly installing version 'b'
create template for extension deps version 'b' as '';

-- and test some control parameters conflicts now
create template for extension deps for update from 'b' to 'c' as '';

-- those should all fail
create template for extension deps version 'c' with (schema foo) as '';
create template for extension deps version 'c' with (superuser) as '';
create template for extension deps version 'c' with (relocatable) as '';
create template for extension deps version 'c' with (requires 'x, y') as '';

-- that one should succeed: no conflict
create template for extension deps version 'c'
  with (schema public, nosuperuser, norelocatable) as '';

-- cleanup
drop extension deps;
drop template for extension deps version 'a' cascade;
drop template for extension deps version 'b' cascade;
drop template for extension deps version 'c' cascade;

-- check that we no longer have control entries
select * from pg_extension_control;

-- test that we can not rename a template in use
create template for extension foo version 'v' AS '';
create extension foo;
alter template for extension foo rename to bar;

drop extension foo;
drop template for extension foo version 'v';

-- now create some templates and an upgrade path
CREATE TEMPLATE
   FOR EXTENSION pair DEFAULT VERSION '1.0'
  WITH (superuser, norelocatable, schema public)
AS $$
  CREATE TYPE pair AS ( k text, v text );
  
  CREATE OR REPLACE FUNCTION pair(anyelement, text)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(text, anyelement)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(anyelement, anyelement)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(text, text)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair;';      
$$;

-- we want to test alter extension update
CREATE TEMPLATE FOR EXTENSION pair
    FOR UPDATE FROM '1.0' TO '1.1'
  WITH (superuser, norelocatable, schema public)
AS $$
  CREATE OPERATOR ~> (LEFTARG = text,
                      RIGHTARG = anyelement,
                      PROCEDURE = pair);
                      
  CREATE OPERATOR ~> (LEFTARG = anyelement,
                      RIGHTARG = text,
                      PROCEDURE = pair);

  CREATE OPERATOR ~> (LEFTARG = anyelement,
                      RIGHTARG = anyelement,
                      PROCEDURE = pair);
                      
  CREATE OPERATOR ~> (LEFTARG = text,
                      RIGHTARG = text,
                      PROCEDURE = pair);           
$$;

-- and we want to test update with more than 1 step
CREATE TEMPLATE FOR EXTENSION pair
    FOR UPDATE FROM '1.1' TO '1.2'
AS
 $$
  COMMENT ON EXTENSION pair IS 'Simple Key Value Text Type';
$$;

-- test some ALTER commands

-- ok
ALTER TEMPLATE FOR EXTENSION pair VERSION '1.0' WITH (relocatable);

-- we don't have a version 1.3 known yet
ALTER TEMPLATE FOR EXTENSION pair VERSION '1.3' WITH (relocatable);

-- you can't set the default on an upgrade script, only an extension's version
ALTER TEMPLATE FOR EXTENSION pair FOR UPDATE FROM '1.0' TO '1.1' SET DEFAULT;

-- you can't set control properties on an upgrade script, only an
-- extension's version
ALTER TEMPLATE FOR EXTENSION pair
             FOR UPDATE FROM '1.0' TO '1.1' WITH (relocatable);

-- try to set the default full version to an unknown extension version
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT FULL VERSION '1.1';

-- now set it to the current one already, should silently do nothing
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT FULL VERSION '1.0';

-- you can actually change the script used to update, though
ALTER TEMPLATE FOR EXTENSION pair FOR UPDATE FROM '1.1' TO '1.2'
AS $$
  COMMENT ON EXTENSION pair IS 'A Toy Key Value Text Type';
$$;

CREATE EXTENSION pair;

\dx pair
\dx+ pair

ALTER EXTENSION pair UPDATE TO '1.2';

\dx+ pair

DROP EXTENSION pair;

-- test with another full version that's not the default
CREATE TEMPLATE
   FOR EXTENSION pair VERSION '1.3'
  WITH (superuser, norelocatable, schema public)
AS $$
  CREATE TYPE pair AS ( k text, v text );
  
  CREATE OR REPLACE FUNCTION pair(anyelement, text)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(text, anyelement)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(anyelement, anyelement)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair';
  
  CREATE OR REPLACE FUNCTION pair(text, text)
  RETURNS pair LANGUAGE SQL AS 'SELECT ROW($1, $2)::pair;';      

  CREATE OPERATOR ~> (LEFTARG = text,
                      RIGHTARG = anyelement,
                      PROCEDURE = pair);
                      
  CREATE OPERATOR ~> (LEFTARG = anyelement,
                      RIGHTARG = text,
                      PROCEDURE = pair);

  CREATE OPERATOR ~> (LEFTARG = anyelement,
                      RIGHTARG = anyelement,
                      PROCEDURE = pair);
                      
  CREATE OPERATOR ~> (LEFTARG = text,
                      RIGHTARG = text,
                      PROCEDURE = pair);           

  COMMENT ON EXTENSION pair IS 'Simple Key Value Text Type';
$$;

-- that's ok
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT VERSION '1.1';

-- that will install 1.0 then run the 1.0 -- 1.1 update script
CREATE EXTENSION pair;
\dx pair
DROP EXTENSION pair;

-- now that should install from the extension from the 1.3 template, even if
-- we have a default_major_version pointing to 1.0, because we actually have
-- a 1.3 create script.
CREATE EXTENSION pair VERSION '1.3';
\dx pair
DROP EXTENSION pair;

-- and now let's ask for 1.3 by default while still leaving the
-- default_major_version at 1.0, so that it's possible to directly install
-- 1.2 if needed.
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT VERSION '1.3';

CREATE EXTENSION pair;
\dx pair
DROP EXTENSION pair;

CREATE EXTENSION pair VERSION '1.2';
\dx pair
DROP EXTENSION pair;

-- test owner change
CREATE ROLE regression_bob;

ALTER TEMPLATE FOR EXTENSION pair OWNER TO regression_bob;

select ctlname, rolname
  from pg_extension_control c join pg_roles r on r.oid = c.ctlowner;

-- test renaming
ALTER TEMPLATE FOR EXTENSION pair RENAME TO keyval;

-- cleanup
DROP TEMPLATE FOR EXTENSION keyval FOR UPDATE FROM '1.1' TO '1.2';
DROP TEMPLATE FOR EXTENSION keyval FOR UPDATE FROM '1.0' TO '1.1';
DROP TEMPLATE FOR EXTENSION keyval VERSION '1.0';
DROP TEMPLATE FOR EXTENSION keyval VERSION '1.3';
DROP ROLE regression_bob;
