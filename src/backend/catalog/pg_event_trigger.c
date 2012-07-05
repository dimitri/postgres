/*-------------------------------------------------------------------------
 *
 * pg_event_trigger.c
 *	  routines to support manipulation of the pg_event_trigger relation
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/pg_event_trigger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_event_trigger.h"
#include "utils/builtins.h"

char *
event_to_string(TrigEvent event)
{
	switch (event)
	{
		case E_CommandStart:
			return "command_start";
	}
	return NULL;
}

TrigEvent
parse_event_name(char *event)
{
	if (pg_strcasecmp(event, "command_start") == 0)
		return E_CommandStart;
	else
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("unrecognized event \"%s\"", event)));

	/* make compiler happy */
	return -1;
}

TrigEventCommand
parse_event_tag(char *command, bool noerror)
{
	if (pg_strcasecmp(command, "ALTER AGGREGATE") == 0)
		return E_AlterAggregate;
	else if (pg_strcasecmp(command, "ALTER COLLATION") == 0)
		return E_AlterCollation;
	else if (pg_strcasecmp(command, "ALTER CONVERSION") == 0)
		return E_AlterConversion;
	else if (pg_strcasecmp(command, "ALTER DOMAIN") == 0)
		return E_AlterDomain;
	else if (pg_strcasecmp(command, "ALTER EXTENSION") == 0)
		return E_AlterExtension;
	else if (pg_strcasecmp(command, "ALTER FOREIGN DATA WRAPPER") == 0)
		return E_AlterForeignDataWrapper;
	else if (pg_strcasecmp(command, "ALTER FOREIGN TABLE") == 0)
		return E_AlterForeignTable;
	else if (pg_strcasecmp(command, "ALTER FUNCTION") == 0)
		return E_AlterFunction;
	else if (pg_strcasecmp(command, "ALTER LANGUAGE") == 0)
		return E_AlterLanguage;
	else if (pg_strcasecmp(command, "ALTER OPERATOR") == 0)
		return E_AlterOperator;
	else if (pg_strcasecmp(command, "ALTER OPERATOR CLASS") == 0)
		return E_AlterOperatorClass;
	else if (pg_strcasecmp(command, "ALTER OPERATOR FAMILY") == 0)
		return E_AlterOperatorFamily;
	else if (pg_strcasecmp(command, "ALTER SEQUENCE") == 0)
		return E_AlterSequence;
	else if (pg_strcasecmp(command, "ALTER SERVER") == 0)
		return E_AlterServer;
	else if (pg_strcasecmp(command, "ALTER SCHEMA") == 0)
		return E_AlterSchema;
	else if (pg_strcasecmp(command, "ALTER TABLE") == 0)
		return E_AlterTable;
	else if (pg_strcasecmp(command, "ALTER TEXT SEARCH CONFIGURATION") == 0)
		return E_AlterTextSearchConfiguration;
	else if (pg_strcasecmp(command, "ALTER TEXT SEARCH DICTIONARY") == 0)
		return E_AlterTextSearchDictionary;
	else if (pg_strcasecmp(command, "ALTER TEXT SEARCH PARSER") == 0)
		return E_AlterTextSearchParser;
	else if (pg_strcasecmp(command, "ALTER TEXT SEARCH TEMPLATE") == 0)
		return E_AlterTextSearchTemplate;
	else if (pg_strcasecmp(command, "ALTER TRIGGER") == 0)
		return E_AlterTrigger;
	else if (pg_strcasecmp(command, "ALTER TYPE") == 0)
		return E_AlterType;
	else if (pg_strcasecmp(command, "ALTER USER MAPPING") == 0)
		return E_AlterUserMapping;
	else if (pg_strcasecmp(command, "ALTER VIEW") == 0)
		return E_AlterView;
	else if (pg_strcasecmp(command, "CLUSTER") == 0)
		return E_Cluster;
	else if (pg_strcasecmp(command, "CREATE AGGREGATE") == 0)
		return E_CreateAggregate;
	else if (pg_strcasecmp(command, "CREATE CAST") == 0)
		return E_CreateCast;
	else if (pg_strcasecmp(command, "CREATE COLLATION") == 0)
		return E_CreateCollation;
	else if (pg_strcasecmp(command, "CREATE CONVERSION") == 0)
		return E_CreateConversion;
	else if (pg_strcasecmp(command, "CREATE DOMAIN") == 0)
		return E_CreateDomain;
	else if (pg_strcasecmp(command, "CREATE EXTENSION") == 0)
		return E_CreateExtension;
	else if (pg_strcasecmp(command, "CREATE FOREIGN DATA WRAPPER") == 0)
		return E_CreateForeignDataWrapper;
	else if (pg_strcasecmp(command, "CREATE FOREIGN TABLE") == 0)
		return E_CreateForeignTable;
	else if (pg_strcasecmp(command, "CREATE FUNCTION") == 0)
		return E_CreateFunction;
	else if (pg_strcasecmp(command, "CREATE INDEX") == 0)
		return E_CreateIndex;
	else if (pg_strcasecmp(command, "CREATE LANGUAGE") == 0)
		return E_CreateLanguage;
	else if (pg_strcasecmp(command, "CREATE OPERATOR") == 0)
		return E_CreateOperator;
	else if (pg_strcasecmp(command, "CREATE OPERATOR CLASS") == 0)
		return E_CreateOperatorClass;
	else if (pg_strcasecmp(command, "CREATE OPERATOR FAMILY") == 0)
		return E_CreateOperatorFamily;
	else if (pg_strcasecmp(command, "CREATE RULE") == 0)
		return E_CreateRule;
	else if (pg_strcasecmp(command, "CREATE SEQUENCE") == 0)
		return E_CreateSequence;
	else if (pg_strcasecmp(command, "CREATE SERVER") == 0)
		return E_CreateServer;
	else if (pg_strcasecmp(command, "CREATE SCHEMA") == 0)
		return E_CreateSchema;
	else if (pg_strcasecmp(command, "CREATE TABLE") == 0)
		return E_CreateTable;
	else if (pg_strcasecmp(command, "CREATE TABLE AS") == 0)
		return E_CreateTableAs;
	else if (pg_strcasecmp(command, "CREATE TEXT SEARCH CONFIGURATION") == 0)
		return E_CreateTextSearchConfiguration;
	else if (pg_strcasecmp(command, "CREATE TEXT SEARCH DICTIONARY") == 0)
		return E_CreateTextSearchDictionary;
	else if (pg_strcasecmp(command, "CREATE TEXT SEARCH PARSER") == 0)
		return E_CreateTextSearchParser;
	else if (pg_strcasecmp(command, "CREATE TEXT SEARCH TEMPLATE") == 0)
		return E_CreateTextSearchTemplate;
	else if (pg_strcasecmp(command, "CREATE TRIGGER") == 0)
		return E_CreateTrigger;
	else if (pg_strcasecmp(command, "CREATE TYPE") == 0)
		return E_CreateType;
	else if (pg_strcasecmp(command, "CREATE USER MAPPING") == 0)
		return E_CreateUserMapping;
	else if (pg_strcasecmp(command, "CREATE VIEW") == 0)
		return E_CreateView;
	else if (pg_strcasecmp(command, "DROP AGGREGATE") == 0)
		return E_DropAggregate;
	else if (pg_strcasecmp(command, "DROP CAST") == 0)
		return E_DropCast;
	else if (pg_strcasecmp(command, "DROP COLLATION") == 0)
		return E_DropCollation;
	else if (pg_strcasecmp(command, "DROP CONVERSION") == 0)
		return E_DropConversion;
	else if (pg_strcasecmp(command, "DROP DOMAIN") == 0)
		return E_DropDomain;
	else if (pg_strcasecmp(command, "DROP EXTENSION") == 0)
		return E_DropExtension;
	else if (pg_strcasecmp(command, "DROP FOREIGN DATA WRAPPER") == 0)
		return E_DropForeignDataWrapper;
	else if (pg_strcasecmp(command, "DROP FOREIGN TABLE") == 0)
		return E_DropForeignTable;
	else if (pg_strcasecmp(command, "DROP FUNCTION") == 0)
		return E_DropFunction;
	else if (pg_strcasecmp(command, "DROP INDEX") == 0)
		return E_DropIndex;
	else if (pg_strcasecmp(command, "DROP LANGUAGE") == 0)
		return E_DropLanguage;
	else if (pg_strcasecmp(command, "DROP OPERATOR") == 0)
		return E_DropOperator;
	else if (pg_strcasecmp(command, "DROP OPERATOR CLASS") == 0)
		return E_DropOperatorClass;
	else if (pg_strcasecmp(command, "DROP OPERATOR FAMILY") == 0)
		return E_DropOperatorFamily;
	else if (pg_strcasecmp(command, "DROP RULE") == 0)
		return E_DropRule;
	else if (pg_strcasecmp(command, "DROP SCHEMA") == 0)
		return E_DropSchema;
	else if (pg_strcasecmp(command, "DROP SEQUENCE") == 0)
		return E_DropSequence;
	else if (pg_strcasecmp(command, "DROP SERVER") == 0)
		return E_DropServer;
	else if (pg_strcasecmp(command, "DROP TABLE") == 0)
		return E_DropTable;
	else if (pg_strcasecmp(command, "DROP TEXT SEARCH CONFIGURATION") == 0)
		return E_DropTextSearchConfiguration;
	else if (pg_strcasecmp(command, "DROP TEXT SEARCH DICTIONARY") == 0)
		return E_DropTextSearchDictionary;
	else if (pg_strcasecmp(command, "DROP TEXT SEARCH PARSER") == 0)
		return E_DropTextSearchParser;
	else if (pg_strcasecmp(command, "DROP TEXT SEARCH TEMPLATE") == 0)
		return E_DropTextSearchTemplate;
	else if (pg_strcasecmp(command, "DROP TRIGGER") == 0)
		return E_DropTrigger;
	else if (pg_strcasecmp(command, "DROP TYPE") == 0)
		return E_DropType;
	else if (pg_strcasecmp(command, "DROP USER MAPPING") == 0)
		return E_DropUserMapping;
	else if (pg_strcasecmp(command, "DROP VIEW") == 0)
		return E_DropView;
	else if (pg_strcasecmp(command, "LOAD") == 0)
		return E_Load;
	else if (pg_strcasecmp(command, "REINDEX") == 0)
		return E_Reindex;
	else if (pg_strcasecmp(command, "SELECT INTO") == 0)
		return E_SelectInto;
	else if (pg_strcasecmp(command, "VACUUM") == 0)
		return E_Vacuum;
	else
	{
		if (!noerror)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("unrecognized command \"%s\"", command)));
	}
	return E_UNKNOWN;
}

