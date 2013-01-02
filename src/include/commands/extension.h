/*-------------------------------------------------------------------------
 *
 * extension.h
 *		Extension management commands (create/drop extension).
 *
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/extension.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef EXTENSION_H
#define EXTENSION_H

#include "nodes/parsenodes.h"


/*
 * creating_extension is only true while running a CREATE EXTENSION command.
 * It instructs recordDependencyOnCurrentExtension() to register a dependency
 * on the current pg_extension object for each SQL object created by its
 * installation script.
 */
extern bool creating_extension;
extern Oid	CurrentExtensionObject;

/*
 * Internal data structure to hold the results of parsing a control file
 */
typedef struct ExtensionControlFile
{
	char	   *name;			/* name of the extension */
	char	   *directory;		/* directory for script files */
	char	   *default_version;	/* default install target version, if any */
	char	   *module_pathname;	/* string to substitute for MODULE_PATHNAME */
	char	   *comment;		/* comment, if any */
	char	   *schema;			/* target schema (allowed if !relocatable) */
	bool		relocatable;	/* is ALTER EXTENSION SET SCHEMA supported? */
	bool		superuser;		/* must be superuser to install? */
	int			encoding;		/* encoding of the script file, or -1 */
	List	   *requires;		/* names of prerequisite extensions */
} ExtensionControlFile;

extern ExtensionControlFile *read_extension_control_file(const char *extname);
extern void check_valid_extension_name(const char *extensionname);

extern Oid CreateExtension(CreateExtensionStmt *stmt);

extern void RemoveExtensionById(Oid extId);

extern Oid InsertExtensionTuple(const char *extName, Oid extOwner,
					 Oid schemaOid, bool relocatable, const char *extVersion,
					 Datum extConfig, Datum extCondition,
					 List *requiredExtensions);

extern Oid ExecAlterExtensionStmt(AlterExtensionStmt *stmt);

extern Oid ExecAlterExtensionContentsStmt(AlterExtensionContentsStmt *stmt);

extern Oid	get_extension_oid(const char *extname, bool missing_ok);
extern char *get_extension_name(Oid ext_oid);

extern Oid AlterExtensionNamespace(List *names, const char *newschema);

extern void AlterExtensionOwner_oid(Oid extensionOid, Oid newOwnerId);

#endif   /* EXTENSION_H */
