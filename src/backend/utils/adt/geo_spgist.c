/*-------------------------------------------------------------------------
 *
 * boxtype_spgist.c
 *	  implementation of quad-4d tree over boxes for SP-GiST.
 *
 * Quad-4d is a 4-dimensional analog of quadtree. Quad-4d tree splits
 * 4-dimensional space into 16 quadrants. Each inner node of a quad-4d tree
 * contains a box. We call these boxes centroids. Main purpose of the boxtype
 * index is to tell, for a given box, which other boxes intersect it,
 * contain or are contained by it, etc.
 *
 * For example consider the case of intersection. When recursion descends
 * deeper and deeper down the tree, all quadrants in the current node will
 * eventually be passed to the intersect4D function call. This function answers
 * the question: can any box from this quadrant intersect with given
 * box (query box)? If yes, then this quadrant will be walked. If no, then this
 * quadrant will be rejected.
 *
 * A quadrant has bounds, but sp-gist keeps only 4-d point (box) in inner nodes.
 * We use traversalValue to calculate quadrant bounds from parent's quadrant
 * bounds.
 *
 * Portions Copyright (c) 1996-2015, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *			src/backend/utils/adt/boxtype_spgist.c
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
 * compare arbitrary double value a with guaranteed non-infinity
 * double b
 */
static int
cmp_double(const double a, const double b)
{
	int r = is_infinite(a);

	Assert(is_infinite(b) == 0);

	if (r > 0)
		return 1;
	else if (r < 0)
		return -1;
	else
	{
		if (FPlt(a, b))
			return -1;
		if (FPgt(a, b))
			return 1;
	}

	return 0;
}

static int
compareDoubles(const void *a, const void *b)
{
	double		x = *(double *) a;
	double		y = *(double *) b;

	if (FPlt(x, y))
		return -1;
	else if (FPgt(x, y))
		return 1;
	else
		return 0;
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

/* Fill RangeBox using BOX */
inline static void
boxPointerToRangeBox(BOX *box, RangeBox * rectangle)
{
	rectangle->left.low = box->low.x;
	rectangle->left.high = box->high.x;

	rectangle->right.low = box->low.y;
	rectangle->right.high = box->high.y;
}

/*-----------------------------------------------------------------
 * quadrant is 8bits unsigned integer with bits:
 * [0,0,0,0,a,b,c,d] where
 * a is 1 if inBox->low.x > centroid->low.x
 * b is 1 if inBox->high.x > centroid->high.x
 * c is 1 if inBox->low.y > centroid->low.y
 * d is 1 if inBox->high.y > centroid->high.y
 *-----------------------------------------------------------------
 */
static uint8
getQuadrant(const BOX *centroid, const BOX *inBox)
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
 * All centroids in q4d tree are bounded by RectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate RectBox,
 * using centroid and quadrant. The following function calculates RangeBox.
 */
static void
evalRangeBox(const RangeBox *range_box, const Range *range, const int half1,
			  const int half2, RangeBox *new_range_box)
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
 * All centroids in q4d tree are bounded by RectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate RectBox,
 * using centroid and quadrant.
 */
static void
evalRectBox(const RectBox *rect_box, const RangeBox *centroid,
			 const uint8 quadrant, RectBox * new_rect_box)
{
	const int	half1 = quadrant & 0x8;
	const int	half2 = quadrant & 0x4;
	const int	half3 = quadrant & 0x2;
	const int	half4 = quadrant & 0x1;

	evalRangeBox(&rect_box->range_box_x, &centroid->left, half1, half2,
				  &new_rect_box->range_box_x);
	evalRangeBox(&rect_box->range_box_y, &centroid->right, half3, half4,
				  &new_rect_box->range_box_y);
}


/*
 *initialize RangeBox covering all space
 */
void
initializeUnboundedBox(RectBox * rect_box)
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


/*
 * answer the question: Can this range and any range from range_box intersect?
 */
static bool
intersect2D(const Range * range, const RangeBox * range_box)
{
	const int	p1 = cmp_double(range_box->right.high, range->low);
	const int	p2 = cmp_double(range_box->left.low, range->high);

	return (p1 >= 0) && (p2 <= 0);
}

/*
 * answer the question: Can this rectangle and any rectangle from rect_box
 * intersect?
 */
static bool
intersect4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	const int	px = intersect2D(&rectangle->left, &rect_box->range_box_x);
	const int	py = intersect2D(&rectangle->right, &rect_box->range_box_y);

	return px && py;
}


