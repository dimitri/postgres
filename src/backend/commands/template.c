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
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_extension_control.h"
#include "catalog/pg_extension_template.h"
#include "catalog/pg_extension_uptmpl.h"
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
									   ExtensionControl *control,
									   const char *version);

static Oid InsertExtensionTemplateTuple(Oid owner,
										ExtensionControl *control,
										const char *version,
										const char *script);

static Oid InsertExtensionUpTmplTuple(Oid owner,
									  const char *extname,
									  ExtensionControl *control,
									  const char *from,
									  const char *to,
									  const char *script);

static Oid AlterTemplateSetDefault(const char *extname, const char *version);
static Oid modify_pg_extension_control_default(const char *extname,
											   const char *version,
											   bool value);

static ExtensionControl *read_pg_extension_control(const char *extname,
												   Relation rel,
												   HeapTuple tuple);


/*
 * The grammar accumulates control properties into a DefElem list that we have
 * to process in multiple places.
 */
static void
parse_statement_control_defelems(ExtensionControl *control, List *defelems)
{
	ListCell	*lc;
	DefElem		*d_schema	   = NULL;
	DefElem		*d_superuser   = NULL;
	DefElem		*d_relocatable = NULL;
	DefElem		*d_requires	   = NULL;

	/*
	 * Read the statement option list
	 */
	foreach(lc, defelems)
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
}

/*
 * CREATE TEMPLATE FOR EXTENSION
 *
 * Routing function, the statement can be either about a template for creating
 * an extension or a template for updating and extension.
 */
Oid
CreateTemplate(CreateTemplateStmt *stmt)
{
	switch (stmt->template)
	{
		case TEMPLATE_CREATE_EXTENSION:
			return CreateExtensionTemplate(stmt);

		case TEMPLATE_UPDATE_EXTENSION:
			return CreateExtensionUpdateTemplate(stmt);
	}
	/* keep compiler happy */
	return InvalidOid;
}

/*
 * CREATE TEMPLATE FOR EXTENSION
 *
 * Create a template for an extension's given version.
 */
Oid
CreateExtensionTemplate(CreateTemplateStmt *stmt)
{
    Oid			 extTemplateOid;
	Oid			 owner		   = GetUserId();
	ExtensionControl *control;

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

	/* Now read the control properties from the statement */
 	control = (ExtensionControl *) palloc0(sizeof(ExtensionControl));
	control->name = pstrdup(stmt->extname);
	parse_statement_control_defelems(control, stmt->control);

	/*
	 * Check that there's no other pg_extension_control row already claiming to
	 * be the default for this extension, when the statement claims to be the
	 * default.
	 */
	if (stmt->default_version)
	{
		ExtensionControl *default_version =
			find_default_pg_extension_control(control->name, true);

		if (default_version)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("extension \"%s\" already has a default control template",
							control->name),
					 errdetail("default version is \"%s\"",
							   default_version->default_version)));

		/* no pre-existing */
		control->default_version = pstrdup(stmt->version);
	}

	extTemplateOid =  InsertExtensionTemplateTuple(owner,
												   control,
												   stmt->version,
												   stmt->script);

	/* Check that we have a default version target now */
	CommandCounterIncrement();
	find_default_pg_extension_control(stmt->extname, false);

	return extTemplateOid;
}

/*
 * CREATE TEMPLATE FOR UPDATE OF EXTENSION
 */
Oid
CreateExtensionUpdateTemplate(CreateTemplateStmt *stmt)
{
	Oid			 owner = GetUserId();
	ExtensionControl *control;

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/*
	 * Check that a template for installing extension already exists in the
	 * catalogs. Do not enforce that we have a complete path upgrade path at
	 * template creation time, that will get checked at CREATE EXTENSION time.
	 */
	(void) can_create_extension_from_template(stmt->extname, false);

	/*
	 * Check for duplicate template for given extension and versions. The
	 * unique index on pg_extension_uptmpl(uptname, uptfrom, uptto) would catch
	 * this anyway, and serves as a backstop in case of race conditions; but
	 * this is a friendlier error message, and besides we need a check to
	 * support IF NOT EXISTS.
	 */
	if (get_uptmpl_oid(stmt->extname, stmt->from, stmt->to, true) != InvalidOid)
	{
		if (stmt->if_not_exists)
		{
			ereport(NOTICE,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("template for extension \"%s\" update from version \"%s\" to version \"%s\" already exists, skipping",
							stmt->extname, stmt->from, stmt->to)));
			return InvalidOid;
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("template for extension \"%s\" update from version \"%s\" to version \"%s\" already exists",
							stmt->extname, stmt->from, stmt->to)));
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

	/* Now read the (optional) control properties from the statement */
	if (stmt->control)
	{
 	    control = (ExtensionControl *) palloc0(sizeof(ExtensionControl));
	    control->name = pstrdup(stmt->extname);

		parse_statement_control_defelems(control, stmt->control);
	}

	return InsertExtensionUpTmplTuple(owner, stmt->extname, control,
									  stmt->from, stmt->to, stmt->script);
}

