/*-------------------------------------------------------------------------
 *
 * SP-GiST implementation for network address types
 *
 * The index used cidr data type on the inner nodes as the prefix.  All
 * of the inner nodes has static number of sub-nodes.  It is 2 for
 * the ones which splits different IP families, and 4 for all others.
 * 2 for the different IP families are one for version 4 and one for
 * version 6 addresses.
 *
 * 4 nodes for all others are more interesting.  The node numbers 0 and
 * 1 are for the addresses which has the same masklen as the prefix.
 * Node numbers 2 and 3 are for the addresses with bigger masklen.  That
 * makes them smaller networks.  We cannot place bigger networks under
 * the smaller ones.  Nodes number 0 and 1 are split by the next host
 * bit of the addresses.  Nodes number 2 and 3 are split by the next
 * network bit of the addresses.  The ones without any more bits are
 * naturally placed under node 0.
 *
 * This design does not all the addresses to be indexed further after
 * first host bits of them.  It is not possible to do use, because cidr
 * data type is used as the prefix.  We would need an additional number
 * to know which host bit of the address, we have split the tree.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/network_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/stratnum.h"
#include "access/spgist.h"
#include "catalog/pg_type.h"
#include "utils/inet.h"

static int inet_spg_node_number(inet *orig, int16 commonbits);
static unsigned char inet_spg_consistent_bitmap(inet *prefix, int nkeys,
												ScanKey scankeys, bool leaf);

/*
 * The SP-GiST configuration function
 */
Datum
inet_spg_config(PG_FUNCTION_ARGS)
{
	/* spgConfigIn *cfgin = (spgConfigIn *) PG_GETARG_POINTER(0); */
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = CIDROID;
	cfg->labelType = VOIDOID;
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * The SP-GiST choose function
 */
Datum
inet_spg_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	inet	   *orig = DatumGetInetPP(in->datum),
			   *prefix;
	int			commonbits;

	/*
	 * When there are addresses from the different families, we divide
	 * them purely on their families.  It can only happen on the top
	 * level node of the index without a prefix.
	 */
	if (!in->hasPrefix)
	{
		/*
		 * It is not okay to choose spgMatchNode when the tuples are
		 * "all the same".  We rely on the knowledge that the picksplit
		 * function splits the items based on their families only when
		 * there are addresses from multiple families.
		 */
		Assert(!in->allTheSame);

		out->resultType = spgMatchNode;
		out->result.matchNode.nodeN = ip_family(orig) == PGSQL_AF_INET ? 0 : 1;
		out->result.matchNode.restDatum = InetPGetDatum(orig);

		PG_RETURN_VOID();
	}

	prefix = DatumGetInetPP(in->prefixDatum);
	commonbits = ip_bits(prefix);

	/*
	 * We cannot put addresses from different families under the same
	 * inner node, so we have to split.
	 */
	if (ip_family(orig) != ip_family(prefix))
	{
		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = false;
		out->result.splitTuple.prefixNNodes = 2;
		out->result.splitTuple.prefixNodeLabels = NULL;

		out->result.splitTuple.postfixNodeN =
								ip_family(prefix) == PGSQL_AF_INET ? 0 : 1;
		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	if (in->allTheSame)
	{
		/* The node number will be set by the SP-GiST framework. */
		out->resultType = spgMatchNode;
		out->result.matchNode.restDatum = InetPGetDatum(orig);

		PG_RETURN_VOID();
	}

	/*
	 * We cannot put addresses of a bigger network under under an inner
	 * node of a smaller network, so we have to split.
	 */
	if (ip_bits(orig) < commonbits ||
		bitncmp(ip_addr(prefix), ip_addr(orig), commonbits) != 0)
	{
		commonbits = bitncommon(ip_addr(prefix), ip_addr(orig), ip_bits(orig));

		out->resultType = spgSplitTuple;
		out->result.splitTuple.prefixHasPrefix = true;
		out->result.splitTuple.prefixPrefixDatum =
					InetPGetDatum(cidr_set_masklen_internal(orig, commonbits));
		out->result.splitTuple.prefixNNodes = 4;
		out->result.splitTuple.prefixNodeLabels = NULL;

		/* We need a new node number for the existing prefix. */
		out->result.splitTuple.postfixNodeN =
									inet_spg_node_number(prefix, commonbits);
		out->result.splitTuple.postfixHasPrefix = true;
		out->result.splitTuple.postfixPrefixDatum = InetPGetDatum(prefix);

		PG_RETURN_VOID();
	}

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = inet_spg_node_number(orig, commonbits);
	out->result.matchNode.restDatum = InetPGetDatum(orig);

	PG_RETURN_VOID();
}

/*
 * The SP-GiST pick-split function
 *
 * There are two ways to split.  First one is to split by address
 * families, if there are multiple families appearing in the input.
 *
 * The second and more common way is to split by addresses.  To
 * achieve this, we determine the number of leading bits shared by all
 * the keys, then split on the next bit.  We limit those bits to
 * the minimum masklen of the input addresses, and put the keys with
 * the same netmask under the first two nodes.
 */
Datum
inet_spg_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	inet	   *prefix,
			   *tmp;
	int			i,
				commonbits;
	bool		differentFamilies = false;

	/* Initialize the prefix with the first element */
	prefix = DatumGetInetPP(in->datums[0]);
	commonbits = ip_bits(prefix);

	for (i = 1; i < in->nTuples; i++)
	{
		tmp = DatumGetInetPP(in->datums[i]);

		if (ip_family(tmp) != ip_family(prefix))
		{
			differentFamilies = true;
			break;
		}

		if (ip_bits(tmp) < commonbits)
			commonbits = ip_bits(tmp);

		if (commonbits == 0)
			break;

		/* Find minimum number of bits in common. */
		commonbits = bitncommon(ip_addr(prefix), ip_addr(tmp), commonbits);
	}

	out->nodeLabels = NULL;
	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	if (differentFamilies)
	{
		out->hasPrefix = false;
		out->nNodes = 2;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);

			out->mapTuplesToNodes[i] = ip_family(tmp) == PGSQL_AF_INET ? 0 : 1;
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}
	else
	{
		out->hasPrefix = true;
		out->prefixDatum =
				InetPGetDatum(cidr_set_masklen_internal(prefix, commonbits));
		out->nNodes = 4;

		for (i = 0; i < in->nTuples; i++)
		{
			tmp = DatumGetInetPP(in->datums[i]);

			out->mapTuplesToNodes[i] = inet_spg_node_number(tmp, commonbits);
			out->leafTupleDatums[i] = InetPGetDatum(tmp);
		}
	}

	PG_RETURN_VOID();
}

