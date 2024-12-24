/*-------------------------------------------------------------------------
 *
 * fkgraph.c
 *	  handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_fkgraph.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "nodes/makefuncs.h"
#include "parser/parse_node.h"
#include "parser/parsetree.h"
#include "parser/parse_relation.h"
#include "parser/parse_fkjoin.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "catalog/pg_constraint.h"
#include "parser/parse_fkgraph.h"

/*
 * Recursion context for single-pass foreign-key-join checks.
 */
typedef struct
{
	ParseState *pstate;
	Query	   *query;
	RangeTblEntry *trunk_rte;	/* must appear exactly once as the root */
	int			trunk_varno;	/* varno of trunk_rte, if we need it */
	int			location;

	int			nodeCount;		/* total RangeTblRefs encountered */
	int			edgeCount;		/* total foreign-key edges encountered */
	bool		trunkFound;		/* did we see the trunk RTE? */
	bool		trunkHasInbound;	/* trunk must not have inbound edges */
} FKCheckContext;

/*
 * Return true if varno is somewhere in the given jointree node.
 * (We only need this to decide if referencing side is 'left' or 'right'.)
 */
static bool
varno_in_subtree(Node *jtnode, Index varno)
{
	if (!jtnode)
		return false;
	if (IsA(jtnode, RangeTblRef))
		return (((RangeTblRef *) jtnode)->rtindex == varno);
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		return varno_in_subtree(j->larg, varno) ||
			varno_in_subtree(j->rarg, varno);
	}
	return false;
}

/*
 * Recursively walk a jointree node, counting RTEs, validating foreign-key edges,
 * and checking that no rows get improperly filtered.
 */
static void
fkgraph_walk(Node *jtnode, FKCheckContext *ctx)
{
	if (!jtnode)
		return;

	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, ctx->query->rtable);

		ctx->nodeCount++;

		/*
		 * Check if this is the trunk RTE. We require exactly one trunk with
		 * no inbound edges.
		 */
		if (rte == ctx->trunk_rte)
		{
			if (ctx->trunkFound)
				ereport(ERROR,
						(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
						 errmsg("trunk relation appears more than once"),
						 parser_errposition(ctx->pstate, ctx->location)));
			ctx->trunkFound = true;
			/* We'll detect inbound edges by a separate check. */
		}
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;
		ForeignKeyJoinNode *fkjn;
		RangeTblEntry *referencing_rte;
		RangeTblEntry *referenced_rte;
		bool		nonNullRefCols;

		/* Recurse left and right. */
		fkgraph_walk(join->larg, ctx);
		fkgraph_walk(join->rarg, ctx);

		/* All joins must be foreign key joins */
		if (!join->fkJoin)
			ereport(ERROR,
					(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
					 errmsg("join must be a foreign key join"),
					 parser_errposition(ctx->pstate, ctx->location)));

		fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);
		referencing_rte = rt_fetch(fkjn->referencingVarno, ctx->query->rtable);
		referenced_rte = rt_fetch(fkjn->referencedVarno, ctx->query->rtable);

		ctx->edgeCount++;

		/*
		 * If trunk is the referenced side, that means trunk has an inbound
		 * edge => not valid root.
		 */
		if (referenced_rte == ctx->trunk_rte)
			ctx->trunkHasInbound = true;

		/* Check referencing columns' nullability. */
		nonNullRefCols = check_referencing_columns_nullability(ctx->pstate,
															   referencing_rte,
															   fkjn->referencingAttnums,
															   ctx->location);

		/*
		 * If child might be missing, i.e. referencing columns are nullable, we
		 * must have an outer join that preserves such rows.
		 */
		if (!nonNullRefCols)
		{
			bool		referencingIsLeft = varno_in_subtree(join->larg, fkjn->referencingVarno);
			bool		outerJoinSafe = (join->jointype == JOIN_LEFT && referencingIsLeft) ||
				(join->jointype == JOIN_RIGHT && !referencingIsLeft);

			if (!outerJoinSafe)
				ereport(ERROR,
						(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
						 errmsg("foreign key join would filter rows"),
						 errdetail("Referencing columns are nullable or parent might be missing, "
								   "but this join is not outer-join-safe."),
						 parser_errposition(ctx->pstate, ctx->location)));
		}
	}
	else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported join tree node in foreign key join"),
				 parser_errposition(ctx->pstate, ctx->location)));
}

/*
 * fkgraph_verify
 *    Single-pass entry point.
 *    1) Walk all items in the jointree->fromlist (which may be multiple top-level items).
 *    2) Check that we found exactly one trunk with no inbound edges.
 *    3) Check edges == nodes - 1 if there's at least one node.
 */
void
fkgraph_verify(ParseState *pstate, Query *query,
			   RangeTblEntry *trunk_rte, int location)
{
	FKCheckContext ctx = {
		.pstate = pstate,
		.query = query,
		.trunk_rte = trunk_rte,
		.location = location
	};

	/* Walk the top-level FROM list. */
	if (query->jointree != NULL)
	{
		ListCell   *lc;

		foreach(lc, query->jointree->fromlist)
			fkgraph_walk((Node *) lfirst(lc), &ctx);
	}

	/*
	 * If no RTEs at all, nothing to check. But presumably trunk must appear
	 * once.
	 */
	if (ctx.nodeCount > 0 && !ctx.trunkFound)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("trunk relation not found in foreign key join"),
				 parser_errposition(pstate, location)));

	/* trunk must not have inbound edges if it is the root. */
	if (ctx.trunkFound && ctx.trunkHasInbound)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("trunk relation cannot have inbound foreign key edges"),
				 parser_errposition(pstate, location)));

	/*
	 * For an arborescence: if we have N nodes, we expect exactly N-1 edges.
	 * If nodeCount==0, there's nothing to check, so skip.
	 */
	if (ctx.nodeCount > 0 && ctx.edgeCount != ctx.nodeCount - 1)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("foreign key join must form a valid arborescence (edges != nodes - 1)"),
				 parser_errposition(pstate, location)));
}