/*
 * answer the question: Can any range from range_box contain this range?
 */
static bool
contain2D(const Range * range, const RangeBox * range_box)
{
	const int	p1 = cmp_double(range_box->right.high, range->high);
	const int	p2 = cmp_double(range_box->left.low, range->low);

	return (p1 >= 0) && (p2 <=0);
}


/*
 * answer the question: Can any rectangle from rect_box contain this rectangle?
 */
static bool
contain4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	const int	px = contain2D(&rectangle->left, &rect_box->range_box_x);
	const int	py = contain2D(&rectangle->right, &rect_box->range_box_y);

	return px && py;
}


/*
 * answer the question: Can this range contain any range from range_box?
 */
static bool
contained2D(const Range * range, const RangeBox * range_box)
{
	const int	p1 = cmp_double(range_box->left.low, range->high);
	const int	p2 = cmp_double(range_box->left.high, range->low);
	const int	p3 = cmp_double(range_box->right.low, range->high);
	const int	p4 = cmp_double(range_box->right.high, range->low);

	return (p1 <= 0) && (p2 >= 0) && (p3 <= 0) && (p4 >= 0);
}

/*
 * answer the question: Can this rectangle contain any rectangle from rect_box?
 */
static bool
contained4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	const int	px = contained2D(&rectangle->left, &rect_box->range_box_x);
	const int	py = contained2D(&rectangle->right, &rect_box->range_box_y);

	return (px && py);
}


/*
 * answer the question: Can any range from range_box to be lower than this
 * range?
 */
static bool
isLower(const Range * range, const RangeBox * range_box)
{
	const int	p1 = cmp_double(range_box->left.low, range->low);
	const int	p2 = cmp_double(range_box->right.low, range->low);

	return (p1 < 0) && (p2 < 0);
}

/*
 * answer the question: Can any range from range_box to be higher than this
 * range?
 */
static bool
isHigher(const Range * range, const RangeBox * range_box)
{
	const int	p1 = cmp_double(range_box->left.high, range->high);
	const int	p2 = cmp_double(range_box->right.high, range->high);

	return (p1 > 0) && (p2 > 0);
}

static bool
left4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	return isLower(&rectangle->left, &rect_box->range_box_x);
}

static bool
right4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	return isHigher(&rectangle->left, &rect_box->range_box_x);
}

static bool
below4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	return isLower(&rectangle->right, &rect_box->range_box_y);
}

static bool
above4D(const RangeBox * rectangle, const RectBox * rect_box)
{
	return isHigher(&rectangle->right, &rect_box->range_box_y);
}

/*
 * SP-GiST 'config' interface function.
 */
Datum
spg_box_quad_config(PG_FUNCTION_ARGS)
{
	spgConfigOut *cfg = (spgConfigOut *) PG_GETARG_POINTER(1);

	cfg->prefixType = BOXOID;
	cfg->labelType = VOIDOID;	/* we don't need node labels */
	cfg->canReturnData = true;
	cfg->longValuesOK = false;
	PG_RETURN_VOID();
}


Datum
spg_box_quad_choose(PG_FUNCTION_ARGS)
{
	const spgChooseIn *in = (spgChooseIn *) PG_GETARG_POINTER(0);
	const spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);
	const BOX  *inBox = DatumGetBoxP(in->datum);
	const BOX  *centroid = DatumGetBoxP(in->prefixDatum);

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
 * spg_box_quad_picksplit
 * splits a list of box into quadrants by choosing a central 4D point as
 * the median of coordinates of boxes
 */
