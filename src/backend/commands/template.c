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

#include <unistd.h>

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
#include "commands/alter.h"
#include "commands/comment.h"
#include "commands/extension.h"
#include "commands/template.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "tcop/utility.h"
#include "utils/acl.h"
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
static Oid AlterTemplateSetDefaultFull(const char *extname,
									   const char *version);
static Oid AlterTemplateSetControl(const char *extname,
								   const char *version,
								   List *options);

static Oid AlterTemplateSetScript(const char *extname,
								  const char *version, const char *script);
static Oid AlterUpTpmlSetScript(const char *extname,
								const char *from,
								const char *to,
								const char *script);

static Oid modify_pg_extension_control_default(const char *extname,
											   const char *version,
											   bool value);

static Oid modify_pg_extension_control_default_full(const char *extname,
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
	DefElem		*d_comment	   = NULL;
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
		else if (strcmp(defel->defname, "comment") == 0)
		{
			if (d_comment)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("conflicting or redundant options")));
			d_comment = defel;

			control->comment = strVal(d_comment->arg);
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
 * Check that no other extension is available on the system or as a template in
 * the catalogs. When that's the case, we ereport() an ERROR to the user.
 */
static bool
CheckExtensionAvailability(const char *extname, const char *version,
						   bool if_not_exists)
{
	if (version)
	{
		/*
		 * Check for duplicate template for given extension and version. The
		 * unique index on pg_extension_template(extname, version) would catch
		 * this anyway, and serves as a backstop in case of race conditions;
		 * but this is a friendlier error message.
		 */
		if (get_template_oid(extname, version, true) != InvalidOid)
		{
			if (if_not_exists)
			{
				ereport(NOTICE,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("template for extension \"%s\" version \"%s\" already exists, skipping",
								extname, version)));
				return false;
			}
			else
			{
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("template for extension \"%s\" version \"%s\" already exists",
								extname, version)));
			}
		}
	}
	else
	{
		/*
		 * Version is NULL here, meaning we're checking for a RENAME of the
		 * extension, and we want to search for any pre-existing version in the
		 * catalogs. As we have setup an invariant that we always have a single
		 * default version, that's the lookup we make here.
		 */
		ExtensionControl *default_version =
			find_default_pg_extension_control(extname, true);

		if (default_version)
				ereport(ERROR,
						(errcode(ERRCODE_DUPLICATE_OBJECT),
						 errmsg("template for extension \"%s\" version \"%s\" already exists",
								extname, default_version->default_version)));
	}

	/*
	 * Check that no control file of the same extension's name is already
	 * available on disk, as a friendliness service to our users. Between
	 * CREATE TEMPLATE FOR EXTENSION and CREATE EXTENSION time, some new file
	 * might have been added to the file-system and would then be prefered, but
	 * at least we tried to be as nice as we possibly can.
	 */
	if (access(get_extension_control_filename(extname), F_OK) == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extension \"%s\" is already available", extname)));
	}

	return true;
}

/*
 * CREATE TEMPLATE FOR EXTENSION
 *
 * Routing function, the statement can be either about a template for creating
 * an extension or a template for updating and extension.
 */
