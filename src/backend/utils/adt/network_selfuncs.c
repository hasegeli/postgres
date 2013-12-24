/*-------------------------------------------------------------------------
 *
 * network_selfuncs.c
 *	  Functions for selectivity estimation of network operators
 *
 * Estimates are based on null fraction, distinct value count, most common
 * values, and histogram of inet, cidr datatypes.
 *
 * Portions Copyright (c) 1996-2013, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/network_selfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "catalog/pg_statistic.h"
#include "utils/lsyscache.h"
#include "utils/inet.h"
#include "utils/selfuncs.h"


static Selectivity inet_hist_overlap_selectivity(VariableStatData *vardata,
												 Datum constvalue,
												 double ndistinc);

/*
 * Selectivity estimation for network overlap operator
 */
Datum
network_overlap_selectivity(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid				operator = PG_GETARG_OID(1);
	List		   *args = (List *) PG_GETARG_POINTER(2);
	int				varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node		   *other;
	bool			varonleft;
	Selectivity		selec,
					mcv_selec,
					hist_selec;
	Datum			constvalue;
	Form_pg_statistic stats;
	FmgrInfo		proc;

	/*
	 * If expression is not (variable op something) or (something op
	 * variable), then punt and return a default estimate.
	 */
	if (!get_restriction_variable(root, args, varRelid,
								  &vardata, &other, &varonleft))
		PG_RETURN_FLOAT8(DEFAULT_NETWORK_OVERLAP_SELECTIVITY);

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_NETWORK_OVERLAP_SELECTIVITY);
	}

	/* Overlap operator is strict. */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	if (!HeapTupleIsValid(vardata.statsTuple))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_NETWORK_OVERLAP_SELECTIVITY);
	}

	constvalue = ((Const *) other)->constvalue;
	stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);

	fmgr_info(get_opcode(operator), &proc);
	selec = mcv_selectivity(&vardata, &proc, constvalue, varonleft,
							&mcv_selec);

	hist_selec = 1.0 - stats->stanullfrac - mcv_selec;

	/* If current selectivity is good enough, just correct and return it. */
	if (hist_selec / mcv_selec < selec)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(selec / (1.0 - hist_selec));
	}

	selec += hist_selec * inet_hist_overlap_selectivity(&vardata,
														constvalue,
														stats->stadistinct);

	/* Result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	ReleaseVariableStats(vardata);
	PG_RETURN_FLOAT8(selec);
}

/*
 * Selectivity estimation for network adjacent operator
 */
Datum
network_adjacent_selectivity(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid				operator = PG_GETARG_OID(1);
	List		   *args = (List *) PG_GETARG_POINTER(2);
	int				varRelid = PG_GETARG_INT32(3);
	Oid				negator;
	Selectivity		selec;

	negator = get_negator(operator);

	if (negator)
		selec = DatumGetFloat8(DirectFunctionCall4(network_overlap_selectivity,
												   PointerGetDatum(root),
												   ObjectIdGetDatum(negator),
												   PointerGetDatum(args),
												   Int32GetDatum(varRelid)));
	else
		elog(ERROR, "inet adjacent operator must have a negator");

	PG_RETURN_FLOAT8(1.0 - selec);
}