Datum
spg_box_quad_picksplit(PG_FUNCTION_ARGS)
{
	const spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	const spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);
	BOX		   *centroid;
	int			median,
				i;
	double	   *lowXs = palloc(sizeof(double) * in->nTuples);
	double	   *highXs = palloc(sizeof(double) * in->nTuples);
	double	   *lowYs = palloc(sizeof(double) * in->nTuples);
	double	   *highYs = palloc(sizeof(double) * in->nTuples);

	for (i = 0; i < in->nTuples; i++)
	{
		const BOX  *box = DatumGetBoxP(in->datums[i]);

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

	/*
	 * This block evaluates the median of coordinates of boxes. End.
	 */

	out->hasPrefix = true;
	out->prefixDatum = BoxPGetDatum(centroid);

	out->nNodes = 16;
	out->nodeLabels = NULL;		/* we don't need node labels */

	out->mapTuplesToNodes = palloc(sizeof(int) * in->nTuples);
	out->leafTupleDatums = palloc(sizeof(Datum) * in->nTuples);

	/*
	 * Assign ranges to corresponding nodes according to quadrants relative to
	 * "centroid" range.
	 */

	for (i = 0; i < in->nTuples; i++)
	{
		const BOX  *box = DatumGetBoxP(in->datums[i]);
		const uint8 quadrant = getQuadrant(centroid, box);

		out->leafTupleDatums[i] = BoxPGetDatum(box);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}

Datum
spg_box_quad_inner_consistent(PG_FUNCTION_ARGS)
{
	spgInnerConsistentIn *in = (spgInnerConsistentIn *) PG_GETARG_POINTER(0);
	spgInnerConsistentOut *out = (spgInnerConsistentOut *) PG_GETARG_POINTER(1);
	int			i;

	MemoryContext oldCtx;
	RectBox   *rect_box;

	uint8		quadrant;

	RangeBox  *rectangle_centroid = (RangeBox *) palloc(sizeof(RangeBox));
	RangeBox  *p_query_rect = (RangeBox *) palloc(sizeof(RangeBox));

	boxPointerToRangeBox(DatumGetBoxP(in->prefixDatum), rectangle_centroid);

	if (in->traversalValue)
	{
		/* Here we get 4 dimension bound box (RectBox) from traversalValue */
		rect_box = in->traversalValue;
	}
	else
	{
		/*
		 * Here we initialize rect_box, because we have just begun to walk
		 * through the tree
		 */

		rect_box = (RectBox *) palloc(sizeof(RectBox));
		initializeUnboundedBox(rect_box);
	}

	out->traversalValues = (void **) palloc(sizeof(void *) * in->nNodes);

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		int			nnode;

		out->nNodes = in->nNodes;
		out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

		/*
		 * We switch memory context, because we want allocate memory for new
		 * traversal values for RectBox and transmit these pieces of memory
		 * to further calls of spg_box_quad_inner_consistent.
		 */
		oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

		for (nnode = 0; nnode < in->nNodes; nnode++)
		{
			RectBox   *new_rect_box;

			new_rect_box = (RectBox *) palloc(sizeof(RectBox));
			memcpy(new_rect_box, rect_box, sizeof(RectBox));

			out->traversalValues[nnode] = new_rect_box;
			out->nodeNumbers[nnode] = nnode;
		}
		/* Switch back */
		MemoryContextSwitchTo(oldCtx);
		PG_RETURN_VOID();
	}

	out->nNodes = 0;
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

	/*
	 * We switch memory context, because we want to allocate memory for new
	 * traversal values (new_rect_box) and pass these pieces of memory to
	 * further call of spg_box_quad_inner_consistent.
	 */
	oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (quadrant = 0; quadrant < in->nNodes; quadrant++)
	{
		RectBox   *new_rect_box;
		bool	   flag = true;

		new_rect_box = (RectBox *) palloc(sizeof(RectBox));

		/* Calculates 4-dim RectBox */
		evalRectBox(rect_box, rectangle_centroid, quadrant, new_rect_box);

		for (i = 0; flag && i < in->nkeys; i++)
		{
			const StrategyNumber strategy = in->scankeys[i].sk_strategy;

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
					elog(ERROR, "This operation doesn't support by SP-Gist");
			}
		}

		if (flag)
		{
			out->traversalValues[out->nNodes] = new_rect_box;
			out->nodeNumbers[out->nNodes] = quadrant;
			out->nNodes++;
		}
	}

	MemoryContextSwitchTo(oldCtx);
	PG_RETURN_VOID();
}

Datum
spg_box_quad_leaf_consistent(PG_FUNCTION_ARGS)
{
	spgLeafConsistentIn *in = (spgLeafConsistentIn *) PG_GETARG_POINTER(0);
	spgLeafConsistentOut *out = (spgLeafConsistentOut *) PG_GETARG_POINTER(1);
	BOX		   *leafBox = DatumGetBoxP(in->leafDatum);
	bool		flag = true;
	int			i;

	/* all tests are exact */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	/* Perform the required comparison(s) */
	for (i = 0; flag && i < in->nkeys; i++)
	{
		const StrategyNumber strategy = in->scankeys[i].sk_strategy;
		const Datum keyDatum = in->scankeys[i].sk_argument;

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
				elog(ERROR, "This type operation doesn't support by sp-gist");
		}
	}

	PG_RETURN_BOOL(flag);
}