Oid
CreateTemplate(CreateExtTemplateStmt *stmt)
{
	switch (stmt->tmpltype)
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
CreateExtensionTemplate(CreateExtTemplateStmt *stmt)
{
	ExtensionControl *default_version;
    Oid			 extTemplateOid;
	Oid			 owner		   = GetUserId();
	ExtensionControl *control;

	/*
	 * It would be nice to allow database owners or even regular users to do
	 * this, but then an evil user could create his own template for a known
	 * extension and then provide malicious features if an extension was
	 * created from that template.
	 */
	if (!superuser())
		ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied to create template for extension \"%s\"",
					stmt->extname),
			 errhint("Must be superuser to create a template for an extension.")));

	/* Check extension name validity before any filesystem access */
	check_valid_extension_name(stmt->extname);

	/* Check that we don't already have an extension of this name available. */
	if (!CheckExtensionAvailability(stmt->extname, stmt->version,
									stmt->if_not_exists))
		/* Messages have already been sent to the client */
		return InvalidOid;

	/* Now read the control properties from the statement */
 	control = (ExtensionControl *) palloc0(sizeof(ExtensionControl));
	control->ctrlOid = InvalidOid;
	control->name = pstrdup(stmt->extname);

	parse_statement_control_defelems(control, stmt->control);

	if (control->schema == NULL)
	{
		/*
		 * Else, use the current default creation namespace, which is the
		 * first explicit entry in the search_path.
		 */
		Oid			schemaOid;
		List	   *search_path = fetch_search_path(false);

		if (search_path == NIL)	/* nothing valid in search_path? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));
		schemaOid = linitial_oid(search_path);
		control->schema = get_namespace_name(schemaOid);
		if (control->schema == NULL) /* recently-deleted namespace? */
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_SCHEMA),
					 errmsg("no schema has been selected to create in")));

		list_free(search_path);
	}

	/*
	 * Check that there's no other pg_extension_control row already claiming to
	 * be the default for this extension, when the statement claims to be the
	 * default.
	 */
	default_version = find_default_pg_extension_control(control->name, true);

	if (stmt->default_version)
	{
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
	else
	{
		/*
		 * No explicit default has been given in the command, and we didn't
		 * find one in the catalogs (it must be the first time we hear about
		 * that very extension): we maintain our invariant that we must have a
		 * single line per extension in pg_extension_control where ctldefault
		 * is true.
		 */
		if (default_version == NULL)
			control->default_version = pstrdup(stmt->version);
	}

	/*
	 * In the control structure, find_default_pg_extension_control() has
	 * stuffed the current default full version of the extension, which might
	 * be different from the default version.
	 *
	 * When creating the first template for an extension, we don't have a
	 * default_full_version set yet. To maintain our invariant that we always
	 * have a single version of the extension templates always as the default
	 * full version, if no default_full_version has been found, forcibly set it
	 * now.
	 */
	if (default_version == NULL ||
		default_version->default_full_version == NULL)
	{
		control->default_full_version = stmt->version;
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
CreateExtensionUpdateTemplate(CreateExtTemplateStmt *stmt)
{
	Oid			 owner = GetUserId();
	ExtensionControl *control;

	if (!superuser())
		ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied to create template for extension \"%s\"",
					stmt->extname),
			 errhint("Must be superuser to create a template for an extension.")));

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
	if (access(get_extension_control_filename(stmt->extname), F_OK) == 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("extension \"%s\" is already available", stmt->extname)));
	}

	/*
	 * The update template can change any control properties of the extension,
	 * so first duplicate the properties of the version we are upgrading from
	 * and then override that with the properties given in the command, if any.
	 */
	control = find_pg_extension_control(stmt->extname, stmt->from, false);

	/* Now reset ctldefault and defaultfull, don't blindly copy them */
	control->default_version = NULL;
	control->default_full_version = NULL;

	/* Now read the (optional) control properties from the statement */
	if (stmt->control)
		parse_statement_control_defelems(control, stmt->control);

	return InsertExtensionUpTmplTuple(owner, stmt->extname, control,
									  stmt->from, stmt->to, stmt->script);
}

/*
 * Utility function to build a text[] from the List *requires option.
 */
static Datum
construct_control_requires_datum(List *requires)
{
	Datum	   *datums;
	int			ndatums;
	ArrayType  *a;
	ListCell   *lc;

	ndatums = list_length(requires);
	datums = (Datum *) palloc(ndatums * sizeof(Datum));
	ndatums = 0;
	foreach(lc, requires)
	{
		char	   *curreq = (char *) lfirst(lc);

		datums[ndatums++] =
			DirectFunctionCall1(namein, CStringGetDatum(curreq));
	}
	a = construct_array(datums, ndatums,
						NAMEOID,
						NAMEDATALEN, false, 'c');

	return PointerGetDatum(a);
}

/*
 * Utility function to check control parameters conflicts when providing
 * another path to get to an extension's version (e.g. adding a upgrade
 * script).
 *
 * In details, we allow to create a template for version '1.2' of an extension
 * even if we already had one for '1.1' and an upgrade script from '1.1' to
 * '1.2', but we insist on the setup (control properties) for '1.2' to not be
 * changed in that case. If you want to also change them, use an ALTER command
 * first, then install the new template.
 */
