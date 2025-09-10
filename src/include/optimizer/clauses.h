/*-------------------------------------------------------------------------
 *
 * clauses.h
 *	  prototypes for clauses.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/optimizer/clauses.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CLAUSES_H
#define CLAUSES_H

#include "nodes/pathnodes.h"
#include "utils/hsearch.h"

/* Forward declaration */
typedef struct CSEContext CSEContext;

/* Hash table entry for tracking equivalent expressions */
typedef struct ExprHashEntry
{
	Node	   *expr;			/* Canonical expression */
	uint32		expr_hash;		/* Hash of the expression */
	List	   *equivalent_exprs;	/* List of equivalent expressions */
	int			usage_count;	/* Number of times referenced */
	bool		is_cse_candidate;	/* Worth extracting as CSE */
	PlaceHolderVar *phv;		/* PlaceHolderVar for this expression, if created */
} ExprHashEntry;

/* CSE context for expression tree walking */
struct CSEContext
{
	PlannerInfo *root;			/* Current planner context */
	HTAB	   *expr_hash_table;	/* Hash table of expressions */
	List	   *cse_targets;	/* Expressions selected for CSE */
	int			cse_threshold;	/* Min usage count for CSE */
	bool		enabled;		/* CSE enabled for this query */
	bool		analyzing;		/* True if analyzing, false if replacing */
};

typedef struct
{
	int			numWindowFuncs; /* total number of WindowFuncs found */
	Index		maxWinRef;		/* windowFuncs[] is indexed 0 .. maxWinRef */
	List	  **windowFuncs;	/* lists of WindowFuncs for each winref */
} WindowFuncLists;

extern bool contain_agg_clause(Node *clause);

extern bool contain_window_function(Node *clause);
extern WindowFuncLists *find_window_functions(Node *clause, Index maxWinRef);

extern double expression_returns_set_rows(PlannerInfo *root, Node *clause);

extern bool contain_subplans(Node *clause);

extern char max_parallel_hazard(Query *parse);
extern bool is_parallel_safe(PlannerInfo *root, Node *node);
extern bool contain_nonstrict_functions(Node *clause);
extern bool contain_exec_param(Node *clause, List *param_ids);
extern bool contain_leaked_vars(Node *clause);

extern Relids find_nonnullable_rels(Node *clause);
extern List *find_nonnullable_vars(Node *clause);
extern List *find_forced_null_vars(Node *node);
extern Var *find_forced_null_var(Node *node);

extern bool is_pseudo_constant_clause(Node *clause);
extern bool is_pseudo_constant_clause_relids(Node *clause, Relids relids);

extern int	NumRelids(PlannerInfo *root, Node *clause);

extern void CommuteOpExpr(OpExpr *clause);

extern Query *inline_set_returning_function(PlannerInfo *root,
											RangeTblEntry *rte);

extern Bitmapset *pull_paramids(Expr *expr);

/* Common Subexpression Elimination functions */
extern Node *apply_common_subexpression_elimination(PlannerInfo *root, Node *expr);
extern void analyze_common_subexpressions(PlannerInfo *root, Node *expr, CSEContext *context);
extern bool is_cse_safe_expression(Node *expr);

#endif							/* CLAUSES_H */