/*
 * Inet histogram overlap selectivity estimation
 *
 * Calculates histogram overlap selectivity for inet datatypes.
 * The return value is between 0 and 1. It should be corrected with MVC
 * selectivity and null fraction. The selectivity is most suitable to
 * the overlap operator. Is can be used as propotion for the sub network
 * and the sup network operators.
 *
 * Minimum bits of the constrant and elements of the histogram compared
 * with each other using the masklen. This would lead to big mistakes
 * for histograms with uneven masklen distribution. To avoid this problem
 * comparison with the left and right side of the buckets used together.
 *
 * Histogram bucket matches calculated in 3 forms. If the minimum bits
 * of the constant matches the both sides, bucket considered as fully
 * matched. If the constant matches only the right side, bucket does
 * not considered as matched at all. In that case, the ratio for only
 * 1 value in the column added to the selectivity.
 *
 * The ratio for only 1 value, calculated with the ndistinct variable,
 * if exists and greater than 0. 0 can be given to it if this behavior
 * does not desired. This ratio can be big enough not to disregard for
 * addresses with small masklen's. See pg_statistic for more information
 * about it.
 *
 * When the constant matches only the right side of the bucket, it will
 * match the next bucket, unless the bucket is the last one. If these
 * buckets would considered as matched, it would lead unfair multiple
 * matches for some constants.
 *
 * The third form is to match the bucket partially. A divider calculated
 * by using the minimum bits and the common bits for the both sides,
 * used as power of two, in this case. It is a heuristic, geometrical
 * approch. Maximum value for the mimimum bits and the common bits for
 * the both side used to mimimize the mistake in the buckets which have
 * disperate masklen's.
 *
 * For partial match with the buckets which have different address
 * families on the left and right sides, only the minimum bits and
 * the common bits for the side which has same address family with
 * the constant, used. This can cause more mistake for these buckets
 * if their masklen's are not close to the average. It is only the case
 * for one bucket, if there are addresses with different families on
 * the column. It seems as a better option than not considering these
 * buckets.
 *
 * If the constant is less than the first element or greater than
 * the last element of the histogram or if the histogram does not available,
 * the return value will be 0.
 */
static Selectivity
inet_hist_overlap_selectivity(VariableStatData *vardata,
							  Datum constvalue,
							  double ndistinct)
{
	float			match,
					divider;
	Datum		   *values;
	int				nvalues,
					i,
					left_min_bits,
					left_order,
					left_common_bits,
					right_min_bits,
					right_order,
					right_common_bits,
					family,
					bits;
	unsigned char  *addr;
	inet		   *tmp,
				   *right,
				   *left;

	if (!(HeapTupleIsValid(vardata->statsTuple) &&
		  get_attstatsslot(vardata->statsTuple,
						   vardata->atttype, vardata->atttypmod,
						   STATISTIC_KIND_HISTOGRAM, InvalidOid,
						   NULL,
						   &values, &nvalues,
						   NULL, NULL)))
		return 0.0;

	/* Initilize variables using the constant. */
	tmp = DatumGetInetP(constvalue);
	family = ip_family(tmp);
	bits = ip_bits(tmp);
	addr = ip_addr(tmp);
	match = 0.0;

	/* Iterate over the histogram buckets. Use i for the right side.*/
	for (i = 0; i < nvalues; i++)
	{
		if (i == 0)
		{
			left = NULL;
			left_min_bits = 0;
			left_order = 1; /* The first value should be greater. */
		}
		else
		{
			/* Shift the variables. */
			left = right;
			left_min_bits = right_min_bits;
			left_order = right_order;
		}

		right = DatumGetInetP(values[i]);
		if (ip_family(right) == family)
		{
			right_min_bits = Min(ip_bits(right), bits);
			if (right_min_bits == 0)
				right_order = 0;
			else
				right_order = bitncmp(ip_addr(right), addr, right_min_bits);
		}
		else if (ip_family(right) > family)
			right_order = 1;
		else
			right_order = -1;

		if (right_order == 0)
		{
			if (left_order == 0)
				/* Full bucket match. */
				match += 1.0;
			else
				/* Only right side match. */
				if (ndistinct > 0)
					match += 1.0 > ndistinct;
		}
		else if (((right_order > 0 && left_order <= 0) ||
				  (right_order < 0 && left_order >= 0)) && left)
		{
			/* Partial bucket match. */

			if (left_min_bits == 0 || ip_family(left) != family)
				left_common_bits = 0;
			else
				left_common_bits = bitncommon(ip_addr(left), addr,
											  left_min_bits);

			if (right_min_bits == 0 || ip_family(left) != family)
				right_common_bits = 0;
			else
				right_common_bits = bitncommon(ip_addr(right), addr,
											   right_min_bits);

			/* Min_bits cannot be less than common_bits in any case. */
			divider = Max(left_min_bits, right_min_bits) -
					  Max(left_common_bits, right_common_bits);

			match += 1.0 / pow(2, divider);
		}
	}

	divider = nvalues - 1;
	if (ndistinct > 0)
		/* Add this in case the constant matches the first element. */
		divider += 1.0 / ndistinct;

	elog(DEBUG1, "inet histogram overlap matches: %f / %f", match, divider);

	free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);

	Assert(match <= divider);

	return match / divider;
}

