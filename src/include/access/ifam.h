/*-------------------------------------------------------------------------
 *
 * ifam.h
 *	  API for Postgres interface access methods
 *
 * Copyright (c) 2015-2021, PostgreSQL Global Development Group
 *
 * src/include/access/ifam.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef IFAM_H
#define IFAM_H

#include "access/genam.h"

/* validate definition of an opclass for this AM */
typedef bool (*amvalidate_function) (Oid opclassoid);

/* validate operators and support functions to be added to an opclass/family */
typedef void (*amadjustmembers_function) (Oid opfamilyoid,
										  Oid opclassoid,
										  List *operators,
										  List *functions);

/*
 * API struct for an index AM.  Note this must be stored in a single palloc'd
 * chunk of memory.
 */
typedef struct InterfaceAmRoutine
{
	NodeTag		type;

	/*
	 * Total number of strategies (operators) by which we can traverse/search
	 * this AM.  Zero if AM does not have a fixed set of strategy assignments.
	 */
	uint16		amstrategies;
	/* total number of support functions that this AM uses */
	uint16		amsupport;
	/* opclass options support function number or 0 */
	uint16		amoptsprocnum;
	/* does AM support ORDER BY indexed column's value? */
	bool		amcanorder;
	/* does AM support ORDER BY result of an operator on indexed column? */
	bool		amcanorderbyop;
	/* can index storage data type differ from column data type? */
	bool		amstorage;

	/*
	 * If you add new properties to either the above or the below lists, then
	 * they should also (usually) be exposed via the property API (see
	 * InterfaceAMProperty at the top of the file, and utils/adt/amutils.c).
	 */

	/* interface functions */
	amvalidate_function amvalidate;
	amadjustmembers_function amadjustmembers;	/* can be NULL */
} InterfaceAmRoutine;

/* Functions in access/interface/ifamapi.c */
extern InterfaceAmRoutine *GetInterfaceAmRoutineByAmId(Oid amoid);

#endif							/* IFAM_H */
