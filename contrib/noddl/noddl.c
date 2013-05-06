/*
 *	PostgreSQL definitions for noddl event trigger extension.
 *
 *	contrib/noddl/noddl.c
 */

#include "postgres.h"
#include "commands/event_trigger.h"


PG_MODULE_MAGIC;

/* forward declarations */
Datum		noddl(PG_FUNCTION_ARGS);


/*
 * This is the trigger that protects us from orphaned large objects
 */
PG_FUNCTION_INFO_V1(noddl);

Datum
noddl(PG_FUNCTION_ARGS)
{
	EventTriggerData *trigdata = (EventTriggerData *) fcinfo->context;

	if (!CALLED_AS_EVENT_TRIGGER(fcinfo))		/* internal error */
		elog(ERROR, "not fired by event trigger manager");

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("command %s denied", trigdata->tag)));

    PG_RETURN_NULL();
}
