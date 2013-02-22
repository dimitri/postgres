-- first create some templates
CREATE TEMPLATE
   FOR EXTENSION PAIR DEFAULT VERSION '1.0'
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
CREATE TEMPLATE
   FOR EXTENSION PAIR FROM '1.0' TO '1.1'
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

-- and we want to test update with a cycle
CREATE TEMPLATE
   FOR EXTENSION PAIR FROM '1.1' TO '1.2'
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
ALTER TEMPLATE FOR EXTENSION pair FROM '1.0' TO '1.1' SET DEFAULT;

-- you can't set control properties on an upgrade script, only an
-- extension's version
ALTER TEMPLATE FOR EXTENSION pair FROM '1.0' TO '1.1' WITH (relocatable);

-- try to set the default full version to an unknown extension version
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT FULL VERSION '1.1';

-- now set it to the current one already, should silently do nothing
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT FULL VERSION '1.0';


-- you can actually change the script used to update, though
ALTER TEMPLATE FOR EXTENSION PAIR FROM '1.1' TO '1.2'
AS $$
  COMMENT ON EXTENSION pair IS 'A Toy Key Value Text Type';
$$;

CREATE EXTENSION pair;

\dx pair
\dx+ pair

ALTER EXTENSION pair UPDATE TO '1.2';

\dx+ pair

DROP EXTENSION pair;


-- that's accepted
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT VERSION '1.1';

-- that will install 1.0 then run the 1.0 -- 1.1 update script
CREATE EXTENSION pair;

-- cleanup
DROP EXTENSION pair;

-- test owner change
CREATE ROLE regression_bob;

ALTER TEMPLATE FOR EXTENSION pair OWNER TO regression_bob;

select ctlname, rolname
  from pg_extension_control c join pg_roles r on r.oid = c.ctlowner;

-- test renaming
ALTER TEMPLATE FOR EXTENSION pair RENAME TO keyval;

-- cleanup
DROP TEMPLATE FOR EXTENSION keyval FROM '1.1' TO '1.2';
DROP TEMPLATE FOR EXTENSION keyval FROM '1.0' TO '1.1';
DROP TEMPLATE FOR EXTENSION keyval VERSION '1.0';
DROP ROLE regression_bob;
