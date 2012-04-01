/*-------------------------------------------------------------------------
 *
 * pg_event_trigger.h
 *	  definition of the system "event trigger" relation (pg_event_trigger)
 *	  along with the relation's initial contents.
 *
 *
 * Portions Copyright (c) 1996-2011, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_event_trigger.h
 *
 * NOTES
 *	  the genbki.pl script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_EVENT_TRIGGER_H
#define PG_EVENT_TRIGGER_H

#include "catalog/genbki.h"

/* ----------------
 *		pg_event_trigger definition.	cpp turns this into
 *		typedef struct FormData_pg_event_trigger
 * ----------------
 */
#define EventTriggerRelationId  3466

CATALOG(pg_event_trigger,3466)
{
	NameData	evtname;		/* trigger's name */
	NameData    evtevent;		/* trigger's event */
	Oid			evtfoid;		/* OID of function to be called */
	char		evttype;		/* BEFORE/INSTEAD OF */
	char		evtenabled;		/* trigger's firing configuration WRT
								 * session_replication_role */
#ifdef CATALOG_VARLEN
	int2        evttags[1];		/* command TAGs this event trigger targets */
#endif
} FormData_pg_event_trigger;

/* ----------------
 *		Form_pg_event_trigger corresponds to a pointer to a tuple with
 *		the format of pg_event_trigger relation.
 * ----------------
 */
typedef FormData_pg_event_trigger *Form_pg_event_trigger;

/* ----------------
 *		compiler constants for pg_event_trigger
 * ----------------
 */
#define Natts_pg_event_trigger					6
#define Anum_pg_event_trigger_evtname			1
#define Anum_pg_event_trigger_evtevent			2
#define Anum_pg_event_trigger_evtfoid			3
#define Anum_pg_event_trigger_evttype			4
#define Anum_pg_event_trigger_evtenabled		5
#define Anum_pg_event_trigger_evttags			6

/*
 * Times at which an event trigger can be fired. These are the
 * possible values for pg_event_trigger.evtevent.
 *
 * We don't use 0 here because of how we manage the backend local caching of
 * event triggers. That cache is an array of commands that contains an array of
 * events that contains a list of function OIDs.
 *
 * To iterate over each function oid to call at a specific event timing for a
 * given command, it's then as easy as:
 *
 *  foreach(cell, EventTriggerCache[EVTG_VACUUM][EVTG_COMMAND_START])
 *
 * The ordering here is important, in order to be able to refuse some cases
 * (late trigger are not possible in commands implementing their own
 * transaction control behavior, such as CLUSTER or CREATE INDEX CONCURRENTLY.
 */
#define EVTG_EVENTS_COUNT		5
#define EVTG_COMMAND_START		1
#define EVTG_SECURITY_CHECK		2
#define EVTG_CONSISTENCY_CHECK	3
#define EVTG_NAME_LOOKUP		4
#define EVTG_COMMAND_END		5

/*
 * Supported commands
 */
