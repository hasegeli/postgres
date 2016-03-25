/*-------------------------------------------------------------------------
 *
 * geo_spgist.c
 *	  SP-GiST implementation of 4-dimensional quad tree over boxes
 *
 * This module provides SP-GiST implementation for boxes using quad tree
 * analogy in 4-dimensional space.  SP-GiST doesn't allow indexing of
 * overlapping objects.  We are making 2D objects never-overlapping in
 * 4D space.  This technique has some benefits compared to traditional
 * R-Tree which is implemented as GiST.  The performance tests reveal
 * that this technique especially beneficial with too much overlapping
 * objects, so called "spaghetti data".
 *
 * Unlike the original quad tree, we are splitting the tree into 16
 * quadrants in 4D space.  It is easier to imagine it as splitting space
 * two times into 4:
 *
 *				|	   |
 *				|	   |
 *				| -----+-----
 *				|	   |
 *				|	   |
 * -------------+-------------
 *				|
 *				|
 *				|
 *				|
 *				|
 *
 * We are using box datatype as the prefix, but we are treating them
 * as points in 4-dimensional space, because 2D boxes are not not enough
 * to represent the quadrant boundaries in 4D space.  They however are
 * sufficient to point out the additional boundaries of the next
 * quadrant.
 *
 * We are using traversal values provided by SP-GiST to calculate and
 * to store the bounds of the quadrants, while traversing into the tree.
 * Traversal value has all the boundaries in the 4D space, and is is
 * capable of transferring the required boundaries to the following
 * traversal values.  In conclusion, three things are necessary
 * to calculate the next traversal value:
 *
 *	(1) the traversal value of the parent
 *	(2) the quadrant of the current node
 *	(3) the prefix of the current node
 *
 * If we visualize them on our simplified drawing (see the drawing above);
 * transfered boundaries of (1) would be the outer axis, relevant part
 * of (2) would be the up right part of the other axis, and (3) would be
 * the inner axis.
 *
 * For example, consider the case of intersection.  When recursion
 * descends deeper and deeper down the tree, all quadrants in
 * the current node will be checked for intersection.  The boundaries
 * will be re-calculated for all quadrants.  Intersection check answers
 * the question: can any box from this quadrant intersect with the given
 * box?  If yes, then this quadrant will be walked.  If no, then this
 * quadrant will be skipped.
 *
 * This method provides restrictions for minimum and maximum values of
 * every dimension of every corner of the box on every level of the tree
 * except the root.  For the root node, we are setting the boundaries
 * that we don't yet have as infinity.
 *
 * Portions Copyright (c) 1996-2016, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/geo_spgist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/spgist.h"
#include "access/stratnum.h"
#include "catalog/pg_type.h"
#include "utils/builtins.h"
#include "utils/geo_decls.h"

/*
 * Comparator for qsort
 *
 * We don't need to use the floating point macros in here, because this
 * is going only going to be used in a place to effect the performance
 * of the index, not the correctness.
 */
static int
compareDoubles(const void *a, const void *b)
{
	double		x = *(double *) a;
	double		y = *(double *) b;

	if (x == y)
		return 0;
	return (x > y) ? 1 : -1;
}

typedef struct
{
	double		low;
	double		high;
}	Range;

typedef struct
{
	Range		left;
	Range		right;
}	RangeBox;

typedef struct
{
	RangeBox	range_box_x;
	RangeBox	range_box_y;
}	RectBox;

/*
 * Fill RangeBox using BOX
 *
 * We are turning the BOX to our structures to emphasise their function
 * of representing points in 4D space.  It also is more convenient to
 * access the values with this structure.
 */
inline static void
boxPointerToRangeBox(BOX *box, RangeBox * rbox)
{
	rbox->left.low = box->low.x;
	rbox->left.high = box->high.x;

	rbox->right.low = box->low.y;
	rbox->right.high = box->high.y;
}

/*
 * Calculate the quadrant
 *
 * The quadrant is 8 bit unsigned integer with 4 least bits in use.
 * This function accepts BOXes as input.  They are not casted to
 * RangeBoxes, yet.  All 4 bits are set by comparing a corner of the box.
 * This makes 16 quadrants in total.
 */