/*
 * InsertExtensionControlTuple
 *
 * Insert the new pg_extension_control row and register its dependency to its
 * owner. Return the OID assigned to the new row.
 */
static Oid
InsertExtensionControlTuple(Oid owner,
							ExtensionControl *control,
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

	/*
	 * We only register that this pg_extension_control row is the default for
	 * the given extension. Necessary controls must have been made before.
	 */
	if (control->default_version == NULL)
		values[Anum_pg_extension_control_ctldefault - 1] = false;
	else
		values[Anum_pg_extension_control_ctldefault - 1] = true;

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
InsertExtensionTemplateTuple(Oid owner, ExtensionControl *control,
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

	recordDependencyOn(&ctrl, &myself, DEPENDENCY_INTERNAL);

	/* Post creation hook for new extension control */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   ExtensionTemplateRelationId, extTemplateOid, 0, NULL);

	return extTemplateOid;
}

/*
 * InsertExtensionUpTmplTuple
 *
 * Insert the new pg_extension_uptmpl row and register its dependencies.
 * Return the OID assigned to the new row.
 */
static Oid
InsertExtensionUpTmplTuple(Oid owner,
						   const char *extname,
						   ExtensionControl *control,
						   const char *from,
						   const char *to,
						   const char *script)
{
	Oid			extControlOid, extUpTmplOid;
	Relation	rel;
	Datum		values[Natts_pg_extension_uptmpl];
	bool		nulls[Natts_pg_extension_uptmpl];
	HeapTuple	tuple;
	ObjectAddress myself;

	/*
	 * First create the companion extension control entry, if any. In the case
	 * of an Update Template the comanion control entry is somilar in scope to
	 * a secondary control file, and is attached to the target version.
	 */
	if (control)
		extControlOid = InsertExtensionControlTuple(owner, control, to);
	else
		extControlOid = InvalidOid;

	/*
	 * Build and insert the pg_extension_uptmpl tuple
	 */
	rel = heap_open(ExtensionUpTmplRelationId, RowExclusiveLock);

	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));

	values[Anum_pg_extension_uptmpl_uptname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(extname));
	values[Anum_pg_extension_uptmpl_uptowner - 1] = ObjectIdGetDatum(owner);
	values[Anum_pg_extension_uptmpl_uptfrom - 1] = CStringGetTextDatum(from);
	values[Anum_pg_extension_uptmpl_uptto - 1] = CStringGetTextDatum(to);
	values[Anum_pg_extension_uptmpl_uptscript - 1] = CStringGetTextDatum(script);

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	extUpTmplOid = simple_heap_insert(rel, tuple);
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
	recordDependencyOnOwner(ExtensionUpTmplRelationId, extUpTmplOid, owner);

	myself.classId = ExtensionUpTmplRelationId;
	myself.objectId = extUpTmplOid;
	myself.objectSubId = 0;

	/* record he dependency between the control row and the template row */
	if (control)
	{
		ObjectAddress ctrl;

		ctrl.classId = ExtensionControlRelationId;
		ctrl.objectId = extControlOid;
		ctrl.objectSubId = 0;

		recordDependencyOn(&ctrl, &myself, DEPENDENCY_INTERNAL);
	}

	/* Post creation hook for new extension control */
	InvokeObjectAccessHook(OAT_POST_CREATE,
						   ExtensionUpTmplRelationId, extUpTmplOid, 0, NULL);

	return extUpTmplOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION name VERSION version
 *
 * This implements high level routing for sub commands.
 */
Oid
AlterTemplate(AlterTemplateStmt *stmt)
{
	switch (stmt->template)
	{
		case TEMPLATE_CREATE_EXTENSION:
			return AlterExtensionTemplate(stmt);

		case TEMPLATE_UPDATE_EXTENSION:
			return AlterExtensionUpdateTemplate(stmt);
	}
	/* keep compiler happy */
	return InvalidOid;
}

