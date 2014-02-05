/*-------------------------------------------------------------------------
 *
 * network_selfuncs.c
 *	  Functions for selectivity estimation of network operators
 *
 * Estimates are based on null fraction, distinct value count, most common
 * values, and histogram of inet, cidr datatypes.
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
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
#include "catalog/pg_operator.h"
#include "catalog/pg_statistic.h"
#include "utils/lsyscache.h"
#include "utils/inet.h"
#include "utils/selfuncs.h"


/* Default selectivity constant for the inet overlap operator */
#define DEFAULT_OVERLAP_SEL 0.01

/* Default selectivity constant for the other operators */
#define DEFAULT_INCLUSION_SEL 0.005

/* Default selectivity for given operator */
#define DEFAULT_SEL(operator) \
	((operator) == OID_INET_OVERLAP_OP ? \
			DEFAULT_OVERLAP_SEL : DEFAULT_INCLUSION_SEL)

static int inet_opr_order(Oid operator);
static Selectivity inet_hist_inclusion_selectivity(VariableStatData *vardata,
												   Datum constvalue,
												   double ndistinc,
												   int opr_order);
static int inet_inclusion_cmp(inet *left, inet *right, int opr_order);
static int inet_masklen_inclusion_cmp(inet *left, inet *right, int opr_order);
static int inet_hist_match_divider(inet *hist, inet *query, int opr_order);

/*
 * Selectivity estimation for the subnet inclusion operators
 */
Datum
inetinclusionsel(PG_FUNCTION_ARGS)
{
	PlannerInfo	   *root = (PlannerInfo *) PG_GETARG_POINTER(0);
	Oid				operator = PG_GETARG_OID(1);
	List		   *args = (List *) PG_GETARG_POINTER(2);
	int				varRelid = PG_GETARG_INT32(3);
	VariableStatData vardata;
	Node		   *other;
	bool			varonleft;
	Selectivity		selec,
					max_mcv_selec,
					mcv_selec,
					max_hist_selec,
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
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));

	/*
	 * Can't do anything useful if the something is not a constant, either.
	 */
	if (!IsA(other, Const))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	/* All of the subnet inclusion operators are strict. */
	if (((Const *) other)->constisnull)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(0.0);
	}

	if (!HeapTupleIsValid(vardata.statsTuple))
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	constvalue = ((Const *) other)->constvalue;
	stats = (Form_pg_statistic) GETSTRUCT(vardata.statsTuple);

	fmgr_info(get_opcode(operator), &proc);
	mcv_selec = mcv_selectivity(&vardata, &proc, constvalue, varonleft,
								&max_mcv_selec);

	max_hist_selec = 1.0 - stats->stanullfrac - max_mcv_selec;

	/* If current selectivity is good enough, just correct and return it. */
	if (max_hist_selec / max_mcv_selec < mcv_selec)
	{
		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(mcv_selec / (1.0 - max_hist_selec));
	}

	hist_selec = inet_hist_inclusion_selectivity(&vardata, constvalue,
					stats->stadistinct, (varonleft ? inet_opr_order(operator) :
										 inet_opr_order(operator) * -1));

	/*
	 * If histogram selectivity is not exist but MCV selectivity exists,
	 * correct and return it. If they both not exist return the default.
	 */
	if (hist_selec < 0)
	{
		if (max_mcv_selec > 0)
		{
			ReleaseVariableStats(vardata);
			PG_RETURN_FLOAT8(mcv_selec / (1.0 - max_hist_selec));
		}

		ReleaseVariableStats(vardata);
		PG_RETURN_FLOAT8(DEFAULT_SEL(operator));
	}

	selec = mcv_selec + max_hist_selec * hist_selec;

	/* Result should be in range, but make sure... */
	CLAMP_PROBABILITY(selec);

	ReleaseVariableStats(vardata);
	PG_RETURN_FLOAT8(selec);
}

/*
 * Practical comparable numbers for the subnet inclusion operators
 */