static uint8
getQuadrant(BOX *centroid, BOX *inBox)
{
	uint8		quadrant = 0;

	if (inBox->low.x > centroid->low.x)
		quadrant |= 0x8;

	if (inBox->high.x > centroid->high.x)
		quadrant |= 0x4;

	if (inBox->low.y > centroid->low.y)
		quadrant |= 0x2;

	if (inBox->high.y > centroid->high.y)
		quadrant |= 0x1;

	return quadrant;
}

/*
 * Fill the RangeBox
 *
 * All centroids are bounded by RectBox, but SP-GiST only keeps
 * boxes.  When we are traversing the tree, we must calculate RectBox,
 * using centroid and quadrant.  This following calculates the inners
 * part of it, the RangeBox.
 */
static void
evalRangeBox(RangeBox *range_box, Range *range, int half1,
			 int half2, RangeBox *new_range_box)
{
	if (half1 == 0)
	{
		new_range_box->left.high = range->low;
		new_range_box->left.low = range_box->left.low;
	}
	else
	{
		new_range_box->left.low = range->low;
		new_range_box->left.high = range_box->left.high;
	}

	if (half2 == 0)
	{
		new_range_box->right.high = range->high;
		new_range_box->right.low = range_box->right.low;
	}
	else
	{
		new_range_box->right.low = range->high;
		new_range_box->right.high = range_box->right.high;
	}
}

/*
 * Fill the RectBox
 *
 * This functions calculates the actual RectBox using the routine above.
 */
static void
evalRectBox(RectBox *rect_box, RangeBox *centroid,
			uint8 quadrant, RectBox * new_rect_box)
{
	int	half1 = quadrant & 0x8;
	int	half2 = quadrant & 0x4;
	int	half3 = quadrant & 0x2;
	int	half4 = quadrant & 0x1;

	evalRangeBox(&rect_box->range_box_x, &centroid->left, half1, half2,
				  &new_rect_box->range_box_x);
	evalRangeBox(&rect_box->range_box_y, &centroid->right, half3, half4,
				  &new_rect_box->range_box_y);
}


/*
 * Initialize RangeBox covering all space
 *
 * In the beginning, we don't have any restrictions.  We have to
 * initialize the struct to cover the whole 4D space.
 */
static void
initializeUnboundedBox(RectBox *rect_box)
{
	rect_box->range_box_x.left.low = -get_float8_infinity();
	rect_box->range_box_x.left.high = get_float8_infinity();

	rect_box->range_box_x.right.low = -get_float8_infinity();
	rect_box->range_box_x.right.high = get_float8_infinity();

	rect_box->range_box_y.left.low = -get_float8_infinity();
	rect_box->range_box_y.left.high = get_float8_infinity();

	rect_box->range_box_y.right.low = -get_float8_infinity();
	rect_box->range_box_y.right.high = get_float8_infinity();
}

/* Can this range and any range from range_box intersect? */
static bool
intersect2D(Range *range, RangeBox *range_box)
{
	return FPge(range_box->right.high, range->low) &&
		   FPle(range_box->left.low, range->high);
}

/* Can this rectangle and any rectangle from rect_box intersect? */
static bool
intersect4D(RangeBox * rectangle, RectBox * rect_box)
{
	return intersect2D(&rectangle->left, &rect_box->range_box_x) &&
		   intersect2D(&rectangle->right, &rect_box->range_box_y);
}

/* Can any range from range_box contain this range? */
static bool
contain2D(Range *range, RangeBox *range_box)
{
	return FPge(range_box->right.high, range->high) &&
		   FPle(range_box->left.low, range->low);
}

/* Can any rectangle from rect_box contain this rectangle? */
static bool
contain4D(RangeBox *range_box, RectBox *rect_box)
{
	return contain2D(&range_box->left, &rect_box->range_box_x) &&
		   contain2D(&range_box->right, &rect_box->range_box_y);
}

/* Can this range contain any range from range_box?  */
static bool
contained2D(Range *range, RangeBox *range_box)
{
	return FPle(range_box->left.low, range->high) &&
		   FPge(range_box->left.high, range->low) &&
		   FPle(range_box->right.low, range->high) &&
		   FPge(range_box->right.high, range->low);
}

/* Can this rectangle contain any rectangle from rect_box?  */
static bool
contained4D(RangeBox *range_box, RectBox *rect_box)
{
	return contained2D(&range_box->left, &rect_box->range_box_x) &&
		   contained2D(&range_box->right, &rect_box->range_box_y);
}

