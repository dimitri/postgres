/*-------------------------------------------------------------------------
 *
 * template.c
 *	  Commands to manipulate templates
 *
 * Extension Templates in PostgreSQL allow creation of Extension from the
 * protocol only.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/template.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/sysattr.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_extension_control.h"
#include "catalog/pg_extension_template.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "commands/extension.h"
#include "commands/template.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tqual.h"

static Oid InsertExtensionControlTuple(Oid owner,
										   ExtensionControlFile *control,
										   const char *version);

static Oid InsertExtensionTemplateTuple(Oid owner,
                                            ExtensionControlFile *control,
                                            const char *version,
											const char *script);

/*
 * CREATE TEMPLATE FOR EXTENSION
 */
Oid
CreateTemplate(CreateTemplateStmt *stmt)
{
	DefElem		*d_schema = NULL;
	DefElem		*d_default = NULL;
	DefElem		*d_superuser = NULL;
	DefElem		*d_relocatable = NULL;
	DefElem		*d_requires = NULL;
	Oid			 owner = GetUserId();
	ExtensionControlFile *control;
	ListCell	*lc;

	elog(NOTICE, "CREATE TEMPLATE FOR EXTENSION %s", stmt->extname);

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check for duplicate extension name in the pg_extension catalogs. Any
	 * extension that already is known in the catalogs needs no template for
	 * creating it in the first place.
	 */
	if (get_extension_oid(stmt->extname, true) != InvalidOid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extension \"%s\" already exists",
						stmt->extname)));
	}

	/*
	 * Check for duplicate template for given extension and version. The unique
	 * index on pg_extension_template(extname, version) would catch this
	 * anyway, and serves as a backstop in case of race conditions; but this is
	 * a friendlier error message, and besides we need a check to support IF
	 * NOT EXISTS.
	 */
	if (get_template_oid(stmt->extname, stmt->version, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("template for extension \"%s\" version \"%s\" already exists, skipping",
							stmt->extname, stmt->version)));
			return InvalidOid;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("template for extension \"%s\" version \"%s\" already exists",
							stmt->extname, stmt->version)));
	}

	/*
	 * Check that no control file of the same extension's name is already
	 * available on disk, as a friendliness service to our users. Between
	 * CREATE TEMPLATE FOR EXTENSION and CREATE EXTENSION time, some new file
	 * might have been added to the file-system and would then be prefered, but
	 * at least we tried to be as nice as we possibly can.
	 */
	PG_TRY();
	{
		control = read_extension_control_file(stmt->extname);
	}
	PG_CATCH();
	{
		/* no control file found is good news for us */
		control = NULL;
	}
	PG_END_TRY();

	if (control)
	{
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" is already available",
							stmt->extname)));
	}

	/* Create an Extension Control structure to pass down the options */
	control = (ExtensionControlFile *) palloc0(sizeof(ExtensionControlFile));
	control->name = pstrdup(stmt->extname);

	/*
	 * Read the statement option list
	 */
	foreach(lc, stmt->control)
	{
		DefElem    *defel = (DefElem *) lfirst(lc);

		if (strcmp(defel->defname, "schema") == 0)
		{
			if (d_schema)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_schema = defel;

			control->schema = strVal(d_schema->arg);
		}
		else if (strcmp(defel->defname, "default") == 0)
		{
			if (d_default)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_default = defel;

			control->default_version = pstrdup(strVal(d_default->arg));
		}
		else if (strcmp(defel->defname, "superuser") == 0)
		{
			if (d_superuser)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_superuser = defel;

			control->superuser = intVal(d_superuser->arg) != 0;
		}
		else if (strcmp(defel->defname, "relocatable") == 0)
		{
			if (d_relocatable)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_relocatable = defel;

			control->relocatable = intVal(d_relocatable->arg) != 0;
		}
		else if (strcmp(defel->defname, "requires") == 0)
		{
			if (d_requires)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_requires = defel;

			if (!SplitIdentifierString(pstrdup(strVal(d_requires->arg)),
									   ',',
									   &control->requires))
			{
				/* syntax error in name list */
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
						 errmsg("parameter \"requires\" must be a list of extension names")));
			}
		}
		else
			elog(ERROR, "unrecognized option: %s", defel->defname);
	}

	return InsertExtensionTemplateTuple(owner,
										control,
										stmt->version,
										stmt->script);
}

/*
 * InsertExtensionControlTuple
 *
 * Insert the new pg_extension_control row and register its dependency to its
 * owner. Return the OID assigned to the new row.
 */