static Oid
check_for_control_conflicts(const ExtensionControl *new_control,
							const char *version)
{
	ExtensionControl *old_control;

	old_control = find_pg_extension_control(new_control->name, version, true);

	if (old_control)
	{
		/*
		 * It must be possible to change default_version and
		 * default_full_version when installing a full script install for an
		 * extension already having a upgrade path to that version.
		 */

		if (strcmp(new_control->schema,
				   old_control->schema) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid setting for \"schema\""),
					 errdetail("Template for extension \"%s\" version \"%s\" is set already with \"schema\" = \"%s\".",
							   new_control->name, version,
							   old_control->schema)));

		if (new_control->relocatable != old_control->relocatable)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid setting for \"relocatable\""),
					 errdetail("Template for extension \"%s\" version \"%s\" is already set with \"relocatable\" = \"%s\".",
							   new_control->name, version,
							   old_control->relocatable ? "true" : "false")));

		if (new_control->superuser != old_control->superuser)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid setting for \"superuser\""),
					 errdetail("Template for extension \"%s\" version \"%s\" is already set with \"superuser\" = \"%s\".",
							   new_control->name, version,
							   old_control->superuser ? "true" : "false")));

		/*
		 * control->requires is a List * of char * extension names.
		 * It's usually empty, or very short.
		 */
		if ((new_control->requires == NIL && old_control->requires != NIL)
			|| (new_control->requires != NIL && old_control->requires == NIL)
			|| (list_length(new_control->requires)
				!= list_length(old_control->requires)))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("invalid setting for \"requires\""),
					 errdetail("Template for extension \"%s\" version \"%s\" is already set with a different \"requires\" list.",
							   new_control->name, version)));

		else if (new_control->requires != NIL && old_control->requires != NIL)
		{
			/* we have to compare two non empty lists of the same size */
			ListCell   *lc1, *lc2;

			foreach(lc1, new_control->requires)
			{
				char *req1 = (char *) lfirst(lc1);
				bool found = false;

				foreach(lc2, old_control->requires)
				{
					char *req2 = (char *) lfirst(lc2);

					if (strcmp(req1, req2) != 0)
					{
						found = true;
						break;
					}
				}
				if (!found)
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
							 errmsg("invalid setting for \"requires\""),
							 errdetail("Template for extension \"%s\" version \"%s\" is already set with a different \"requires\" list.",
									   new_control->name, version)));
			}
			/*
			 * As lists are of the same size, there's no need to check about
			 * elements in the second list not present in the first one, which
			 * means we're done now.
			 */
		}

		return old_control->ctrlOid;
	}
	return InvalidOid;
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

	if (control->default_full_version == NULL)
		values[Anum_pg_extension_control_ctldefaultfull - 1] = false;
	else
		values[Anum_pg_extension_control_ctldefaultfull - 1] = true;

	if (control->requires == NULL)
		nulls[Anum_pg_extension_control_ctlrequires - 1] = true;
	else
		values[Anum_pg_extension_control_ctlrequires - 1] =
			construct_control_requires_datum(control->requires);

	tuple = heap_form_tuple(rel->rd_att, values, nulls);

	extControlOid = simple_heap_insert(rel, tuple);
	CatalogUpdateIndexes(rel, tuple);

	heap_freetuple(tuple);
	heap_close(rel, RowExclusiveLock);

	/*
	 * Record dependencies on owner.
	 *
	 * When we create the extension template and control file, the target
	 * extension, its schema and requirements usually do not exist in the
	 * database. Don't even think about registering a dependency from the
	 * template.
	 */
	recordDependencyOnOwner(ExtensionControlRelationId, extControlOid, owner);

	/* if created from within an extension script, register dependency */
	myself.classId = ExtensionControlRelationId;
	myself.objectId = extControlOid;
	myself.objectSubId = 0;

	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new extension control */
	InvokeObjectPostCreateHook(ExtensionControlRelationId, extControlOid, 0);

	/*
	 * Apply any control-file comment on extension control
	 */
	if (control->comment != NULL)
		CreateComments(extControlOid, ExtensionControlRelationId, 0,
					   control->comment);

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

	/*
	 * Check that no pre-existing control entry exists for our version. That
	 * happens when adding a new full script for a version where you already
	 * have an upgrade path from a previous version.
	 */
	extControlOid = check_for_control_conflicts(control, version);

	if (!OidIsValid(extControlOid))
		/* Then create the companion extension control entry */
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

	/* if created from within an extension script, register dependency */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new extension control */
	InvokeObjectPostCreateHook(ExtensionTemplateRelationId, extTemplateOid, 0);

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
	ObjectAddress myself, ctrl;

	/*
	 * First create the companion extension control entry. In the case of an
	 * Update Template the companion control entry is similar in scope to a
	 * secondary control file, and is attached to the target version.
	 *
	 * Check that no pre-existing control entry exists for the target version
	 * of the upgrade script.
	 */
	extControlOid = check_for_control_conflicts(control, to);

	if (!OidIsValid(extControlOid))
		/* Then create the companion extension control entry */
		extControlOid = InsertExtensionControlTuple(owner, control, to);

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
	ctrl.classId = ExtensionControlRelationId;
	ctrl.objectId = extControlOid;
	ctrl.objectSubId = 0;

	recordDependencyOn(&ctrl, &myself, DEPENDENCY_INTERNAL);

	/* if created from within an extension script, register dependency */
	recordDependencyOnCurrentExtension(&myself, false);

	/* Post creation hook for new extension control */
	InvokeObjectPostCreateHook(ExtensionUpTmplRelationId, extUpTmplOid, 0);

	return extUpTmplOid;
}