/*
 * The SP-GiST query consistency check
 */
Datum
inet_spg_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;
	unsigned char bitmap;

	if (in->allTheSame)
	{
		/*
		 * The node without prefix cannot be "all the same".  See
		 * the comment on inet_spg_choose().
		 */
		Assert(in->hasPrefix);

		/*
		 * The nodes are set by the SP-GiST framework when it is
		 * "all the same".  We are marking all of them.
		 */
		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;

		PG_RETURN_VOID();
	}

	if (!in->hasPrefix)
	{
		Assert(in->nNodes == 2);

		bitmap = 1 | 1 << 1;

		for (i = 0; i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;
			inet	   *argument = DatumGetInetPP(in->scankeys[i].sk_argument);

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET)
						bitmap &= 1;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) == PGSQL_AF_INET6)
						bitmap &= 1 << 1;
					break;

				case RTNotEqualStrategyNumber:
					break;
			}
		}
	}
	else
	{
		Assert(in->nNodes == 4);

		bitmap = inet_spg_consistent_bitmap(DatumGetInetPP(in->prefixDatum),
											in->nkeys, in->scankeys, false);
	}

	out->nNodes = 0;
	if (bitmap)
	{
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);
		for (i = 0; i < in->nNodes; i++)
		{
			if (bitmap & 1 << i)
			{
				out->nodeNumbers[out->nNodes] = i;
				out->nNodes++;
			}
		}
	}

	PG_RETURN_VOID();
}

/*
 * The SP-GiST leaf consistency check
 */
Datum
inet_spg_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	inet	   *leaf = DatumGetInetPP(in->leafDatum);

	/* All tests are exact. */
	out->recheck = false;

	/* Leaf is what it is... */
	out->leafValue = InetPGetDatum(leaf);

	PG_RETURN_BOOL(inet_spg_consistent_bitmap(leaf, in->nkeys, in->scankeys,
											  true));
}