/* Can any range from range_box to be lower than this range? */
static bool
isLower(Range *range, RangeBox *range_box)
{
	return FPlt(range_box->left.low, range->low) &&
		   FPlt(range_box->right.low, range->low);
}

/* Can any range from range_box to be higher than this range? */
static bool
isHigher(Range *range, RangeBox *range_box)
{
	return FPgt(range_box->left.high, range->high) &&
		   FPgt(range_box->right.high, range->high);
}

static bool
left4D(RangeBox *range_box, RectBox *rect_box)
{
	return isLower(&range_box->left, &rect_box->range_box_x);
}

static bool
right4D(RangeBox *range_box, RectBox *rect_box)
{
	return isHigher(&range_box->left, &rect_box->range_box_x);
}

static bool
below4D(RangeBox *range_box, RectBox *rect_box)
{
	return isLower(&range_box->right, &rect_box->range_box_y);
}

static bool
above4D(RangeBox *range_box, RectBox *rect_box)
{
	return isHigher(&range_box->right, &rect_box->range_box_y);
}

/*
 * SP-GiST config function
 */
Datum
spg_box_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;
	cfg->labelType = VOIDOID;	/* We don't need node labels. */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;

	PG_RETURN_VOID();
}

/*
 * SP-GiST choose function
 */
Datum
spg_box_quad_choose(PG_FUNCTION_ARGS)
{
	spgChooseIn		*in = (spgChooseIn *) PG_GETARG_POINTER(0);
	spgChooseOut	*out = (spgChooseOut *) PG_GETARG_POINTER(1);
	BOX				*inBox = DatumGetBoxP(in->datum);
	BOX				*centroid = DatumGetBoxP(in->prefixDatum);

	uint8		quadrant;

	if (in->allTheSame)
	{
		out->resultType = spgMatchNode;
		/* nodeN will be set by core */
		out->result.matchNode.levelAdd = 0;
		out->result.matchNode.restDatum = BoxPGetDatum(inBox);
		PG_RETURN_VOID();
	}

	quadrant = getQuadrant(centroid, inBox);

	out->resultType = spgMatchNode;
	out->result.matchNode.nodeN = quadrant;
	out->result.matchNode.levelAdd = 1;
	out->result.matchNode.restDatum = BoxPGetDatum(inBox);
	PG_RETURN_VOID();
}

/*
 * SP-GiST pick-split function
 *
 * It splits a list of boxes into quadrants by choosing a central 4D
 * point as the median of the coordinates of the boxes.
 */
Datum
spg_box_quad_picksplit(PG_FUNCTION_ARGS)
{
	spgPickSplitIn	*in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut	*out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid;
	int			median,
				i;
	double	   *lowXs = palloc(sizeof(double) * in->nTuples);
	double	   *highXs = palloc(sizeof(double) * in->nTuples);
	double	   *lowYs = palloc(sizeof(double) * in->nTuples);
	double	   *highYs = palloc(sizeof(double) * in->nTuples);

	/* Calculate median of all 4D coordinates */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX  *box = DatumGetBoxP(in->datums[i]);

		lowXs[i] = box->low.x;
		highXs[i] = box->high.x;
		lowYs[i] = box->low.y;
		highYs[i] = box->high.y;
	}

	qsort(lowXs, in->nTuples, sizeof(double), compareDoubles);
	qsort(highXs, in->nTuples, sizeof(double), compareDoubles);
	qsort(lowYs, in->nTuples, sizeof(double), compareDoubles);
	qsort(highYs, in->nTuples, sizeof(double), compareDoubles);

	median = in->nTuples / 2;

	centroid = palloc(sizeof(BOX));

	centroid->low.x = lowXs[median];
	centroid->high.x = highXs[median];
	centroid->low.y = lowYs[median];
	centroid->high.y = highYs[median];

	/* Fill the output */
	out->hasPrefix = true;
	out->prefixDatum = BoxPGetDatum(centroid);

	out->nNodes = 16;
	out->nodeLabels = NULL;		/* We don't need node labels. */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * Assign ranges to corresponding nodes according to quadrants
	 * relative to the "centroid" range
	 */
	for (i = 0; i < in->nTuples; i++)
	{
		BOX  *box = DatumGetBoxP(in->datums[i]);
		uint8 quadrant = getQuadrant(centroid, box);

		out->leafTupleDatums[i] = BoxPGetDatum(box);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}

