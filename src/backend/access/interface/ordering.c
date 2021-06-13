/*-------------------------------------------------------------------------
 *
 * ordering.c
 *	  ordering interface access method routines
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/interface/ordering.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/ifam.h"
#include "access/nbtree.h"
#include "utils/fmgrprotos.h"

/*
 * Ordering handler function: return InterfaceAmRoutine with access method
 * parameters and callbacks.
 */
Datum
ordering_ifam_handler(PG_FUNCTION_ARGS)
{
	InterfaceAmRoutine *amroutine = makeNode(InterfaceAmRoutine);

	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amoptsprocnum = BTOPTIONS_PROC;
	amroutine->amcanorder = true;
	amroutine->amcanorderbyop = false;
	amroutine->amvalidate = btvalidate;

	PG_RETURN_POINTER(amroutine);
}