/*
 * Lookup functions
 */
char *
get_extension_control_name(Oid ctrlOid)
{
	char	   *result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ctrlOid));

	scandesc = systable_beginscan(rel, ExtensionControlOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(
			NameStr(((Form_pg_extension_control) GETSTRUCT(tuple))->ctlname));
	else
		result = NULL;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}

char *
get_extension_template_name(Oid tmplOid)
{
	char	   *result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionTemplateRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tmplOid));

	scandesc = systable_beginscan(rel, ExtensionTemplateOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(
			NameStr(((Form_pg_extension_template) GETSTRUCT(tuple))->tplname));
	else
		result = NULL;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}

char *
get_extension_uptmpl_name(Oid tmplOid)
{
	char	   *result;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[1];

	rel = heap_open(ExtensionUpTmplRelationId, AccessShareLock);

	ScanKeyInit(&entry[0],
				ObjectIdAttributeNumber,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(tmplOid));

	scandesc = systable_beginscan(rel, ExtensionUpTmplOidIndexId, true,
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		result = pstrdup(
			NameStr(((Form_pg_extension_uptmpl) GETSTRUCT(tuple))->uptname));
	else
		result = NULL;

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return result;
}


/*
 * ALTER TEMPLATE FOR EXTENSION name VERSION version
 *
 * This implements high level routing for sub commands.
 */
Oid
AlterTemplate(AlterExtTemplateStmt *stmt)
{
	switch (stmt->tmpltype)
	{
		case TEMPLATE_CREATE_EXTENSION:
			return AlterExtensionTemplate(stmt);

		case TEMPLATE_UPDATE_EXTENSION:
			return AlterExtensionUpdateTemplate(stmt);
	}
	/* keep compiler happy */
	return InvalidOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION routing
 */
Oid
AlterExtensionTemplate(AlterExtTemplateStmt *stmt)
{
	switch (stmt->cmdtype)
	{
		case AET_SET_DEFAULT:
			return AlterTemplateSetDefault(stmt->extname, stmt->version);

		case AET_SET_DEFAULT_FULL:
			return AlterTemplateSetDefaultFull(stmt->extname, stmt->version);

		case AET_SET_SCRIPT:
			return AlterTemplateSetScript(stmt->extname,
										  stmt->version,
										  stmt->script);

		case AET_UPDATE_CONTROL:
			return AlterTemplateSetControl(stmt->extname,
										   stmt->version,
										   stmt->control);
	}
	/* make compiler happy */
	return InvalidOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION UPDATE routing
 */
Oid
AlterExtensionUpdateTemplate(AlterExtTemplateStmt *stmt)
{
	switch (stmt->cmdtype)
	{
		case AET_SET_DEFAULT:
		case AET_SET_DEFAULT_FULL:
			/* shouldn't happen */
			elog(ERROR, "pg_extension_control is associated to a specific version of an extension, not an update script.");
			break;

		case AET_UPDATE_CONTROL:
			/* shouldn't happen */
			elog(ERROR, "pg_extension_control is associated to a specific version of an extension, not an update script.");
			break;

		case AET_SET_SCRIPT:
			return AlterUpTpmlSetScript(stmt->extname,
										stmt->from,
										stmt->to,
										stmt->script);

	}
	/* make compiler happy */
	return InvalidOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION ... OWNER TO ...
 *
 * In fact we are going to change the owner of all the templates objects
 * related to the extension name given: pg_extension_control entries,
 * pg_extension_template entries and also pg_extension_uptmpl entries.
 *
 * There's no reason to be able to change the owner of only a part of an
 * extension's template (the control but not the template, or just the upgrade
 * script).
 */
Oid
AlterExtensionTemplateOwner(const char *extname, Oid newOwnerId)
{
	int       controls = 0;
	ListCell *lc;

	/* Alter owner of all pg_extension_control entries for extname */
	foreach(lc, list_pg_extension_control_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		controls++;
		elog(DEBUG1, "alter owner of pg_extension_control %u", objectId);

		catalog = heap_open(ExtensionControlRelationId, RowExclusiveLock);
		AlterObjectOwner_internal(catalog, objectId, newOwnerId);
		heap_close(catalog, RowExclusiveLock);
	}

	if (controls == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("template for extension \"%s\" does not exist", extname)));

	/* Alter owner of all pg_extension_template entries for extname */
	foreach(lc, list_pg_extension_template_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		elog(DEBUG1, "alter owner of pg_extension_template %u", objectId);

		catalog = heap_open(ExtensionTemplateRelationId, RowExclusiveLock);
		AlterObjectOwner_internal(catalog, objectId, newOwnerId);
		heap_close(catalog, RowExclusiveLock);
	}

	/* Alter owner of all pg_extension_uptmpl entries for extname */
	foreach(lc, list_pg_extension_uptmpl_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		elog(DEBUG1, "alter owner of pg_extension_uptmpl %u", objectId);

		catalog = heap_open(ExtensionUpTmplRelationId, RowExclusiveLock);
		AlterObjectOwner_internal(catalog, objectId, newOwnerId);
		heap_close(catalog, RowExclusiveLock);
	}

	/* which Oid to return here? */
	return InvalidOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION ... RENAME TO ...
 *
 * There's no reason to be able to change the name of only a part of an
 * extension's template (the control but not the template, or just the upgrade
 * script).
 */
Oid
AlterExtensionTemplateRename(const char *extname, const char *newname)
{
	int       controls = 0;
	ListCell *lc;

	/*
	 * Forbid renaming a template that's already in use: we wouldn't be able to
	 * pg_restore after that.
	 */
	if (get_extension_oid(extname, true) != InvalidOid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_IN_USE),
				 errmsg("template for extension \"%s\" is in use", extname),
				 errdetail("extension \"%s\" already exists", extname)));
	}

	/* Check that we don't already have an extension of this name available. */
	if (!CheckExtensionAvailability(newname, NULL, false))
		/* Messages have already been sent to the client */
		return InvalidOid;

	/* Rename all pg_extension_control entries for extname */
	foreach(lc, list_pg_extension_control_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		controls++;
		elog(DEBUG1, "rename pg_extension_control %u", objectId);

		catalog = heap_open(ExtensionControlRelationId, RowExclusiveLock);
		AlterObjectRename_internal(catalog, objectId, newname);
		heap_close(catalog, RowExclusiveLock);
	}

	if (controls == 0)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("template for extension \"%s\" does not exist", extname)));

	/* Rename all pg_extension_template entries for extname */
	foreach(lc, list_pg_extension_template_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		elog(DEBUG1, "rename pg_extension_template %u", objectId);

		catalog = heap_open(ExtensionTemplateRelationId, RowExclusiveLock);
		AlterObjectRename_internal(catalog, objectId, newname);
		heap_close(catalog, RowExclusiveLock);
	}

	/* Rename all pg_extension_uptmpl entries for extname */
	foreach(lc, list_pg_extension_uptmpl_oids_for(extname))
	{
		Relation		catalog;
		Oid				objectId = lfirst_oid(lc);

		elog(DEBUG1, "rename pg_extension_uptmpl %u", objectId);

		catalog = heap_open(ExtensionUpTmplRelationId, RowExclusiveLock);
		AlterObjectRename_internal(catalog, objectId, newname);
		heap_close(catalog, RowExclusiveLock);
	}

	/* which Oid to return here? */
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
		find_default_pg_extension_control(extname, false);

	if (!pg_extension_control_ownercheck(current->ctrlOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EXTCONTROL,
					   extname);

	/* silently do nothing if the default is already set as wanted */
	if (strcmp(current->default_version, version) == 0)
		return current->ctrlOid;

	/* set ctldefault to false on current default extension */
	modify_pg_extension_control_default(current->name,
										current->default_version,
										false);

	/* set ctldefault to true on new default extension */
	return modify_pg_extension_control_default(extname, version, true);
}