static int
inet_opr_order(Oid operator)
{
	switch (operator)
	{
		case OID_INET_CONTAINS_OP:
			return -2;
		case OID_INET_CONTAINSEQ_OP:
			return -1;
		case OID_INET_OVERLAP_OP:
			return 0;
		case OID_INET_CONTAINEDEQ_OP:
			return 1;
		case OID_INET_CONTAINED_OP:
			return 2;
	}

	elog(ERROR, "unknown operator for inet inclusion selectivity");
}

/*
 * Inet histogram inclusion selectivity estimation
 *
 * Calculates histogram selectivity for the subnet inclusion operators of
 * the inet type. In the normal case, the return value is between 0 and 1.
 * It should be corrected with the MVC selectivity and null fraction. If
 * the constant is less than the first element or greater than the last
 * element of the histogram the return value will be 0. If the histogram
 * is not available, the return value will be -1.
 *
 * The histogram is originally for the basic comparison operators. Only
 * the common bits of the network part and the lenght of the network part
 * (masklen) are appropriate for the subnet inclusion opeators. Fortunately,
 * basic comparison fits in this situation. Even so, the lenght of the
 * network part would not really be significant in the histogram. This would
 * lead to big mistakes for data sets with uneven masklen distribution.
 * To avoid this problem, comparison with the left and the right side of the
 * buckets used together.
 *
 * Histogram bucket matches are calculated in 3 forms. If the constant
 * matches both sides the bucket is considered as fully matched. If the
 * constant matches only the right side the bucket is not considered as
 * matched at all. In that case the ratio for only one value in the column
 * is added to the selectivity.
 *
 * The ratio for only one value is calculated with the ndistinct variable
 * if greater than 0. 0 can be given if this behavior is not desired.
 * This ratio can be big enough to not disregard for addresses with small
 * masklens. See pg_statistic for more information about it.
 *
 * When the constant matches only the right side of the bucket, it will match
 * the next bucket, unless the bucket is the last one. If these buckets would
 * be considered as matched it would lead to unfair multiple matches for some
 * constants.
 *
 * The third form is to match the bucket partially. We try to calculate
 * dividers for both of the boundaries. If the address family of the boundary
 * does not match the constant or comparison of the lenght of the network
 * parts is not true by the operator, the divider for the boundary would not
 * taken into account. If both of the dividers can be calculated the greater
 * one will be used to mimimize the mistake in the buckets which have
 * disperate masklens.
 *
 * The divider on the partial bucket match is imagined as the distance
 * between the decisive bits and the common bits of the addresses. It will be
 * used as power of two as it is the natural scale for the IP network
 * inclusion. The partial bucket match divider calculation is an empirical
 * formula and subject to change with more experiment.
 *
 * For partial match with buckets which have different address families
 * on the left and right sides only the boundary with the same address
 * family is taken into consideration. This can cause more mistakes for these
 * buckets if the masklens of their boundaries are also disparate. It can
 * only be the case for one bucket, if there are addresses with different
 * families on the column. It seems as a better option than not considering
 * these buckets.
 */
static Selectivity
inet_hist_inclusion_selectivity(VariableStatData *vardata, Datum constvalue,
								double ndistinct, int opr_order)
{
	Datum		   *values;
	inet		   *query,
				   *left,
				   *right;
	int				nvalues,
					left_order,
					right_order,
					i;
	double			match,
					divider,
					left_divider,
					right_divider;

	if (!(HeapTupleIsValid(vardata->statsTuple) &&
		  get_attstatsslot(vardata->statsTuple,
						   vardata->atttype, vardata->atttypmod,
						   STATISTIC_KIND_HISTOGRAM, InvalidOid,
						   NULL,
						   &values, &nvalues,
						   NULL, NULL)))
		return -1;

	query = DatumGetInetP(constvalue);
	left = NULL;
	left_order = -255; /* The first value should be greater. */
	match = 0.0;

	/* Iterate over the histogram buckets. Use i for the right side. */
	for (i = 0; i < nvalues; i++)
	{
		right = DatumGetInetP(values[i]);
		right_order = inet_inclusion_cmp(right, query, opr_order);

		if (right_order == 0)
		{
			if (left_order == 0)
				/* Full bucket match. */
				match += 1.0;
			else
				/* Only right boundry match. */
				if (ndistinct > 0)
					match += 1.0 / ndistinct;
		}
		else if (((right_order > 0 && left_order <= 0) ||
				  (right_order < 0 && left_order >= 0)) && left)
		{
			left_divider = inet_hist_match_divider(left, query, opr_order);
			right_divider = inet_hist_match_divider(right, query, opr_order);

			if (left_divider > right_divider && left_divider > 0)
				/* Partial bucket left boundry match. */
				match += 1.0 / pow(2, left_divider);
			else if (right_divider > 0)
				/* Partial bucket right boundry match. */
				match += 1.0 / pow(2, right_divider);
		}

		/* Shift the variables. */
		left = right;
		left_order = right_order;
	}

	divider = nvalues - 1;
	if (ndistinct > 0)
		/* Add this in case the constant matches the first element. */
		divider += 1.0 / ndistinct;

	elog(DEBUG1, "inet histogram inclusion matches: %f / %f", match, divider);

	free_attstatsslot(vardata->atttype, values, nvalues, NULL, 0);

	Assert(match <= divider);

	return match / divider;
}

