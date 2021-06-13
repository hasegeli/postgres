/*-------------------------------------------------------------------------
 *
 * amcmds.c
 *	  Routines for SQL commands that manipulate access methods.
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/amcmds.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amimplements.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"


static void StoreCatalogAmimpments(Oid implementing, List *implements);
static Oid	lookup_am_handler_func(List *handler_name, char amtype);
static Oid get_am_type_oid(const char *amname, char amtype, char amtype2, bool missing_ok);
static const char *get_am_type_string(char amtype);


/*
 * CreateAccessMethod
 *		Registers a new access method.
 */
ObjectAddress
CreateAccessMethod(CreateAmStmt *stmt)
{
	Relation	rel;
	ObjectAddress myself;
	ObjectAddress referenced;
	Oid			amoid;
	Oid			amhandler;
	bool		nulls[Natts_pg_am];
	Datum		values[Natts_pg_am];
	HeapTuple	tup;
	List	   *implementsoids;
	ListCell   *listptr;

	rel = table_open(AccessMethodRelationId, RowExclusiveLock);

	/* Must be super user */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to create access method \"%s\"",
						stmt->amname),
				 errhint("Must be superuser to create an access method.")));

	/* Check if name is used */
	amoid = GetSysCacheOid1(AMNAME, Anum_pg_am_oid,
							CStringGetDatum(stmt->amname));
	if (OidIsValid(amoid))
	{
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("access method \"%s\" already exists",
						stmt->amname)));
	}

	/*
	 * Get the handler function oid, verifying the AM type while at it.
	 */
	amhandler = lookup_am_handler_func(stmt->handler_name, stmt->amtype);

	/*
	 * Determine the list of OIDs of the implemented access methods
	 */
	implementsoids = NIL;
	foreach(listptr, stmt->implements)
	{
		char	   *name = strVal(lfirst(listptr));
		Oid			oid = get_am_type_oid(name, AMTYPE_INTERFACE, '\0', false);

		/* Reject duplications */
		if (list_member_oid(implementsoids, oid))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_TABLE),
					 errmsg("access method \"%s\" would be implemented more than once",
							name)));

		implementsoids = lappend_oid(implementsoids, oid);
	}

	if (implementsoids != NIL && stmt->amtype != AMTYPE_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("only index access methods can implement interfaces")));

	/*
	 * Insert tuple into pg_am.
	 */
	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	amoid = GetNewOidWithIndex(rel, AmOidIndexId, Anum_pg_am_oid);
	values[Anum_pg_am_oid - 1] = ObjectIdGetDatum(amoid);
	values[Anum_pg_am_amname - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->amname));
	values[Anum_pg_am_amhandler - 1] = ObjectIdGetDatum(amhandler);
	values[Anum_pg_am_amtype - 1] = CharGetDatum(stmt->amtype);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);

	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	myself.classId = AccessMethodRelationId;
	myself.objectId = amoid;
	myself.objectSubId = 0;

	/* Record dependency on handler function */
	referenced.classId = ProcedureRelationId;
	referenced.objectId = amhandler;
	referenced.objectSubId = 0;

	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);

	recordDependencyOnCurrentExtension(&myself, false);

	InvokeObjectPostCreateHook(AccessMethodRelationId, amoid, 0);

	table_close(rel, RowExclusiveLock);

	StoreCatalogAmimpments(amoid, implementsoids);

	return myself;
}

/*
 * StoreCatalogAmimplements
 *      Updates the system catalogs with proper implements information.
 */
static void
StoreCatalogAmimpments(Oid implementing, List *implements)
{
	int			seq;
	ListCell   *listptr;
	Relation	amirelation;
	ObjectAddress childobject,
				parentobject;
	Datum		values[Natts_pg_amimplements];
	bool		nulls[Natts_pg_amimplements];
	HeapTuple	tuple;

	AssertArg(OidIsValid(implementing));

	/* Prepare to insert into pg_amimplements */
	amirelation = table_open(AmimplementsRelationId, RowExclusiveLock);
	values[Anum_pg_amimplements_amiamid - 1] = ObjectIdGetDatum(implementing);
	memset(nulls, 0, sizeof(nulls));

	/* Prepare the dependency objects */
	parentobject.classId = AccessMethodRelationId;
	parentobject.objectSubId = 0;
	childobject.classId = AccessMethodRelationId;
	childobject.objectId = implementing;
	childobject.objectSubId = 0;

	seq = 1;
	foreach(listptr, implements)
	{
		Oid			oid = lfirst_oid(listptr);

		AssertArg(OidIsValid(listptr));

		/* Store pg_amimplements entry */
		values[Anum_pg_amimplements_amiparent - 1] = ObjectIdGetDatum(oid);
		values[Anum_pg_amimplements_amiseqno - 1] = Int32GetDatum(seq);
		tuple = heap_form_tuple(RelationGetDescr(amirelation), values, nulls);
		CatalogTupleInsert(amirelation, tuple);
		heap_freetuple(tuple);

		/* Store a dependency too */
		parentobject.objectId = oid;
		recordDependencyOn(&childobject, &parentobject, DEPENDENCY_NORMAL);

		seq++;
	}

	table_close(amirelation, RowExclusiveLock);
}