/*
 * Implement flipping the ctldefaultfull bit to given value.
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
								  NULL, 2, entry);

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
 * ALTER TEMPLATE FOR EXTENSION ... SET DEFAULT FULL VERSION ...
 */
static Oid
AlterTemplateSetDefaultFull(const char *extname, const char *version)
{
	/* we need to know who's the default */
	ExtensionControl *current =
		find_default_pg_extension_control(extname, false);

	/* the target version must be an installation script */
	Oid target = get_template_oid(extname, version, false);

	(void) target;				/* silence compiler */

	/* only check the owner of one of those, as we maintain them all the same */
	if (!pg_extension_control_ownercheck(current->ctrlOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EXTCONTROL,
					   extname);

	/* silently do nothing if the default is already set as wanted */
	if (strcmp(current->default_full_version, version) == 0)
		return current->ctrlOid;

	/* set ctldefault to false on current default extension */
	modify_pg_extension_control_default_full(current->name,
											 current->default_full_version,
											 false);

	/* set ctldefault to true on new default extension */
	return modify_pg_extension_control_default_full(extname, version, true);
}

/*
 * Implement flipping the ctldefaultfull bit to given value.
 */
static Oid
modify_pg_extension_control_default_full(const char *extname,
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
								  NULL, 2, entry);

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

	values[Anum_pg_extension_control_ctldefaultfull - 1] = BoolGetDatum(value);
	repl[Anum_pg_extension_control_ctldefaultfull - 1] = true;

	tuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
							  values, nulls, repl);

	simple_heap_update(rel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return ctrlOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION ... AS $$ ... $$
 */
static Oid
AlterTemplateSetScript(const char *extname,
					   const char *version,
					   const char *script)
{
	Oid			extTemplateOid;
	Relation	rel;
	SysScanDesc	scandesc;
	HeapTuple	tuple;
	ScanKeyData	entry[2];
	Datum		values[Natts_pg_extension_template];
	bool		nulls[Natts_pg_extension_template];
	bool		repl[Natts_pg_extension_template];

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
								  NULL, 2, entry);

	tuple = systable_getnext(scandesc);

	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR,
			 "pg_extension_template for extension \"%s\" version \"%s\" does not exist",
			 extname, version);

	extTemplateOid = HeapTupleGetOid(tuple);

	/* check privileges */
	if (!pg_extension_template_ownercheck(extTemplateOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EXTTEMPLATE,
					   extname);

	/* Modify ctldefault in the pg_extension_control tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(repl, 0, sizeof(repl));

	repl[Anum_pg_extension_template_tplscript - 1] = true;
	values[Anum_pg_extension_template_tplscript - 1] =
		CStringGetTextDatum(script);

	tuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
							  values, nulls, repl);

	simple_heap_update(rel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return extTemplateOid;
}

/*
 * ALTER TEMPLATE FOR EXTENSION ... FROM ... TO ... AS $$ ... $$
 */
static Oid
AlterUpTpmlSetScript(const char *extname,
					 const char *from,
					 const char *to,
					 const char *script)
{
	Oid			extUpTmplOid;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[3];
	Datum		values[Natts_pg_extension_uptmpl];
	bool		nulls[Natts_pg_extension_uptmpl];
	bool		repl[Natts_pg_extension_uptmpl];

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
								  NULL, 3, entry);

	tuple = systable_getnext(scandesc);

	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR,
			 "pg_extension_template for extension \"%s\" from version \"%s\" to version \"%s\" does not exist",
			 extname, from, to);

	extUpTmplOid = HeapTupleGetOid(tuple);

	/* check privileges */
	if (!pg_extension_uptmpl_ownercheck(extUpTmplOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EXTUPTMPL,
					   extname);

	/* Modify ctldefault in the pg_extension_control tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(repl, 0, sizeof(repl));

	repl[Anum_pg_extension_uptmpl_uptscript - 1] = true;
	values[Anum_pg_extension_uptmpl_uptscript - 1] =
		CStringGetTextDatum(script);

	tuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
							  values, nulls, repl);

	simple_heap_update(rel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return extUpTmplOid;
}

/*
 * AlterTemplateSetControl
 */
static Oid
AlterTemplateSetControl(const char *extname,
						const char *version,
						List *options)
{
	Oid         ctrlOid;
	Relation	rel;
	SysScanDesc scandesc;
	HeapTuple	tuple;
	ScanKeyData entry[2];
	Datum		values[Natts_pg_extension_control];
	bool		nulls[Natts_pg_extension_control];
	bool		repl[Natts_pg_extension_control];

	ExtensionControl *current_control;
 	ExtensionControl *new_control =
		(ExtensionControl *) palloc0(sizeof(ExtensionControl));

	/* parse the new control options given in the SQL command */
	new_control->name = pstrdup(extname);
	parse_statement_control_defelems(new_control, options);

	/* now find the tuple we want to edit */
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
								  NULL, 2, entry);

	tuple = systable_getnext(scandesc);

	if (!HeapTupleIsValid(tuple))		/* should not happen */
		elog(ERROR,
			 "pg_extension_control for extension \"%s\" version \"%s\" does not exist",
			 extname, version);

	ctrlOid = HeapTupleGetOid(tuple);

	/* check privileges */
	if (!pg_extension_control_ownercheck(ctrlOid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_EXTCONTROL,
					   extname);

	current_control = read_pg_extension_control(extname, rel, tuple);

	/* Modify the pg_extension_control tuple */
	memset(values, 0, sizeof(values));
	memset(nulls, 0, sizeof(nulls));
	memset(repl, 0, sizeof(repl));

	/*
	 * We don't compare with the current value, we directly set what's been
	 * given in the new command, if anything was given
	 */
	if (new_control->schema)
	{
		values[Anum_pg_extension_control_ctlnamespace - 1] =
			DirectFunctionCall1(namein, CStringGetDatum((new_control->schema)));
		repl[Anum_pg_extension_control_ctlnamespace - 1] = true;
	}
	if (new_control->requires)
	{
		values[Anum_pg_extension_control_ctlrequires - 1] =
			construct_control_requires_datum(new_control->requires);
		repl[Anum_pg_extension_control_ctlrequires - 1] = true;
	}

	/*
	 * We need to compare superuser and relocatable because those are bools,
	 * and we don't have a NULL pointer when they were omited in the command.
	 */
	if (new_control->superuser != current_control->superuser)
	{
		values[Anum_pg_extension_control_ctlsuperuser - 1] =
			BoolGetDatum(new_control->superuser);
		repl[Anum_pg_extension_control_ctlsuperuser - 1] = true;
	}
	if (new_control->relocatable != current_control->relocatable)
	{
		values[Anum_pg_extension_control_ctlrelocatable - 1] =
			BoolGetDatum(new_control->relocatable);
		repl[Anum_pg_extension_control_ctlrelocatable - 1] = true;
	}

	tuple = heap_modify_tuple(tuple, RelationGetDescr(rel),
							  values, nulls, repl);

	simple_heap_update(rel, &tuple->t_self, tuple);
	CatalogUpdateIndexes(rel, tuple);

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return ctrlOid;
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
								  NULL, 2, entry);

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
								  NULL, 1, entry);

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
								  NULL, 3, entry);

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
								  NULL, 1, entry);

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
								  NULL, 1, entry);

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
								  NULL, 1, entry);

	tuple = systable_getnext(scandesc);

	/* We assume that there can be at most one matching tuple */
	if (HeapTupleIsValid(tuple))
		simple_heap_delete(rel, &tuple->t_self);

	systable_endscan(scandesc);

	heap_close(rel, RowExclusiveLock);
}

