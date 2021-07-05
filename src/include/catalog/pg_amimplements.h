/*-------------------------------------------------------------------------
 *
 * pg_amimplements.h
 *	  definition of the "implements" system catalog (pg_amimplements)
 *
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_amimplements.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_AMIMPLEMENTS_H
#define PG_AMIMPLEMENTS_H

#include "catalog/genbki.h"
#include "catalog/pg_amimplements_d.h"

#include "nodes/pg_list.h"
#include "storage/lock.h"

/* ----------------
 *		pg_amimplements definition.  cpp turns this into
 *		typedef struct FormData_pg_amimplements
 * ----------------
 */
CATALOG(pg_amimplements,4544,AmimplementsRelationId)
{
	Oid			amiamid BKI_LOOKUP(pg_am);
	Oid			amiparent BKI_LOOKUP(pg_am);
	int32		amiseqno;
} FormData_pg_amimplements;

/* ----------------
 *		Form_pg_amimplements corresponds to a pointer to a tuple with
 *		the format of pg_amimplements relation.
 * ----------------
 */
typedef FormData_pg_amimplements *Form_pg_amimplements;

DECLARE_UNIQUE_INDEX_PKEY(pg_amimplements_amid_seqno_index, 4545, AmimplementsAmidSeqnoIndexId, on pg_amimplements using btree(amiamid oid_ops, amiseqno int4_ops));

#endif							/* PG_AMIMPLEMENTS_H */