char *
command_to_string(TrigEventCommand command)
{
	switch (command)
	{
		case E_UNKNOWN:
			return "UNKNOWN";
		case E_ANY:
			return "ANY";
		case E_AlterCast:
			return "ALTER CAST";
		case E_AlterIndex:
			return "ALTER INDEX";
		case E_AlterAggregate:
			return "ALTER AGGREGATE";
		case E_AlterCollation:
			return "ALTER COLLATION";
		case E_AlterConversion:
			return "ALTER CONVERSION";
		case E_AlterDomain:
			return "ALTER DOMAIN";
		case E_AlterExtension:
			return "ALTER EXTENSION";
		case E_AlterForeignDataWrapper:
			return "ALTER FOREIGN DATA WRAPPER";
		case E_AlterForeignTable:
			return "ALTER FOREIGN TABLE";
		case E_AlterFunction:
			return "ALTER FUNCTION";
		case E_AlterLanguage:
			return "ALTER LANGUAGE";
		case E_AlterOperator:
			return "ALTER OPERATOR";
		case E_AlterOperatorClass:
			return "ALTER OPERATOR CLASS";
		case E_AlterOperatorFamily:
			return "ALTER OPERATOR FAMILY";
		case E_AlterSequence:
			return "ALTER SEQUENCE";
		case E_AlterServer:
			return "ALTER SERVER";
		case E_AlterSchema:
			return "ALTER SCHEMA";
		case E_AlterTable:
			return "ALTER TABLE";
		case E_AlterTextSearchConfiguration:
			return "ALTER TEXT SEARCH CONFIGURATION";
		case E_AlterTextSearchDictionary:
			return "ALTER TEXT SEARCH DICTIONARY";
		case E_AlterTextSearchParser:
			return "ALTER TEXT SEARCH PARSER";
		case E_AlterTextSearchTemplate:
			return "ALTER TEXT SEARCH TEMPLATE";
		case E_AlterTrigger:
			return "ALTER TRIGGER";
		case E_AlterType:
			return "ALTER TYPE";
		case E_AlterUserMapping:
			return "ALTER USER MAPPING";
		case E_AlterView:
			return "ALTER VIEW";
		case E_Cluster:
			return "CLUSTER";
		case E_CreateAggregate:
			return "CREATE AGGREGATE";
		case E_CreateCast:
			return "CREATE CAST";
		case E_CreateCollation:
			return "CREATE COLLATION";
		case E_CreateConversion:
			return "CREATE CONVERSION";
		case E_CreateDomain:
			return "CREATE DOMAIN";
		case E_CreateExtension:
			return "CREATE EXTENSION";
		case E_CreateForeignDataWrapper:
			return "CREATE FOREIGN DATA WRAPPER";
		case E_CreateForeignTable:
			return "CREATE FOREIGN TABLE";
		case E_CreateFunction:
			return "CREATE FUNCTION";
		case E_CreateIndex:
			return "CREATE INDEX";
		case E_CreateLanguage:
			return "CREATE LANGUAGE";
		case E_CreateOperator:
			return "CREATE OPERATOR";
		case E_CreateOperatorClass:
			return "CREATE OPERATOR CLASS";
		case E_CreateOperatorFamily:
			return "CREATE OPERATOR FAMILY";
		case E_CreateRule:
			return "CREATE RULE";
		case E_CreateSequence:
			return "CREATE SEQUENCE";
		case E_CreateServer:
			return "CREATE SERVER";
		case E_CreateSchema:
			return "CREATE SCHEMA";
		case E_CreateTable:
			return "CREATE TABLE";
		case E_CreateTableAs:
			return "CREATE TABLE AS";
		case E_CreateTextSearchConfiguration:
			return "CREATE TEXT SEARCH CONFIGURATION";
		case E_CreateTextSearchDictionary:
			return "CREATE TEXT SEARCH DICTIONARY";
		case E_CreateTextSearchParser:
			return "CREATE TEXT SEARCH PARSER";
		case E_CreateTextSearchTemplate:
			return "CREATE TEXT SEARCH TEMPLATE";
		case E_CreateTrigger:
			return "CREATE TRIGGER";
		case E_CreateType:
			return "CREATE TYPE";
		case E_CreateUserMapping:
			return "CREATE USER MAPPING";
		case E_CreateView:
			return "CREATE VIEW";
		case E_DropAggregate:
			return "DROP AGGREGATE";
		case E_DropCast:
			return "DROP CAST";
		case E_DropCollation:
			return "DROP COLLATION";
		case E_DropConversion:
			return "DROP CONVERSION";
		case E_DropDomain:
			return "DROP DOMAIN";
		case E_DropExtension:
			return "DROP EXTENSION";
		case E_DropForeignDataWrapper:
			return "DROP FOREIGN DATA WRAPPER";
		case E_DropForeignTable:
			return "DROP FOREIGN TABLE";
		case E_DropFunction:
			return "DROP FUNCTION";
		case E_DropIndex:
			return "DROP INDEX";
		case E_DropLanguage:
			return "DROP LANGUAGE";
		case E_DropOperator:
			return "DROP OPERATOR";
		case E_DropOperatorClass:
			return "DROP OPERATOR CLASS";
		case E_DropOperatorFamily:
			return "DROP OPERATOR FAMILY";
		case E_DropRule:
			return "DROP RULE";
		case E_DropSchema:
			return "DROP SCHEMA";
		case E_DropSequence:
			return "DROP SEQUENCE";
		case E_DropServer:
			return "DROP SERVER";
		case E_DropTable:
			return "DROP TABLE";
		case E_DropTextSearchConfiguration:
			return "DROP TEXT SEARCH CONFIGURATION";
		case E_DropTextSearchDictionary:
			return "DROP TEXT SEARCH DICTIONARY";
		case E_DropTextSearchParser:
			return "DROP TEXT SEARCH PARSER";
		case E_DropTextSearchTemplate:
			return "DROP TEXT SEARCH TEMPLATE";
		case E_DropTrigger:
			return "DROP TRIGGER";
		case E_DropType:
			return "DROP TYPE";
		case E_DropUserMapping:
			return "DROP USER MAPPING";
		case E_DropView:
			return "DROP VIEW";
		case E_Load:
			return "LOAD";
		case E_Reindex:
			return "REINDEX";
		case E_SelectInto:
			return "SELECT INTO";
		case E_Vacuum:
			return "VACUUM";
	}
	return NULL;
}