/*
 * Internal tool to get the version string of a pg_extension_control tuple.
 */
static char *
extract_ctlversion(Relation rel, HeapTuple tuple)
{
	bool	isnull;
	Datum	dvers;

	dvers = heap_getattr(tuple, Anum_pg_extension_control_ctlversion,
						 RelationGetDescr(rel), &isnull);
	if (isnull)
		elog(ERROR, "invalid null extension version");

	return text_to_cstring(DatumGetTextPP(dvers));
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
	control->ctrlOid = HeapTupleGetOid(tuple);

	if (extname)
		control->name = pstrdup(extname);
	else
		control->name = pstrdup(NameStr(ctrl->ctlname));

	control->is_template = true;
	control->relocatable = ctrl->ctlrelocatable;
	control->superuser = ctrl->ctlsuperuser;
	control->schema = pstrdup(NameStr(ctrl->ctlnamespace));

	/* set control->version */
	control->version = extract_ctlversion(rel, tuple);

	/* set control->default_version */
	if (ctrl->ctldefault)
		control->default_version = extract_ctlversion(rel, tuple);

	/* set control->default_full_version */
	if (ctrl->ctldefaultfull)
	{
		/* avoid extracting it again if we just did so */
		if (control->default_version)
			control->default_full_version = control->default_version;
		else
			control->default_full_version = extract_ctlversion(rel, tuple);
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
								  NULL, 2, entry);

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

	/* Don't forget the comments! */
	if (control != NULL)
		control->comment = GetComment(control->ctrlOid,
									  ExtensionControlRelationId, 0);

	return control;
}

