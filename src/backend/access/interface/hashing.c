/*-------------------------------------------------------------------------
 *
 * hashing.c
 *	  hashing interface access method routines
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/interface/hashing.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/ifam.h"
#include "access/hash.h"
#include "utils/fmgrprotos.h"

/*
 * Hashing handler function: return InterfaceAmRoutine with access method
 * parameters and callbacks.
 */
Datum
hashing_ifam_handler(PG_FUNCTION_ARGS)
{
	InterfaceAmRoutine *amroutine = makeNode(InterfaceAmRoutine);

	amroutine->amstrategies = HTMaxStrategyNumber;
	amroutine->amsupport = HASHNProcs;
	amroutine->amoptsprocnum = HASHOPTIONS_PROC;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amvalidate = hashvalidate;

	PG_RETURN_POINTER(amroutine);
}
