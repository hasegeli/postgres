/*-------------------------------------------------------------------------
 *
 * geo_spgist.c
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
 * bounds. Let explain with two-dimensional example over points. ASCII-art:
 *			  |
 *			  |
 *	   1	  |		 2
 *			  |
 * -----------+-------------
 *			  |P
 *		3	  |		 4
 *			  |
 *			  |
 *
 * '+' with 'A' represents a centroid or, other words, point which splits plane
 * for 4 quadrants and it strorend in parent node. 1,2,3,4 are labels of
 * quadrants, each labeling will be the same for all pictures and all centriods,
 * and in following them will not be shown them in pictures later to prevent
 * too complicated images. Let we add C - child node (and again, it will split
 * plane for 4 quads):
 *
 *			  |			|
 *		  ----+---------+---
 *		X	  |B		|C
 *			  |			|
 * -----------+---------+---
 *			  |P		|A
 *			  |			|
 *			  |
 *			  |
 *
 * A and B are points of intersection of lines. So, box PBCA is a bounding box
 * for points contained in 3-rd (see labeling above). For example, X labeled
 * point is not a descendace of child node with centroid  C because it must be
 * in branch of 1-st quad of parent node. So, each node (except root) will have
 * a limitation in its quadrant. To transfer that limitation the traversalValue
 * is used.
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
 * compareDoubles is a comparator for qsort, it should not
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

/* Fill RangeBox using BOX */
inline static void
boxPointerToRangeBox(BOX *box, RangeBox * rbox)
{
	rbox->left.low = box->low.x;
	rbox->left.high = box->high.x;

	rbox->right.low = box->low.y;
	rbox->right.high = box->high.y;
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
 * All centroids in q4d tree are bounded by RectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate RectBox,
 * using centroid and quadrant. The following function calculates RangeBox.
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
 * All centroids in q4d tree are bounded by RectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate RectBox,
 * using centroid and quadrant.
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
 *initialize RangeBox covering all space
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


/*
 * answer the question: Can this range and any range from range_box intersect?
 */
static bool
intersect2D(Range *range, RangeBox *range_box)
{
	return FPge(range_box->right.high, range->low) &&
		   FPle(range_box->left.low, range->high);
}

/*
 * answer the question: Can this rectangle and any rectangle from rect_box
 * intersect?
 */
static bool
intersect4D(RangeBox * rectangle, RectBox * rect_box)
{
	return intersect2D(&rectangle->left, &rect_box->range_box_x) &&
		   intersect2D(&rectangle->right, &rect_box->range_box_y);
}


/*
 * answer the question: Can any range from range_box contain this range?
 */
static bool
contain2D(Range *range, RangeBox *range_box)
{
	return FPge(range_box->right.high, range->high) &&
		   FPle(range_box->left.low, range->low);
}


/*
 * answer the question: Can any rectangle from rect_box contain this rectangle?
 */
static bool
contain4D(RangeBox *range_box, RectBox *rect_box)
{
	return contain2D(&range_box->left, &rect_box->range_box_x) &&
		   contain2D(&range_box->right, &rect_box->range_box_y);
}


/*
 * answer the question: Can this range contain any range from range_box?
 */
static bool
contained2D(Range *range, RangeBox *range_box)
{
	return FPle(range_box->left.low, range->high) &&
		   FPge(range_box->left.high, range->low) &&
		   FPle(range_box->right.low, range->high) &&
		   FPge(range_box->right.high, range->low);
}

/*
 * answer the question: Can this rectangle contain any rectangle from rect_box?
 */
static bool
contained4D(RangeBox *range_box, RectBox *rect_box)
{
	return contained2D(&range_box->left, &rect_box->range_box_x) &&
		   contained2D(&range_box->right, &rect_box->range_box_y);
}


/*
 * answer the question: Can any range from range_box to be lower than this
 * range?
 */
static bool
isLower(Range *range, RangeBox *range_box)
{
	return FPlt(range_box->left.low, range->low) &&
		   FPlt(range_box->right.low, range->low);
}

/*
 * answer the question: Can any range from range_box to be higher than this
 * range?
 */
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
 * spg_box_quad_picksplit
 * splits a list of box into quadrants by choosing a central 4D point as
 * the median of coordinates of boxes
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

	/* calculate median for each 4D coords */
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

	/* fill output */
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
		BOX  *box = DatumGetBoxP(in->datums[i]);
		uint8 quadrant = getQuadrant(centroid, box);

		out->leafTupleDatums[i] = BoxPGetDatum(box);
		out->mapTuplesToNodes[i] = quadrant;
	}

	PG_RETURN_VOID();
}

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
	out->nodeNumbers = (int *) palloc(sizeof(int) * in->nNodes);

	if (in->allTheSame)
	{
		/* Report that all nodes should be visited */
		int			nnode;

		out->nNodes = in->nNodes;

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

	rectangle_centroid = (RangeBox *) palloc(sizeof(RangeBox));
	p_query_rect = (RangeBox *) palloc(sizeof(RangeBox));
	boxPointerToRangeBox(DatumGetBoxP(in->prefixDatum), rectangle_centroid);

	out->nNodes = 0;

	/*
	 * We switch memory context, because we want to allocate memory for new
	 * traversal values (new_rect_box) and pass these pieces of memory to
	 * further call of spg_box_quad_inner_consistent.
	 */
	oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

	for (quadrant = 0; quadrant < in->nNodes; quadrant++)
	{
		bool	   flag = true;

		if (new_rect_box == NULL)
			new_rect_box = (RectBox *) palloc(sizeof(RectBox));

		/* Calculates 4-dim RectBox */
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
					elog(ERROR, "This operation doesn't support by SP-Gist");
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
				elog(ERROR, "This type operation doesn't support by sp-gist");
		}
	}

	PG_RETURN_BOOL(flag);
}
