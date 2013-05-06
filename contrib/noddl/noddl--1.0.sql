/* contrib/lo/lo--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION lo" to load this file. \quit

CREATE FUNCTION noddl()
        RETURNS pg_catalog.event_trigger
             AS 'noddl'
             LANGUAGE C;

CREATE EVENT TRIGGER noddl on ddl_command_start
   execute procedure noddl();
