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
ALTER TEMPLATE FOR EXTENSION PAIR VERSION '1.0' WITH (relocatable);

-- we don't have a version 1.3 known yet
ALTER TEMPLATE FOR EXTENSION PAIR VERSION '1.3' WITH (relocatable);

-- you can't set the default on an upgrade script, only an extension's version
ALTER TEMPLATE FOR EXTENSION PAIR FROM '1.0' TO '1.1' SET DEFAULT;

-- you can't set control properties on an upgrade script, only an
-- extension's version
ALTER TEMPLATE FOR EXTENSION PAIR FROM '1.0' TO '1.1' WITH (relocatable);

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

-- error, we don't have control settings for 1.2
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT VERSION '1.2';

-- that's accepted
ALTER TEMPLATE FOR EXTENSION pair SET DEFAULT VERSION '1.1';

-- but we don't know how to apply 1.0 -- 1.1 at install yet
CREATE EXTENSION pair;

\dx pair