/*
 * Calculate node number
 *
 * This function returns the node number for the given inet for any
 * inner node except the one without the prefix which splits the nodes
 * based on their families.
 */
static int
inet_spg_node_number(inet *orig, int16 commonbits)
{
	int		nodeN = 0;

	if (commonbits < ip_maxbits(orig) &&
		ip_addr(orig)[commonbits / 8] & 1 << (7 - commonbits % 8))
		nodeN += 1;
	if (commonbits < ip_bits(orig))
		nodeN += 2;

	return nodeN;
}

/*
 * Calculate bitmap of consistent nodes
 *
 * This function returns the bitmap of the selected nodes except the one
 * without the prefix which splits the nodes based on their families.
 * It works for the leaf nodes using a single bit of the bitmap.  In
 * this case the result would be 0 or 1.
 *
 * The checks for the inner and leaf nodes are mostly common which is
 * a good reason to merge them in a same function.  Using the same
 * function makes it easier to catch inconsistencies.
 */
static unsigned char
inet_spg_consistent_bitmap(inet *prefix, int nkeys, ScanKey scankeys, bool leaf)
{
	unsigned char bitmap;
	int			commonbits,
				order,
				i;

	if (leaf)
		bitmap = 1;
	else
		bitmap = 1 | 1 << 1 | 1 << 2 | 1 << 3;

	commonbits = ip_bits(prefix);

	for (i = 0; i < nkeys; i++)
	{
		inet	   *argument = DatumGetInetPP(scankeys[i].sk_argument);
		StrategyNumber strategy = scankeys[i].sk_strategy;

		/*
		 * Check 0: IP family
		 *
		 * Matching families do not help any of the strategies.
		 */
		if (ip_family(argument) != ip_family(prefix))
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (ip_family(argument) < ip_family(prefix))
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (ip_family(argument) > ip_family(prefix))
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/*
					 * For all other cases, we can be sure there is
					 * no match.
					 */
					bitmap = 0;
			}

			if (!bitmap)
				break;

			/* Other checks makes no sense with different families. */
			continue;
		}

		/*
		 * Check 1: network bit count
		 *
		 * Network bit count (ip_bits) helps to check leaves for sub
		 * network and sup network operators.  At non-leaf nodes, we
		 * know every child value has greater ip_bits, so we can avoid
		 * descending in some cases too.
		 *
		 * This check is less expensive than checking the addresses, so
		 * we are doing this before, but it has to be done after for
		 * the basic comparison strategies, because ip_bits only affect
		 * their results when the common network bits are the same.
		 */
		switch (strategy)
		{
			case RTSubStrategyNumber:
				if (commonbits <= ip_bits(argument))
					bitmap &= 1 << 2 | 1 << 3;
				break;

			case RTSubEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= 1 << 2 | 1 << 3;
				break;

			case RTSuperStrategyNumber:
				if (commonbits == ip_bits(argument) - 1)
					bitmap &= 1 | 1 << 1;
				else if (commonbits >= ip_bits(argument))
					bitmap = 0;
				break;

			case RTSuperEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | 1 << 1;
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTEqualStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= 1 << 2 | 1 << 3;
				else if (commonbits == ip_bits(argument))
					bitmap &= 1 | 1 << 1;
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;
		}

		if (!bitmap)
			break;

		/*
		 * Check 2: common network bits
		 *
		 * Compare available common prefix bits to the query, but not
		 * beyond either the query's netmask or the minimum netmask
		 * among the represented values.  If these bits don't match
		 * the query, we have our answer (and may or may not need to
		 * descend, depending on the operator).
		 */
		order = bitncmp(ip_addr(prefix), ip_addr(argument),
						Min(commonbits, ip_bits(argument)));

		if (order != 0)
		{
			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					/*
					 * For all other cases, we can be sure there is
					 * no match.
					 */
					bitmap = 0;
			}

			if (!bitmap)
				break;

			/*
			 * Remaining checks makes no sense, when the common bits
			 * don't match.
			 */
			continue;
		}

		/*
		 * Check 3: next network bit
		 *
		 * We can filter out one branch of the tree, using the next
		 * network bit of the argument, if it is available.
		 *
		 * This check matters for the performance of the search.
		 * The results would be correct without it.
		 */
		if (bitmap & (1 << 2 | 1 << 3) &&
			commonbits < ip_bits(argument))
		{
			unsigned char nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
					  1 << (7 - commonbits % 8);

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | 1 << 1 | 1 << 2;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= 1 | 1 << 1 | 1 << 3;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | 1 << 1 | 1 << 2;
					else
						bitmap &= 1 | 1 << 1 | 1 << 3;
			}

			if (!bitmap)
				break;
		}

		/*
		 * Remaining checks are only for the basic comparison
		 * strategies.  We are relying on the ordering of the strategy
		 * numbers to identify them.  They are defined on stratnum.h.
		 */
		if (strategy < RTEqualStrategyNumber ||
			strategy > RTGreaterEqualStrategyNumber)
			continue;

		/*
		 * Check 4: network bit count again
		 *
		 * At this point, we know that the common network bits of
		 * the prefix and the argument are the same, so we can go
		 * forward and check the ip_bits.
		 */
		switch (strategy)
		{
			case RTLessStrategyNumber:
			case RTLessEqualStrategyNumber:
				if (commonbits == ip_bits(argument))
					bitmap &= 1 | 1 << 1;
				else if (commonbits > ip_bits(argument))
					bitmap = 0;
				break;

			case RTGreaterEqualStrategyNumber:
			case RTGreaterStrategyNumber:
				if (commonbits < ip_bits(argument))
					bitmap &= 1 << 2 | 1 << 3;
				break;
		}

		if (!bitmap)
			break;

		/* Remaining checks don't make sense with different ip_bits. */
		if (commonbits != ip_bits(argument))
			continue;

		/*
		 * Check 5: next host bit
		 *
		 * We can filter out one branch of the tree, using the next
		 * host bit of the argument, if it is available.
		 *
		 * This check matters for the performance of the search.
		 * The results could be correct without it.  There is no point
		 * of running it for the leafs as we have to check the whole
		 * address on the next step.
		 *
		 * Furthermore by not running this check, we restrict return
		 * value to 0 and 1 for the leafs.  If we would have run this
		 * for leafs, we would need to initialise the bitmap with
		 * 1 | 1 << 1.  None of the checks before this one threat 1 and
		 * 1 << 1 separately.
		 */
		if (!leaf && bitmap & (1 | 1 << 1) &&
			commonbits < ip_maxbits(argument))
		{
			unsigned char nextbit;

			nextbit = ip_addr(argument)[commonbits / 8] &
					  1 << (7 - commonbits % 8);

			switch (strategy)
			{
				case RTLessStrategyNumber:
				case RTLessEqualStrategyNumber:
					if (!nextbit)
						bitmap &= 1 | 1 << 2 | 1 << 3;
					break;

				case RTGreaterEqualStrategyNumber:
				case RTGreaterStrategyNumber:
					if (nextbit)
						bitmap &= 1 << 1 | 1 << 2 | 1 << 3;
					break;

				case RTNotEqualStrategyNumber:
					break;

				default:
					if (!nextbit)
						bitmap &= 1 | 1 << 2 | 1 << 3;
					else
						bitmap &= 1 << 1 | 1 << 2 | 1 << 3;
			}

			if (!bitmap)
				break;
		}

		/*
		 * Check 6: whole address
		 *
		 * This is the last check for correctness of the basic
		 * comparison strategies.
		 */
		if (leaf)
		{
			order = bitncmp(ip_addr(prefix), ip_addr(argument),
							ip_maxbits(prefix));

			switch (strategy)
			{
				case RTLessStrategyNumber:
					if (order >= 0)
						bitmap = 0;
					break;

				case RTLessEqualStrategyNumber:
					if (order > 0)
						bitmap = 0;
					break;

				case RTEqualStrategyNumber:
					if (order != 0)
						bitmap = 0;
					break;

				case RTGreaterEqualStrategyNumber:
					if (order < 0)
						bitmap = 0;
					break;

				case RTGreaterStrategyNumber:
					if (order <= 0)
						bitmap = 0;
					break;

				case RTNotEqualStrategyNumber:
					if (order == 0)
						bitmap = 0;
					break;
			}

			if (!bitmap)
				break;
		}
	}

	return bitmap;
}