/*
 * Find the default extension's control properties, and its OID, for internal
 * use (such as checking ACLs).
 *
 * In a single scan of the pg_extension_control catalog, also find out the
 * default full version of that extension, needed in extension.c when doing
 * multi steps CREATE EXTENSION.
 */
ExtensionControl *
find_default_pg_extension_control(const char *extname, bool missing_ok)
{
	ExtensionControl *control = NULL;
	char *default_full_version = NULL;
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
								  NULL, 1, entry);

	/* find all the control tuples for extname */
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		bool	isnull;
		Datum	tmpdatum;
		bool	ctldefault;
		bool	ctldefaultfull;

		tmpdatum = fastgetattr(tuple,
							   Anum_pg_extension_control_ctldefault,
							   RelationGetDescr(rel), &isnull);
		if (isnull)
			elog(ERROR, "invalid null ctldefault");
		ctldefault = DatumGetBool(tmpdatum);

		/* only one of these is the default */
		if (ctldefault)
		{
			if (!control)
				control = read_pg_extension_control(extname, rel, tuple);
			else
				/* should not happen */
				elog(ERROR,
					 "extension \"%s\" has more than one default control template",
					 extname);
		}

		tmpdatum = fastgetattr(tuple,
							   Anum_pg_extension_control_ctldefaultfull,
							   RelationGetDescr(rel), &isnull);
		if (isnull)
			elog(ERROR, "invalid null ctldefaultfull");
		ctldefaultfull = DatumGetBool(tmpdatum);

		/* the default version and default full version might be different */
		if (ctldefaultfull)
			default_full_version = extract_ctlversion(rel, tuple);
	}
	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	/* we really need a single default version. */
	if (!control && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("extension \"%s\" has no default control template",
						extname)));

	/* don't forget to add in the default full version */
	if (control != NULL && default_full_version)
		control->default_full_version = default_full_version;

	/* Don't forget the comments! */
	if (control != NULL)
		control->comment = GetComment(control->ctrlOid,
									  ExtensionControlRelationId, 0);

	return control;
}