/*
 * Comparison function for the subnet inclusion operators
 *
 * Comparison is compatible with the basic comparison function for the inet
 * type. See network_cmp_internal on network.c for the original. Basic
 * comparison operators are implemented with the network_cmp_internal
 * function. It is possible to implement the subnet inclusion operators with
 * this function.
 *
 * Comparison is first on the common bits of the network part, then on
 * the length of the network part (masklen) as the network_cmp_internal
 * function. Only the first part is on this function. The second part is
 * seperated to another function for reusability. The difference between
 * the second part and the original network_cmp_internal is that the operator
 * is used while comparing the lengths of the network parts. See the second
 * part on the inet_masklen_inclusion_cmp function below.
 */
static int
inet_inclusion_cmp(inet *left, inet *right, int opr_order)
{
	if (ip_family(left) == ip_family(right))
	{
		int		 order;

		order = bitncmp(ip_addr(left), ip_addr(right),
						Min(ip_bits(left), ip_bits(right)));

		if (order != 0)
			return order;

		return inet_masklen_inclusion_cmp(left, right, opr_order);
	}

	return ip_family(left) - ip_family(right);
}

/*
 * Masklen comparison function for the subnet inclusion operators
 *
 * Compares the lengths of network parts of the inputs using the operator.
 * If the comparision is okay for the operator the return value will be 0.
 * Otherwise the return value will be less than or greater than 0 with
 * respect to the operator.
 */
static int
inet_masklen_inclusion_cmp(inet *left, inet *right, int opr_order)
{
	if (ip_family(left) == ip_family(right))
	{
		int		 order;

		order = ip_bits(left) - ip_bits(right);

		if ((order > 0 && opr_order >= 0) ||
			(order == 0 && opr_order >= -1 && opr_order <= 1) ||
			(order < 0 && opr_order <= 0))
			return 0;

		return opr_order;
	}

	return ip_family(left) - ip_family(right);
}

/*
 * Inet histogram partial match divider calculation
 *
 * First the families and the lenghts of the network parts are compared
 * using the subnet inclusion operator. If they are not equal -1 returned is
 * which means a divider not available. Othervise the divider will be
 * calculated using the masklens and the common bits of the addresses of
 * the inputs.
 */
static int
inet_hist_match_divider(inet *hist, inet *query, int opr_order)
{
	if (inet_masklen_inclusion_cmp(hist, query, opr_order) == 0)
	{
		int		min_bits,
				decisive_bits;

		min_bits = Min(ip_bits(hist), ip_bits(query));

		/*
		 * Set the decisive bits from the one which should contain the other
		 * according to the operator.
		 */
		if (opr_order < 0)
			decisive_bits = ip_bits(hist);
		else if (opr_order > 0)
			decisive_bits = ip_bits(query);
		else
			decisive_bits = min_bits;

		if (min_bits > 0)
			return decisive_bits - bitncommon(ip_addr(hist), ip_addr(query),
											  min_bits);
		return decisive_bits;
	}

	return -1;
}
