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
 * For example consider the case of intersection.
 * When recursion descends deeper and deeper down the tree, all quadrants in
 * the current node will eventually be passed to the intersect4D function call.
 * This function answers the question: can any box from this quadrant intersect
 * with given box (query box)? If yes, then this quadrant will be walked.
 * If no, then this quadrant will be rejected.
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
#include "utils/builtins.h";
#include "utils/datum.h"
#include "utils/geo_decls.h"

#define NegInf	-1
#define PosInf	 1
#define NotInf	 0

/* InfR type implements doubles and +- infinity */
typedef struct
{
	int			infFlag;
	double		val;
}	InfR;

static InfR negInf = {NegInf, 0};
static InfR posInf = {PosInf, 0};

/* wrap double to InfR */
static InfR
toInfR(double v, InfR * r)
{
	r->infFlag = NotInf;
	r->val = v;
}

/* compare InfR with double */
static int
cmp_InfR_r(const InfR * infVal, const double val)
{
	if (infVal->infFlag == PosInf)
		return 1;
	else if (infVal->infFlag == NegInf)
		return -1;
	else
	{
		double		val0 = infVal->val;

		if (FPlt(val0, val))
			return -1;
		if (FPgt(val0, val))
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

/*-------------------------------------------------------------------------
 * We have two families of types:
 *       IRange, IRangeBox and IRectBox are parameterized with InfR,
 * while Range and Rectangle are parameterized with double
 *-------------------------------------------------------------------------
 */
typedef struct
{
	InfR		low;
	InfR		high;
}	IRange;

typedef struct
{
	IRange		left;
	IRange		right;
}	IRangeBox;

typedef struct
{
	IRangeBox	range_box_x;
	IRangeBox	range_box_y;
}	IRectBox;

typedef struct
{
	double		low;
	double		high;
}	Range;

typedef struct
{
	Range		range_x;
	Range		range_y;
}	Rectangle;


/* Fill Rectangle using BOX */
inline static void
boxPointerToRectangle(BOX *box, Rectangle * rectangle)
{
	rectangle->range_x.low = box->low.x;
	rectangle->range_x.high = box->high.x;

	rectangle->range_y.low = box->low.y;
	rectangle->range_y.high = box->high.y;
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
 * All centroids in q4d tree are bounded by IRectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate IRectBox,
 * using centroid and quadrant. The following function calculates IRangeBox.
 */
static void
evalIRangeBox(const IRangeBox *range_box, const Range *range, const int half1,
			  const int half2, IRangeBox *new_range_box)
{
	if (half1 == 0)
	{
		toInfR(range->low, &(new_range_box->left.high));
		new_range_box->left.low = range_box->left.low;
	}
	else
	{
		toInfR(range->low, &(new_range_box->left.low));
		new_range_box->left.high = range_box->left.high;
	}

	if (half2 == 0)
	{
		toInfR(range->high, &(new_range_box->right.high));
		new_range_box->right.low = range_box->right.low;
	}
	else
	{
		toInfR(range->high, &(new_range_box->right.low));
		new_range_box->right.high = range_box->right.high;
	}
}



/*
 * All centroids in q4d tree are bounded by IRectBox, but SP-Gist only keeps
 * boxes. When we walk into depth, we must calculate IRectBox,
 * using centroid and quadrant.
 */
static void
evalIRectBox(const IRectBox *rect_box, const Rectangle *centroid,
			 const uint8 quadrant, IRectBox * new_rect_box)
{
	const int	half1 = quadrant & 0x8;
	const int	half2 = quadrant & 0x4;
	const int	half3 = quadrant & 0x2;
	const int	half4 = quadrant & 0x1;

	evalIRangeBox(&rect_box->range_box_x, &centroid->range_x, half1, half2,
				  &new_rect_box->range_box_x);
	evalIRangeBox(&rect_box->range_box_y, &centroid->range_y, half3, half4,
				  &new_rect_box->range_box_y);
}


/*
 *initialize IRangeBox covering all space
 */
inline static void
initializeUnboundedBox(IRectBox * rect_box)
{
	rect_box->range_box_x.left.low = negInf;
	rect_box->range_box_x.left.high = posInf;

	rect_box->range_box_x.right.low = negInf;
	rect_box->range_box_x.right.high = posInf;

	rect_box->range_box_y.left.low = negInf;
	rect_box->range_box_y.left.high = posInf;

	rect_box->range_box_y.right.low = negInf;
	rect_box->range_box_y.right.high = posInf;
}


/*
 * answer the question: Can this range and any range from range_box intersect?
 */
static int
intersect2D(const Range * range, const IRangeBox * range_box)
{
	const InfR *x0 = &(range_box->left.low);
	const InfR *y1 = &(range_box->right.high);

	const double a = range->low;
	const double b = range->high;

	const int	p1 = cmp_InfR_r(y1, a);
	const int	p2 = cmp_InfR_r(x0, b);

	return ((p1 >= 0) && (p2 <= 0));
}

/*
 * answer the question: Can this rectangle and any rectangle from rect_box
 * intersect?
 */
static int
intersect4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	const int	px = intersect2D(&rectangle->range_x, &rect_box->range_box_x);
	const int	py = intersect2D(&rectangle->range_y, &rect_box->range_box_y);

	return (px && py);
}


/*
 * answer the question: Can any range from range_box contain this range?
 */
static int
contain2D(const Range * range, const IRangeBox * range_box)
{
	const InfR *x0 = &range_box->left.low;
	const InfR *y1 = &range_box->right.high;

	const double a = range->low;
	const double b = range->high;

	const int	p1 = cmp_InfR_r(y1, b);
	const int	p2 = cmp_InfR_r(x0, a);

	return ((p1 >= 0) && (p2 <=0));
}


/*
 * answer the question: Can any rectangle from rect_box contain this rectangle?
 */
static int
contain4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	const int	px = contain2D(&rectangle->range_x, &rect_box->range_box_x);
	const int	py = contain2D(&rectangle->range_y, &rect_box->range_box_y);

	return (px && py);
}


/*
 * answer the question: Can this range contain any range from range_box?
 */
static int
contained2D(const Range * range, const IRangeBox * range_box)
{
	const InfR *x0 = &range_box->left.low;
	const InfR *x1 = &range_box->left.high;

	const InfR *y0 = &range_box->right.low;
	const InfR *y1 = &range_box->right.high;

	const double a = range->low;
	const double b = range->high;

	const int	p1 = cmp_InfR_r(x0, b);
	const int	p2 = cmp_InfR_r(x1, a);
	const int	p3 = cmp_InfR_r(y0, b);
	const int	p4 = cmp_InfR_r(y1, a);

	return ((p1 <= 0) && (p2 >= 0) && (p3 <= 0) && (p4 >= 0));
}

/*
 * answer the question: Can this rectangle contain any rectangle from rect_box?
 */
static int
contained4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	const int	px = contained2D(&rectangle->range_x, &rect_box->range_box_x);
	const int	py = contained2D(&rectangle->range_y, &rect_box->range_box_y);

	return (px && py);
}