Oid
AlterExtensionTemplate(AlterTemplateStmt *stmt)
{
	switch (stmt->cmdtype)
	{
		case AET_SET_DEFAULT:
			return AlterTemplateSetDefault(stmt->extname, stmt->version);

		case AET_SET_SCRIPT:
			elog(WARNING, "Not Yet Implemented");
			break;

		case AET_UPDATE_CONTROL:
			elog(WARNING, "Not Yet Implemented");
			break;
	}

	return InvalidOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION ... SET DEFAULT VERSION ...
 *
 * We refuse to run without a default, so we drop the current one when
 * assigning a new one.
 */
static Oid
AlterTemplateSetDefault(const char *extname, const char *version)
{
	/* we need to know who's the default */
	ExtensionControl *current =
		find_default_pg_extension_control(extname, true);

	if (current)
	{
		if (strcmp(current->default_version, version) == 0)
			/* silently do nothing */
			return InvalidOid;

		/* set ctldefault to false on current default extension */
		modify_pg_extension_control_default(current->name,
											current->default_version,
											false);
	}
	/* set ctldefault to true on new default extension */
	return modify_pg_extension_control_default(extname, version, true);
}

/*
 * Implement flipping the ctldefault bit from value to repl.
 */
static Oid
modify_pg_extension_control_default(const char *extname,
									const char *version,
									bool value)
{
	Oid         ctrlOid;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[2];
	Datum		values[Natts_pg_extension_control];
	bool		nulls[Natts_pg_extension_control];
	bool		repl[Natts_pg_extension_control];

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_control_ctlname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	ScanKeyInit(&entry[1],
				Anum_pg_extension_control_ctlversion,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(version));

	scandesc = systable_beginscan(rel,
								  ExtensionControlNameVersionIndexId, true,
								  SnapshotNow, 2, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */

	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR,
			 "pg_extension_control for extension \"%s\" version \"%s\" does not exist",
			 extname, version);

	ctrlOid = HeapTupleGetOid(tuple);

	/* Modify ctldefault in the pg_extension_control tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(repl, 0, sizeof(repl));

	values[Anum_pg_extension_control_ctldefault - 1] = BoolGetDatum(value);
	repl[Anum_pg_extension_control_ctldefault - 1] = true;

	tuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
							  values, nulls, repl);

	simple_heap_update(rel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return ctrlOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION name FROM old TO new
 *
 * This implements high level routing for sub commands.
 */
Oid
AlterExtensionUpdateTemplate(AlterTemplateStmt *stmt)
{
	elog(WARNING, "Not Yet Implemented");
	return InvalidOid;
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

/*
 * Check that the given extension name has a create template.
 */
bool
can_create_extension_from_template(const char *extname, bool missing_ok)
{
	bool		result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionTemplateRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_template_tplname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel,
								  ExtensionTemplateNameVersionIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We only are interested into knowing if we found at least one tuple */
	result = HeapTupleIsValid(tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	if (!result && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("no template for extension \"%s\"", extname)));

	return result;
}

/*
 * get_uptmpl_oid - given an extension name, from version and to version, look
 * up the uptmpl OID
 *
 * If missing_ok is false, throw an error if extension name not found.	If
 * true, just return InvalidOid.
 */
Oid
get_uptmpl_oid(const char *extname, const char *from, const char *to,
			   bool missing_ok)
{
	Oid			result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[3];

	rel = heap_open(ExtensionUpTmplRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_uptmpl_uptname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	ScanKeyInit(&entry[1],
				Anum_pg_extension_uptmpl_uptfrom,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(from));

	ScanKeyInit(&entry[2],
				Anum_pg_extension_uptmpl_uptto,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(to));

	scandesc = systable_beginscan(rel,
								  ExtensionUpTpmlNameFromToIndexId, true,
								  SnapshotNow, 3, entry);

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
				 errmsg("template for extension \"%s\" update from version \"%s\" to version \"%s\"does not exist",
						extname, from, to)));

	return result;
}

/*
 * Remove Extension Control by OID
 */
void
RemoveExtensionControlById(Oid extControlOid)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionControlRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extControlOid));
	scandesc = systable_beginscan(rel, ExtensionControlOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Remove Extension Control by OID
 */
void
RemoveExtensionTemplateById(Oid extTemplateOid)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionTemplateRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extTemplateOid));
	scandesc = systable_beginscan(rel, ExtensionTemplateOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Remove Extension Control by OID
 */
void
RemoveExtensionUpTmplById(Oid extUpTmplOid)
{
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionUpTmplRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(extUpTmplOid));
	scandesc = systable_beginscan(rel, ExtensionUpTmplOidIndexId, true,
								  SnapshotNow, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}

/*
 * read_pg_extension_control
 *
 * Read a pg_extension_control row and fill in an ExtensionControl
 * structure with the right elements in there.
 */
static ExtensionControl *
read_pg_extension_control(const char *extname, Relation rel, HeapTuple tuple)
{
	Datum dreqs;
	bool isnull;
	Form_pg_extension_control ctrl =
		(Form_pg_extension_control) GETSTRUCT(tuple);

	ExtensionControl *control =
		(ExtensionControl *) palloc0(sizeof(ExtensionControl));

	/* Those fields are not null */
	control->name = pstrdup(extname);
	control->is_template = true;
	control->relocatable = ctrl->ctlrelocatable;
	control->superuser = ctrl->ctlsuperuser;
	control->schema = pstrdup(NameStr(ctrl->ctlnamespace));

	if (ctrl->ctldefault)
	{
		Datum dvers =
			heap_getattr(tuple, Anum_pg_extension_control_ctlversion,
						 RelationGetDescr(rel), &isnull);

		char *version = isnull? NULL : text_to_cstring(DatumGetTextPP(dvers));

		if (isnull)
		{
			/* shouldn't happen */
			elog(ERROR,
				 "pg_extension_control row without version for \"%s\"",
				 extname);
		}

		/* get the version and requires fields from here */
		control->default_version = pstrdup(version);
	}

	/* now see about the dependencies array */
	dreqs = heap_getattr(tuple, Anum_pg_extension_control_ctlrequires,
						 RelationGetDescr(rel), &isnull);

	if (!isnull)
	{
		ArrayType  *arr = DatumGetArrayTypeP(dreqs);
		Datum	   *elems;
		int			i;
		int			nelems;

		if (ARR_NDIM(arr) != 1 || ARR_HASNULL(arr) || ARR_ELEMTYPE(arr) != TEXTOID)
			elog(ERROR, "expected 1-D text array");
		deconstruct_array(arr, TEXTOID, -1, false, 'i', &elems, NULL, &nelems);

		for (i = 0; i < nelems; ++i)
			control->requires = lappend(control->requires,
										TextDatumGetCString(elems[i]));

		pfree(elems);
	}
	return control;
}

/*
 * Find the pg_extension_control row for given extname and version, if any, and
 * return a filled in ExtensionControl structure.
 *
 * In case we don't have any pg_extension_control row for given extname and
 * version, return NULL.
 */
ExtensionControl *
find_pg_extension_control(const char *extname,
						  const char *version,
						  bool missing_ok)
{
	ExtensionControl *control = NULL;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[2];

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_control_ctlname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	ScanKeyInit(&entry[1],
				Anum_pg_extension_control_ctlversion,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(version));

	scandesc = systable_beginscan(rel,
								  ExtensionControlNameVersionIndexId, true,
								  SnapshotNow, 2, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		control = read_pg_extension_control(extname, rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	if (control == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" has no control template for version \"%s\"",
						extname, version)));

	return control;
}

/*
 * Find the default extension's control properties.
 */
ExtensionControl *
find_default_pg_extension_control(const char *extname, bool missing_ok)
{
	ExtensionControl *control = NULL;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_control_ctlname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel,
								  ExtensionControlNameVersionIndexId, true,
								  SnapshotNow, 1, entry);

	/* find all the control tuples for extname */
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		bool isnull;
		bool ctldefault =
			DatumGetBool(
				fastgetattr(tuple, Anum_pg_extension_control_ctldefault,
							RelationGetDescr(rel), &isnull));

		/* only of those is the default */
		if (ctldefault)
		{
			if (control == NULL)
				control = read_pg_extension_control(extname, rel, tuple);
			else
				/* should not happen */
				elog(ERROR,
					 "Extension \"%s\" has more than one default control template",
					 extname);
		}
	}
	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	/* we really need a single default version. */
	if (control == NULL && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" has no default control template",
						extname)));

	return control;
}