static Oid
InsertExtensionControlTuple(Oid owner, ExtensionControlFile *control,
							const char *version)
{
	Oid			extControlOid;
	Relation	rel;
	Datum		values[Natts_pg_extension_control];
	bool		nulls[Natts_pg_extension_control];
	HeapTuple	tuple;
	ObjectAddress myself;

	/*
	 * Build and insert the pg_extension_control tuple
	 */
	rel = heap_open(ExtensionControlRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[Anum_pg_extension_control_ctlname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(control->name));

	values[Anum_pg_extension_control_ctlowner - 1] =
		ObjectIdGetDatum(owner);

	values[Anum_pg_extension_control_ctlrelocatable - 1] =
		BoolGetDatum(control->relocatable);

	values[Anum_pg_extension_control_ctlsuperuser - 1] =
		BoolGetDatum(control->superuser);

	if (control->schema == NULL)
		nulls[Anum_pg_extension_control_ctlnamespace - 1] = true;
	else
		values[Anum_pg_extension_control_ctlnamespace - 1] =
			DirectFunctionCall1(namein, CStringGetDatum((control->schema)));

	values[Anum_pg_extension_control_ctlversion - 1] =
		CStringGetTextDatum(version);

	if (control->default_version == NULL)
		nulls[Anum_pg_extension_control_ctldefault - 1] = true;
	else
		values[Anum_pg_extension_control_ctldefault - 1] =
			CStringGetTextDatum(control->default_version);

	if (control->requires == NULL)
		nulls[Anum_pg_extension_control_ctlrequires - 1] = true;
	else
	{
		Datum	   *datums;
		int			ndatums;
		ArrayType  *a;
		ListCell   *lc;

		ndatums = list_length(control->requires);
		datums = (Datum *) palloc(ndatums * sizeof(Datum));
		ndatums = 0;
		foreach(lc, control->requires)
		{
			char	   *curreq = (char *) lfirst(lc);

			datums[ndatums++] =
				DirectFunctionCall1(namein, CStringGetDatum(curreq));
		}
		a = construct_array(datums, ndatums,
							NAMEOID,
							NAMEDATALEN, false, 'c');

		values[Anum_pg_extension_control_ctlrequires - 1] =
			PointerGetDatum(a);
	}

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	extControlOid = simple_heap_insert(rel, tuple);
	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner only.
	 *
	 * When we create the extension template and control file, the target
	 * extension, its schema and requirements usually do not exist in the
	 * database. Don't even think about registering a dependency from the
	 * template.
	 */
	recordDependencyOnOwner(ExtensionControlRelationId, extControlOid, owner);

	myself.classId = ExtensionControlRelationId;
	myself.objectId = extControlOid;
	myself.objectSubId = 0;

	/* Post creation hook for new extension control */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   ExtensionControlRelationId, extControlOid, 0, NULL);

	return extControlOid;
}

/*
 * InsertExtensionTemplateTuple
 *
 * Insert the new pg_extension_template row and register its dependencies.
 * Return the OID assigned to the new row.
 */
static Oid
InsertExtensionTemplateTuple(Oid owner, ExtensionControlFile *control,
							 const char *version, const char *script)
{
	Oid			extControlOid, extTemplateOid;
	Relation	rel;
	Datum		values[Natts_pg_extension_template];
	bool		nulls[Natts_pg_extension_template];
	HeapTuple	tuple;
	ObjectAddress myself, ctrl;

	/* First create the companion extension control entry */
	extControlOid = InsertExtensionControlTuple(owner, control, version);

	/*
	 * Build and insert the pg_extension_template tuple
	 */
	rel = heap_open(ExtensionTemplateRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[Anum_pg_extension_template_tplname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(control->name));

	values[Anum_pg_extension_template_tplowner - 1] =
		ObjectIdGetDatum(owner);

	values[Anum_pg_extension_template_tplversion - 1] =
		CStringGetTextDatum(version);

	values[Anum_pg_extension_template_tplscript - 1] =
		CStringGetTextDatum(script);

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	extTemplateOid = simple_heap_insert(rel, tuple);
	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner only.
	 *
	 * When we create the extension template and control file, the target
	 * extension, its schema and requirements usually do not exist in the
	 * database. Don't even think about registering a dependency from the
	 * template.
	 */
	recordDependencyOnOwner(ExtensionTemplateRelationId, extTemplateOid, owner);

	myself.classId = ExtensionTemplateRelationId;
	myself.objectId = extTemplateOid;
	myself.objectSubId = 0;

	/* record he dependency between the control row and the template row */
	ctrl.classId = ExtensionControlRelationId;
	ctrl.objectId = extControlOid;
	ctrl.objectSubId = 0;

	recordDependencyOn(&myself, &ctrl, DEPENDENCY_NORMAL);

	/* Post creation hook for new extension control */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   ExtensionTemplateRelationId, extTemplateOid, 0, NULL);

	return extTemplateOid;
}

/*
 * get_template_oid - given an extension name and version, look up the template
 * OID
 *
 * If missing_ok is false, throw an error if extension name not found.	If
 * true, just return InvalidOid.
 */
Oid
get_template_oid(const char *extname, const char *version, bool missing_ok)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[2];

	rel = heap_open(ExtensionTemplateRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_template_tplname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	ScanKeyInit(&entry[1],
				Anum_pg_extension_template_tplversion,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(version));

	scandesc = systable_beginscan(rel,
								  ExtensionTemplateNameVersionIndexId, true,
								  SnapshotNow, 2, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = HeapTupleGetOid(tuple);
	else
		result = InvalidOid;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	if (!OidIsValid(result) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("template for extension \"%s\" version \"%s\" does not exist",
						extname, version)));

	return result;
}
