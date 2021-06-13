/*-------------------------------------------------------------------------
 *
 * ifamapi.c
 *	  interface access method routines
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/interface/ifamapi.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/ifam.h"
#include "catalog/pg_am.h"
#include "catalog/pg_opclass.h"
#include "utils/fmgrprotos.h"
#include "utils/syscache.h"

/*
 * GetInterfaceAmRoutine - call the specified access method handler routine
 * to get its InterfaceAmRoutine struct, which will be palloc'd in the caller's
 * context.
 *
 * Note that if the amhandler function is built-in, this will not involve
 * any catalog access.  It's therefore safe to use this while bootstrapping
 * indexes for the system catalogs.  relcache.c relies on that.
 */
static InterfaceAmRoutine *
GetInterfaceAmRoutine(Oid amhandler)
{
	Datum		datum;
	InterfaceAmRoutine *routine;

	datum = OidFunctionCall0(amhandler);
	routine = (InterfaceAmRoutine *) DatumGetPointer(datum);

	if (routine == NULL || !IsA(routine, InterfaceAmRoutine))
		elog(ERROR, "interface access method handler function %u did not return an InterfaceAmRoutine struct",
			 amhandler);

	return routine;
}

/*
 * TranslateIndexToInterfaceAmRoutine - gets the IndexAmRoutine struct and
 * turnes it to InterfaceAmRoutine, which will be palloc'd in the caller's
 * context.
 */
static InterfaceAmRoutine *
GetInterfaceAmRoutineFromIndexAmHander(Oid amhandler)
{
	IndexAmRoutine *indexroutine;
	InterfaceAmRoutine *ifroutine;

	indexroutine = GetIndexAmRoutine(amhandler);

	ifroutine = makeNode(InterfaceAmRoutine);

	ifroutine->amstrategies = indexroutine->amstrategies;
	ifroutine->amsupport = indexroutine->amsupport;
	ifroutine->amoptsprocnum = indexroutine->amoptsprocnum;
	ifroutine->amcanorder = indexroutine->amcanorder;
	ifroutine->amcanorderbyop = indexroutine->amcanorderbyop;
	ifroutine->amvalidate = indexroutine->amvalidate;
	ifroutine->amadjustmembers = indexroutine->amadjustmembers;

	pfree(indexroutine);

	return ifroutine;
}

/*
 * GetInterfaceAmRoutineByAmId - look up the handler of the index access method
 * with the given OID, and get its InterfaceAmRoutine struct.
 */
InterfaceAmRoutine *
GetInterfaceAmRoutineByAmId(Oid amoid)
{
	char		amtype;
	HeapTuple	tuple;
	Form_pg_am	amform;
	regproc		amhandler;

	/* Get handler function OID for the access method */
	tuple = SearchSysCache1(AMOID, ObjectIdGetDatum(amoid));
	if (!HeapTupleIsValid(tuple))
		elog(ERROR, "cache lookup failed for access method %u",
			 amoid);

	amform = (Form_pg_am) GETSTRUCT(tuple);
	amtype = amform->amtype;

	/* Check if it's an index access method as opposed to some other AM */
	if (amtype != AMTYPE_INTERFACE && amtype != AMTYPE_INDEX)
		elog(ERROR, "access method \"%s\" is not of type INDEX or INTERFACE",
			 NameStr(amform->amname));

	amhandler = amform->amhandler;

	/* Complain if handler OID is invalid */
	if (!RegProcedureIsValid(amhandler))
		elog(ERROR, "access method \"%s\" does not have a handler",
			 NameStr(amform->amname));

	ReleaseSysCache(tuple);

	/* And finally, call the handler function to get the API struct */
	if (amtype == AMTYPE_INTERFACE)
		return GetInterfaceAmRoutine(amhandler);
	else
		return GetInterfaceAmRoutineFromIndexAmHander(amhandler);
}

/*
 * Ask appropriate access method to validate the specified opclass.
 */
Datum
amvalidate(PG_FUNCTION_ARGS)
{
	Oid			opclassoid = PG_GETARG_OID(0);
	bool		result;
	HeapTuple	classtup;
	Form_pg_opclass classform;
	Oid			amoid;
	InterfaceAmRoutine *amroutine;

	classtup = SearchSysCache1(CLAOID, ObjectIdGetDatum(opclassoid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for operator class %u", opclassoid);
	classform = (Form_pg_opclass) GETSTRUCT(classtup);

	amoid = classform->opcmethod;

	ReleaseSysCache(classtup);

	amroutine = GetInterfaceAmRoutineByAmId(amoid);

	if (amroutine->amvalidate == NULL)
		elog(ERROR, "function amvalidate is not defined for index access method %u",
			 amoid);

	result = amroutine->amvalidate(opclassoid);

	pfree(amroutine);

	PG_RETURN_BOOL(result);
}