#define EVTG_COMMAND_COUNT						84
#define EVTG_ALTER_AGGREGATE					0
#define EVTG_ALTER_COLLATION					1
#define EVTG_ALTER_CONVERSION					2
#define EVTG_ALTER_DOMAIN						3
#define EVTG_ALTER_EXTENSION					4
#define EVTG_ALTER_FOREIGN_DATA_WRAPPER			5
#define EVTG_ALTER_FOREIGN_TABLE				6
#define EVTG_ALTER_FUNCTION						7
#define EVTG_ALTER_LANGUAGE						8
#define EVTG_ALTER_OPERATOR						9
#define EVTG_ALTER_OPERATOR_CLASS				10
#define EVTG_ALTER_OPERATOR_FAMILY				11
#define EVTG_ALTER_SCHEMA						12
#define EVTG_ALTER_SEQUENCE						13
#define EVTG_ALTER_SERVER						14
#define EVTG_ALTER_TABLE						15
#define EVTG_ALTER_TEXT_SEARCHARSER				16
#define EVTG_ALTER_TEXT_SEARCH_CONFIGURATION	17
#define EVTG_ALTER_TEXT_SEARCH_DICTIONARY		18
#define EVTG_ALTER_TEXT_SEARCH_TEMPLATE			19
#define EVTG_ALTER_TRIGGER						20
#define EVTG_ALTER_TYPE							21
#define EVTG_ALTER_USER_MAPPING					22
#define EVTG_ALTER_VIEW							23
#define EVTG_CLUSTER							24
#define EVTG_CREATE_AGGREGATE					25
#define EVTG_CREATE_CAST						26
#define EVTG_CREATE_COLLATION					27
#define EVTG_CREATE_CONVERSION					28
#define EVTG_CREATE_DOMAIN						29
#define EVTG_CREATE_EXTENSION					30
#define EVTG_CREATE_FOREIGN_DATA_WRAPPER		31
#define EVTG_CREATE_FOREIGN_TABLE				32
#define EVTG_CREATE_FUNCTION					33
#define EVTG_CREATE_INDEX						34
#define EVTG_CREATE_LANGUAGE					35
#define EVTG_CREATE_OPERATOR					36
#define EVTG_CREATE_OPERATOR_CLASS				37
#define EVTG_CREATE_OPERATOR_FAMILY				38
#define EVTG_CREATE_RULE						39
#define EVTG_CREATE_SCHEMA						40
#define EVTG_CREATE_SEQUENCE					41
#define EVTG_CREATE_SERVER						42
#define EVTG_CREATE_TABLE						43
#define EVTG_CREATE_TABLE_AS					44
#define EVTG_CREATE_TEXT_SEARCHARSER			45
#define EVTG_CREATE_TEXT_SEARCH_CONFIGURATION	46
#define EVTG_CREATE_TEXT_SEARCH_DICTIONARY		47
#define EVTG_CREATE_TEXT_SEARCH_TEMPLATE		48
#define EVTG_CREATE_TRIGGER						49
#define EVTG_CREATE_TYPE						50
#define EVTG_CREATE_USER_MAPPING				51
#define EVTG_CREATE_VIEW						52
#define EVTG_DROP_AGGREGATE						53
#define EVTG_DROP_CAST							54
#define EVTG_DROP_COLLATION						55
#define EVTG_DROP_CONVERSION					56
#define EVTG_DROP_DOMAIN						57
#define EVTG_DROP_EXTENSION						58
#define EVTG_DROP_FOREIGN_DATA_WRAPPER			59
#define EVTG_DROP_FOREIGN_TABLE					60
#define EVTG_DROP_FUNCTION						61
#define EVTG_DROP_INDEX							62
#define EVTG_DROP_LANGUAGE						63
#define EVTG_DROP_OPERATOR						64
#define EVTG_DROP_OPERATOR_CLASS				65
#define EVTG_DROP_OPERATOR_FAMILY				66
#define EVTG_DROP_RULE							67
#define EVTG_DROP_SCHEMA						68
#define EVTG_DROP_SEQUENCE						69
#define EVTG_DROP_SERVER						70
#define EVTG_DROP_TABLE							71
#define EVTG_DROP_TEXT_SEARCHARSER				72
#define EVTG_DROP_TEXT_SEARCH_CONFIGURATION		73
#define EVTG_DROP_TEXT_SEARCH_DICTIONARY		74
#define EVTG_DROP_TEXT_SEARCH_TEMPLATE			75
#define EVTG_DROP_TRIGGER						76
#define EVTG_DROP_TYPE							77
#define EVTG_DROP_USER_MAPPING					78
#define EVTG_DROP_VIEW							79
#define EVTG_LOAD								80
#define EVTG_REINDEX							81
#define EVTG_SELECT_INTO						82
#define EVTG_VACUUM								83

/*
 * Times at which an event trigger can be fired. These are the
 * possible values for pg_event_trigger.evttype.
 *
 * pg_trigger is using binary mask tricks to make it super fast, but we don't
 * need to be that tricky here: we're talking about commands, not data editing,
 * and we don't have so many conditions, only type and enabled.
 */
#define EVTG_FIRED_BEFORE			'B'
#define EVTG_FIRED_INSTEAD_OF		'I'

#endif   /* PG_EVENT_TRIGGER_H */