/*
 * answer the question: Can any range from range_box to be lower than this
 * range?
 */
static int
isLower(const Range * range, const IRangeBox * range_box)
{
	const InfR *x0 = &range_box->left.low;
	const InfR *y0 = &range_box->right.low;

	const double a = range->low;

	const int	p1 = cmp_InfR_r(x0, a);
	const int	p2 = cmp_InfR_r(y0, a);

	return (p1 < 0 && p2 < 0);
}

/*
 * answer the question: Can any range from range_box to be higher than this
 * range?
 */
static int
isHigher(const Range * range, const IRangeBox * range_box)
{
	const InfR *x1 = &range_box->left.high;
	const InfR *y1 = &range_box->right.high;

	const double b = range->high;

	const int	p1 = cmp_InfR_r(x1, b);
	const int	p2 = cmp_InfR_r(y1, b);

	return (p1 > 0 && p2 > 0);
}

static int
left4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	return isLower(&rectangle->range_x, &rect_box->range_box_x);
}

static int
right4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	return isHigher(&rectangle->range_x, &rect_box->range_box_x);
}

static int
below4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	return isLower(&rectangle->range_y, &rect_box->range_box_y);
}

static int
above4D(const Rectangle * rectangle, const IRectBox * rect_box)
{
	return isHigher(&rectangle->range_y, &rect_box->range_box_y);
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
	spgChooseOut *out = (spgChooseOut *) PG_GETARG_POINTER(1);

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


Datum
spg_box_quad_picksplit(PG_FUNCTION_ARGS)
{
	const spgPickSplitIn *in = (spgPickSplitIn *) PG_GETARG_POINTER(0);
	spgPickSplitOut *out = (spgPickSplitOut *) PG_GETARG_POINTER(1);

	BOX		   *centroid;
	int			median,
				i;


	/*
	 * Begin. This block evaluates the median of coordinates of boxes
	 */

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
	IRectBox   *rect_box;

	uint8		quadrant;

	Rectangle  *rectangle_centroid = (Rectangle *) palloc(sizeof(Rectangle));
	Rectangle  *p_query_rect = (Rectangle *) palloc(sizeof(Rectangle));

	boxPointerToRectangle(DatumGetBoxP(in->prefixDatum), rectangle_centroid);

	if (in->traversalValue)
	{
		/* Here we get 4 dimension bound box (IRectBox) from traversalValue */
		rect_box = in->traversalValue;
	}
	else
	{
		/*
		 * Here we initialize rect_box, because we have just begun to walk
		 * through the tree
		 */

		rect_box = (IRectBox *) palloc(sizeof(IRectBox));
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
		 * traversal values for IRectBox and transmit these pieces of memory
		 * to further calls of spg_box_quad_inner_consistent.
		 */
		oldCtx = MemoryContextSwitchTo(in->traversalMemoryContext);

		for (nnode = 0; nnode < in->nNodes; nnode++)
		{
			IRectBox   *new_rect_box;

			new_rect_box = (IRectBox *) palloc(sizeof(IRectBox));
			memcpy(new_rect_box, rect_box, sizeof(IRectBox));

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
		IRectBox   *new_rect_box;
		int			flag = 1;

		new_rect_box = (IRectBox *) palloc(sizeof(IRectBox));

		/* Calculates 4-dim IRectBox */
		evalIRectBox(rect_box, rectangle_centroid, quadrant, new_rect_box);

		for (i = 0; flag && i < in->nkeys && flag; i++)
		{
			const StrategyNumber strategy = in->scankeys[i].sk_strategy;

			boxPointerToRectangle(
				DatumGetBoxP(in->scankeys[i].sk_argument),
				p_query_rect
			);

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
	int			flag = 1,
				i;

	/* all tests are exact */
	out->recheck = false;

	/* leafDatum is what it is... */
	out->leafValue = in->leafDatum;

	/* Perform the required comparison(s) */
	for (i = 0; flag && i < in->nkeys && flag; i++)
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