/*
 * SP-GiST inner consistent function
 */
Datum
spg_box_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn   *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut  *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int						i;
	MemoryContext			oldCtx;
	RectBox				   *rect_box;
	uint8					quadrant;
	RangeBox			   *rectangle_centroid,
						   *p_query_rect,
						   *new_rect_box = NULL;

	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		out->nNodes = in->nNodes;
		for (i = 0; i < in->nNodes; i++)
			out->nodeNumbers[i] = i;

		PG_RETURN_VOID();
	}

	if (in->traversalValue)
	{
		/* Here we get 4D bounded box (RectBox) from the traversal value. */
		rect_box = in->traversalValue;
	}
	else
	{
		/*
		 * Here we initialize the bounded box, because we have just
		 * begun to walk the tree.
		 */
		rect_box = (RectBox *) palloc(sizeof(RectBox));
		initializeUnboundedBox(rect_box);
	}

	rectangle_centroid = (RangeBox *) palloc(sizeof(RangeBox));
	p_query_rect = (RangeBox *) palloc(sizeof(RangeBox));
	boxPointerToRangeBox(DatumGetBoxP(in->prefixDatum), rectangle_centroid);

	out->nNodes = 0;
	out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);

	/*
	 * We switch memory context, because we want to allocate memory for new
	 * traversal values (new_rect_box) and pass these pieces of memory to
	 * further call of this function..
	 */
	oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (quadrant = 0; quadrant < in->nNodes; quadrant++)
	{
		bool	   flag = true;

		if (new_rect_box == NULL)
			new_rect_box = (RectBox *) palloc(sizeof(RectBox));

		/* Calculate 4D RectBox */
		evalRectBox(rect_box, rectangle_centroid, quadrant, new_rect_box);

		for (i = 0; flag && i < in->nkeys; i++)
		{
			StrategyNumber strategy = in->scankeys[i].sk_strategy;

			boxPointerToRangeBox(DatumGetBoxP(in->scankeys[i].sk_argument),
								 p_query_rect);

			switch (strategy)
			{
				case RTOverlapStrategyNumber:
					flag = intersect4D(p_query_rect, new_rect_box);
					break;

				case RTContainsStrategyNumber:
					flag = contain4D(p_query_rect, new_rect_box);
					break;

				case RTContainedByStrategyNumber:
					flag = contained4D(p_query_rect, new_rect_box);
					break;

				case RTLeftStrategyNumber:
					flag = left4D(p_query_rect, new_rect_box);
					break;

				case RTRightStrategyNumber:
					flag = right4D(p_query_rect, new_rect_box);
					break;

				case RTAboveStrategyNumber:
					flag = above4D(p_query_rect, new_rect_box);
					break;

				case RTBelowStrategyNumber:
					flag = below4D(p_query_rect, new_rect_box);
					break;

				default:
					elog(ERROR, "unrecognized strategy: %d", strategy);
			}
		}

		if (flag)
		{
			out->traversalValues[out->nNodes] = new_rect_box;
			out->nodeNumbers[out->nNodes] = quadrant;
			out->nNodes++;
			new_rect_box = NULL;
		}
	}

	if (new_rect_box)
		pfree(new_rect_box);

	MemoryContextSwitchTo(oldCtx);
	PG_RETURN_VOID();
}

/*
 * SP-GiST inner consistent function
 */
Datum
spg_box_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	BOX		   *leafBox = DatumGetBoxP(in->leafDatum);
	bool		flag = true;
	int			i;

	/* All tests are exact. */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	/* Perform the required comparison(s) */
	for (i = 0; flag && i < in->nkeys; i++)
	{
		StrategyNumber strategy = in->scankeys[i].sk_strategy;
		Datum keyDatum = in->scankeys[i].sk_argument;

		switch (strategy)
		{
			case RTOverlapStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_overlap,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTContainsStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contain,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTContainedByStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_contained,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTLeftStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_left,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTRightStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_right,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTAboveStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_above,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			case RTBelowStrategyNumber:
				flag = DatumGetBool(DirectFunctionCall2(box_below,
											PointerGetDatum(leafBox),
															keyDatum));
				break;

			default:
				elog(ERROR, "unrecognized strategy: %d", strategy);
		}
	}

	PG_RETURN_BOOL(flag);
}