/*
 * read_pg_extension_uptmpl_script
 *
 * Return the script from the pg_extension_template catalogs.
 */
char *
read_pg_extension_template_script(const char *extname, const char *version)
{
	char		*script;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;
	ScanKeyData	 entry[2];

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
	{
		bool	isnull;
		Datum	dscript;

		dscript = heap_getattr(tuple, Anum_pg_extension_template_tplscript,
							   RelationGetDescr(rel), &isnull);

		script = isnull? NULL : text_to_cstring(DatumGetTextPP(dscript));
	}
	else
		/* can't happen */
		elog(ERROR,
			 "Missing Extension Template entry for extension \"%s\" version \"%s\"",
			 extname, version);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return script;
}

/*
 * read_pg_extension_uptmpl_script
 *
 * Return the script from the pg_extension_uptmpl catalogs.
 */
char *
read_pg_extension_uptmpl_script(const char *extname,
								const char *from_version, const char *version)
{
	char		*script;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;
	ScanKeyData	 entry[3];

	rel = heap_open(ExtensionUpTmplRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_uptmpl_uptname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	ScanKeyInit(&entry[1],
				Anum_pg_extension_uptmpl_uptfrom,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(from_version));

	ScanKeyInit(&entry[2],
				Anum_pg_extension_uptmpl_uptto,
				BTEqualStrategyNumber, F_TEXTEQ,
				CStringGetTextDatum(version));

	scandesc = systable_beginscan(rel,
								  ExtensionUpTpmlNameFromToIndexId, true,
								  SnapshotNow, 3, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
	{
		bool	isnull;
		Datum	dscript;

		dscript = heap_getattr(tuple, Anum_pg_extension_uptmpl_uptscript,
							   RelationGetDescr(rel), &isnull);

		script = isnull? NULL : text_to_cstring(DatumGetTextPP(dscript));
	}
	else
		/* can't happen */
		elog(ERROR, "Extension Template Control entry for \"%s\" has no template for update from version \"%s\" to version \"%s\"",
			 extname, from_version, version);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return script;
}

/*
 * read_extension_template_script
 *
 * Given an extension's name and a version, return the extension's script from
 * the pg_extension_template or the pg_extension_uptmpl catalog. The former is
 * used when from_version is NULL.
 */
char *
read_extension_template_script(const char *extname,
							   const char *from_version, const char *version)
{
	if (from_version)
		return read_pg_extension_uptmpl_script(extname, from_version, version);
	else
		return read_pg_extension_template_script(extname, version);
}

/*
 * Returns a list of cstring containing all known versions that you can install
 * for a given extension.
 */
List *
list_pg_extension_template_versions(const char *extname)
{
	List		*versions = NIL;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;
	ScanKeyData	 entry[1];

	rel = heap_open(ExtensionTemplateRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_template_tplname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel,
								  ExtensionTemplateNameVersionIndexId, true,
								  SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		bool	isnull;
		Datum dvers =
			heap_getattr(tuple, Anum_pg_extension_template_tplversion,
						 RelationGetDescr(rel), &isnull);

		char *version = isnull? NULL : text_to_cstring(DatumGetTextPP(dvers));

		versions = lappend(versions, version);
	}

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return versions;
}

/*
 * Returns a list of lists of source and target versions for which we have a
 * direct upgrade path for.
 */
List *
list_pg_extension_update_versions(const char *extname)
{
	List		*versions = NIL;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;
	ScanKeyData	 entry[1];

	rel = heap_open(ExtensionUpTmplRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				Anum_pg_extension_uptmpl_uptname,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(extname));

	scandesc = systable_beginscan(rel,
								  ExtensionUpTpmlNameFromToIndexId, true,
								  SnapshotNow, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		bool	 isnull;
		Datum	 dfrom, dto;
		char	*from, *to;

		/* neither from nor to are allowed to be null... */
		dfrom = heap_getattr(tuple, Anum_pg_extension_uptmpl_uptfrom,
							 RelationGetDescr(rel), &isnull);

		from = isnull ? NULL : text_to_cstring(DatumGetTextPP(dfrom));

		dto = heap_getattr(tuple, Anum_pg_extension_uptmpl_uptto,
						   RelationGetDescr(rel), &isnull);

		to = isnull ? NULL : text_to_cstring(DatumGetTextPP(dto));

		versions = lappend(versions, list_make2(from, to));
	}

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return versions;
}