/*
 * read_pg_extension_template_script
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
								  NULL, 2, entry);

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
								  NULL, 3, entry);

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
								  NULL, 1, entry);

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
								  NULL, 1, entry);

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

/*
 * pg_extension_controls
 *
 * List all extensions for which we have a default control entry. Returns a
 * sorted list of name, version, comment of such extensions.
 */
List *
pg_extension_default_controls(void)
{
	List		*extensions = NIL;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	scandesc = systable_beginscan(rel,
								  ExtensionControlNameVersionIndexId, true,
								  NULL, 0, NULL);

	/* find all the control tuples for extname */
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_extension_control ctrl =
			(Form_pg_extension_control) GETSTRUCT(tuple);

		bool isnull;
		bool ctldefault =
			DatumGetBool(
				fastgetattr(tuple, Anum_pg_extension_control_ctldefault,
							RelationGetDescr(rel), &isnull));

		/* only of those is the default */
		if (ctldefault)
		{
			Datum dvers =
				heap_getattr(tuple, Anum_pg_extension_control_ctlversion,
							 RelationGetDescr(rel), &isnull);

			char *version = isnull? NULL:text_to_cstring(DatumGetTextPP(dvers));
			char *comment = GetComment(HeapTupleGetOid(tuple),
									   ExtensionControlRelationId, 0);

			extensions = lappend(extensions,
								 list_make3(pstrdup(NameStr(ctrl->ctlname)),
											version,
											comment));
		}
	}

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return extensions;
}

/*
 * pg_extension_controls
 *
 * List all extensions for which we have a default control entry. Returns a
 * sorted list of ExtensionControl *.
 */
List *
pg_extension_controls(void)
{
	List		*extensions = NIL;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;

	rel = heap_open(ExtensionControlRelationId, AccessShareLock);

	scandesc = systable_beginscan(rel,
								  ExtensionControlNameVersionIndexId, true,
								  SnapshotNow, 0, NULL);

	/* find all the control tuples for extname */
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		ExtensionControl *control = read_pg_extension_control(NULL, rel, tuple);

		extensions = lappend(extensions, control);
	}

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return extensions;
}

/*
 * pg_extension_templates
 *
 * Return a list of list of (name, version) of extensions available to install
 * from templates, in alphabetical order.
 */
List *
pg_extension_templates(void)
{
	List		*templates = NIL;
	Relation	 rel;
	SysScanDesc	 scandesc;
	HeapTuple	 tuple;
	ScanKeyData	 entry[1];

	rel = heap_open(ExtensionTemplateRelationId, AccessShareLock);

	scandesc = systable_beginscan(rel,
								  ExtensionTemplateNameVersionIndexId, true,
								  NULL, 0, entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
	{
		Form_pg_extension_template tmpl =
			(Form_pg_extension_template) GETSTRUCT(tuple);

		bool	isnull;

		Datum dvers =
			heap_getattr(tuple, Anum_pg_extension_template_tplversion,
						 RelationGetDescr(rel), &isnull);

		char *version = isnull? NULL : text_to_cstring(DatumGetTextPP(dvers));

		templates = lappend(templates,
							list_make2(pstrdup(NameStr(tmpl->tplname)),
									   version));
	}

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return templates;
}

List *
list_pg_extension_control_oids_for(const char *extname)
{
	List       *oids = NIL;
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
								  NULL, 1, entry);

	/* find all the control tuples for extname */
	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
		oids = lappend_oid(oids, HeapTupleGetOid(tuple));

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return oids;
}

List *
list_pg_extension_template_oids_for(const char *extname)
{
	List        *oids = NIL;
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
								  NULL, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
		oids = lappend_oid(oids, HeapTupleGetOid(tuple));

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return oids;
}

List *
list_pg_extension_uptmpl_oids_for(const char *extname)
{
	List        *oids = NIL;
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
								  NULL, 1, entry);

	while (HeapTupleIsValid(tuple = systable_getnext(scandesc)))
		oids = lappend_oid(oids, HeapTupleGetOid(tuple));

	systable_endscan(scandesc);

	heap_close(rel, AccessShareLock);

	return oids;
}