/*
 * get_am_type_oid
 *		Worker for various get_am_*_oid variants
 *
 * If missing_ok is false, throw an error if access method not found.  If
 * true, just return InvalidOid.
 *
 * If amtype is not '\0', an error is raised if the AM found is not of the
 * given types.
 */
static Oid
get_am_type_oid(const char *amname, char amtype, char amtype2, bool missing_ok)
{
	HeapTuple	tup;
	Oid			oid = InvalidOid;

	tup = SearchSysCache1(AMNAME, CStringGetDatum(amname));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		if (amtype != '\0' &&
			amform->amtype != amtype)
		{
			if (amtype2 == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("access method \"%s\" is not of type %s",
								NameStr(amform->amname),
								get_am_type_string(amtype))));
			else if (amform->amtype != amtype2)
				ereport(ERROR,
						(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						 errmsg("access method \"%s\" is not of type %s or %s",
								NameStr(amform->amname),
								get_am_type_string(amtype),
								get_am_type_string(amtype2))));
		}

		oid = amform->oid;
		ReleaseSysCache(tup);
	}

	if (!OidIsValid(oid) && !missing_ok)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("access method \"%s\" does not exist", amname)));
	return oid;
}

/*
 * get_interface_or_index_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to an index or interface AM.
 */
Oid
get_interface_or_index_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_INTERFACE, AMTYPE_INDEX, missing_ok);
}

/*
 * get_table_am_oid - given an access method name, look up its OID
 *		and verify it corresponds to an table AM.
 */
Oid
get_table_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, AMTYPE_TABLE, '\0', missing_ok);
}

/*
 * get_am_oid - given an access method name, look up its OID.
 *		The type is not checked.
 */
Oid
get_am_oid(const char *amname, bool missing_ok)
{
	return get_am_type_oid(amname, '\0', '\0', missing_ok);
}

/*
 * get_am_name - given an access method OID, look up its name.
 */
char *
get_am_name(Oid amOid)
{
	HeapTuple	tup;
	char	   *result = NULL;

	tup = SearchSysCache1(AMOID, ObjectIdGetDatum(amOid));
	if (HeapTupleIsValid(tup))
	{
		Form_pg_am	amform = (Form_pg_am) GETSTRUCT(tup);

		result = pstrdup(NameStr(amform->amname));
		ReleaseSysCache(tup);
	}
	return result;
}

/*
 * Convert single-character access method type into string for error reporting.
 */
static const char *
get_am_type_string(char amtype)
{
	switch (amtype)
	{
		case AMTYPE_INTERFACE:
			return "INTERFACE";
		case AMTYPE_INDEX:
			return "INDEX";
		case AMTYPE_TABLE:
			return "TABLE";
		default:
			/* shouldn't happen */
			elog(ERROR, "invalid access method type '%c'", amtype);
			return NULL;		/* keep compiler quiet */
	}
}

/*
 * Convert a handler function name to an Oid.  If the return type of the
 * function doesn't match the given AM type, an error is raised.
 *
 * This function either return valid function Oid or throw an error.
 */
static Oid
lookup_am_handler_func(List *handler_name, char amtype)
{
	Oid			handlerOid;
	Oid			funcargtypes[1] = {INTERNALOID};
	Oid			expectedType = InvalidOid;

	if (handler_name == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("handler function is not specified")));

	/* handlers have one argument of type internal */
	handlerOid = LookupFuncName(handler_name, 1, funcargtypes, false);

	/* check that handler has the correct return type */
	switch (amtype)
	{
		case AMTYPE_INTERFACE:
			expectedType = INTERFACE_AM_HANDLEROID;
			break;
		case AMTYPE_INDEX:
			expectedType = INDEX_AM_HANDLEROID;
			break;
		case AMTYPE_TABLE:
			expectedType = TABLE_AM_HANDLEROID;
			break;
		default:
			elog(ERROR, "unrecognized access method type \"%c\"", amtype);
	}

	if (get_func_rettype(handlerOid) != expectedType)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("function %s must return type %s",
						get_func_name(handlerOid),
						format_type_extended(expectedType, -1, 0))));

	return handlerOid;
}
