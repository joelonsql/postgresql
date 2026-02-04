/*-------------------------------------------------------------------------
 *
 * parse_fkjoin.c
 *	  handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_fkjoin.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_constraint.h"
#include "nodes/bitmapset.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_fkjoin.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * FKJoinFuncDep - Represents a functional dependency between base tables
 *
 * This is used during FK join validation to track which base table indices
 * are functionally dependent on others.
 */
typedef struct FKJoinFuncDep
{
	NodeTag		type;
	int			determinant;	/* base table index of determinant */
	int			dependent;		/* base table index of dependent */
} FKJoinFuncDep;

/*
 * RTEBaseIndexEntry - Hash table entry mapping RTE pointer to base index
 *
 * The RTE pointer is used as the hash key because the same RTE object
 * is reached whether we traverse via jointree enumeration or drill-down.
 */
typedef struct RTEBaseIndexEntry
{
	RangeTblEntry *rte;			/* hash key */
	int			base_idx;		/* assigned base table index */
} RTEBaseIndexEntry;

/*
 * FKJoinValidationContext - Context for FK join validation passes
 *
 * This structure is used during FK join validation that runs after
 * view expansion (fireRIRrules).
 */
typedef struct FKJoinValidationContext
{
	int			next_base_index;		/* Counter for base table enumeration */
	Bitmapset  *uniqueness_preservation;/* Set of base indices preserving uniqueness */
	List	   *functional_dependencies;/* List of FKJoinFuncDep */
	Query	   *query;					/* The current query being validated */
	List	   *query_stack;			/* Stack of ancestor queries (for CTE lookup) */
	HTAB	   *rte_to_base_index;		/* RTE* -> base index mapping */
} FKJoinValidationContext;

/* Forward declarations for static functions */
static Node *build_fk_join_on_clause(ParseState *pstate,
									 ParseNamespaceColumn *l_nscols, List *l_attnums,
									 ParseNamespaceColumn *r_nscols, List *r_attnums);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid,
							 List *referencing_attnums, List *referenced_attnums);
static char *column_list_to_string(const List *columns);
static char *attnum_list_to_string(Oid relid, List *attnums);
static bool subquery_has_cte_ref(RangeTblEntry *rte);
static RangeTblEntry *drill_down_to_base_rel(List *query_stack, RangeTblEntry *rte,
											 List *attnos, List **base_attnums);
static RangeTblEntry *drill_down_to_base_rel_query(List *query_stack, Query *subquery,
												   List *attnos, List **base_attnums);
static bool check_unique_index_covers_columns(Relation rel, Bitmapset *columns);
static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums,
										 List **notNullConstraints);

/* Validation pass functions */
static void fkjoin_enumerate_and_compute(Node *jtnode, FKJoinValidationContext *context,
										 Bitmapset **uniqueness_out,
										 List **funcdeps_out);

/* Helper functions for functional dependencies */
static FKJoinFuncDep *makeFKJoinFuncDep(int determinant, int dependent);
static bool has_self_dependency(List *funcdeps, int base_idx);
static Bitmapset *update_uniqueness_preservation(Bitmapset *referencing_up,
												 Bitmapset *referenced_up,
												 int referencing_base_idx,
												 bool fk_cols_unique);
static List *update_functional_dependencies(List *referencing_fds,
											int referencing_base_idx,
											List *referenced_fds,
											int referenced_base_idx,
											bool fk_cols_not_null,
											JoinType join_type,
											ForeignKeyDirection fk_dir);

/*
 * transformAndValidateForeignKeyJoin
 *		Transform a foreign key join clause during parsing.
 *
 * This function handles the parsing phase of FK joins:
 * - Validates that the FK constraint exists in pg_constraint
 * - Builds the ON clause (equality conditions)
 * - Creates ForeignKeyJoinNode with constraint OID
 *
 * The actual validation of uniqueness preservation and functional dependencies
 * is deferred to validateForeignKeyJoins(), which runs after view expansion.
 */
void
transformAndValidateForeignKeyJoin(ParseState *pstate, JoinExpr *join,
								   ParseNamespaceItem *r_nsitem,
								   List *l_namespace)
{
	ForeignKeyClause *fkjn = castNode(ForeignKeyClause, join->fkJoin);
	ListCell   *lc;
	RangeTblEntry *referencing_rte,
			   *referenced_rte;
	RangeTblEntry *base_referencing_rte;
	RangeTblEntry *base_referenced_rte;
	ParseNamespaceItem *referencing_rel,
			   *referenced_rel,
			   *other_rel = NULL;
	List	   *referencing_cols,
			   *referenced_cols;
	Node	   *referencing_arg;

	List	   *referencing_base_attnums;
	List	   *referenced_base_attnums;
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;
	Oid			referencing_relid = InvalidOid;
	Oid			referenced_relid = InvalidOid;
	List	   *notNullConstraints = NIL;

	/* Find the referenced relation in the left namespace */
	foreach(lc, l_namespace)
	{
		ParseNamespaceItem *nsi = (ParseNamespaceItem *) lfirst(lc);

		if (!nsi->p_rel_visible)
			continue;

		if (strcmp(nsi->p_names->aliasname, fkjn->refAlias) == 0)
		{
			other_rel = nsi;
			break;
		}
	}

	if (other_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table reference \"%s\" not found", fkjn->refAlias),
				 parser_errposition(pstate, fkjn->location)));

	if (list_length(fkjn->refCols) != list_length(fkjn->localCols))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foriegn key disagree"),
				 parser_errposition(pstate, fkjn->location)));

	/* Determine which side is referencing and which is referenced */
	if (fkjn->fkdir == FKDIR_FROM)
	{
		referencing_rel = other_rel;
		referenced_rel = r_nsitem;
		referencing_cols = fkjn->refCols;
		referenced_cols = fkjn->localCols;
		referencing_arg = join->larg;
	}
	else
	{
		referenced_rel = other_rel;
		referencing_rel = r_nsitem;
		referenced_cols = fkjn->refCols;
		referencing_cols = fkjn->localCols;
		referencing_arg = join->rarg;
	}

	referencing_rte = rt_fetch(referencing_rel->p_rtindex, pstate->p_rtable);
	referenced_rte = rt_fetch(referenced_rel->p_rtindex, pstate->p_rtable);

	/* Resolve column names to attribute numbers for referencing side */
	foreach(lc, referencing_cols)
	{
		char	   *ref_colname = strVal(lfirst(lc));
		List	   *colnames = referencing_rel->p_names->colnames;
		ListCell   *col;
		int			ndx = 0,
					col_index = -1;

		foreach(col, colnames)
		{
			char	   *colname = strVal(lfirst(col));

			if (strcmp(colname, ref_colname) == 0)
			{
				if (col_index >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("common column name \"%s\" appears more than once in referencing table",
									ref_colname),
							 parser_errposition(pstate, fkjn->location)));
				col_index = ndx;
			}
			ndx++;
		}
		if (col_index < 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in referencing table",
							ref_colname),
					 parser_errposition(pstate, fkjn->location)));
		referencing_attnums = lappend_int(referencing_attnums, col_index + 1);
	}

	/* Resolve column names to attribute numbers for referenced side */
	foreach(lc, referenced_cols)
	{
		char	   *ref_colname = strVal(lfirst(lc));
		List	   *colnames = referenced_rel->p_names->colnames;
		ListCell   *col;
		int			ndx = 0,
					col_index = -1;

		foreach(col, colnames)
		{
			char	   *colname = strVal(lfirst(col));

			if (strcmp(colname, ref_colname) == 0)
			{
				if (col_index >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("common column name \"%s\" appears more than once in referenced table",
									ref_colname),
							 parser_errposition(pstate, fkjn->location)));
				col_index = ndx;
			}
			ndx++;
		}
		if (col_index < 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in referenced table",
							ref_colname),
					 parser_errposition(pstate, fkjn->location)));
		referenced_attnums = lappend_int(referenced_attnums, col_index + 1);
	}

	/*
	 * Check if either relation is a CTE or JOIN.  These can't be validated
	 * during parse because we don't have the full query context to resolve
	 * them.  The FK constraint will be validated during the rewrite phase
	 * (after fireRIRrules).
	 */
	if (referencing_rte->rtekind == RTE_CTE || referenced_rte->rtekind == RTE_CTE ||
		referencing_rte->rtekind == RTE_JOIN || referenced_rte->rtekind == RTE_JOIN ||
		subquery_has_cte_ref(referencing_rte) || subquery_has_cte_ref(referenced_rte))
	{
		/*
		 * Skip FK constraint lookup for CTEs/JOINs during parse.
		 * Also skip when a subquery contains CTE references, since
		 * drill_down cannot resolve CTEs without the full query context.
		 * Still set fkoid to InvalidOid so it gets validated later.
		 */
		fkoid = InvalidOid;
	}
	else
	{
		/* Drill down to find the base relations */
		base_referencing_rte = drill_down_to_base_rel(NULL, referencing_rte,
													  referencing_attnums,
													  &referencing_base_attnums);
		base_referenced_rte = drill_down_to_base_rel(NULL, referenced_rte,
													 referenced_attnums,
													 &referenced_base_attnums);

		referencing_relid = base_referencing_rte->relid;
		referenced_relid = base_referenced_rte->relid;

		Assert(referencing_relid != InvalidOid && referenced_relid != InvalidOid);

		/* Verify the FK constraint exists */
		fkoid = find_foreign_key(referencing_relid, referenced_relid,
								 referencing_base_attnums, referenced_base_attnums);

		if (fkoid == InvalidOid)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
							referencing_rte->alias ? referencing_rte->alias->aliasname :
							(referencing_rte->relid == InvalidOid) ? "<unnamed derived table>" :
							get_rel_name(referencing_rte->relid),
							column_list_to_string(referencing_cols),
							referenced_rte->alias ? referenced_rte->alias->aliasname :
							(referenced_rte->relid == InvalidOid) ? "<unnamed derived table>" :
							get_rel_name(referenced_rte->relid),
							column_list_to_string(referenced_cols)),
					 parser_errposition(pstate, fkjn->location)));
	}

	/* Build the ON clause for the join */
	join->quals = build_fk_join_on_clause(pstate,
										  referencing_rel->p_nscolumns, referencing_attnums,
										  referenced_rel->p_nscolumns, referenced_attnums);

	/*
	 * Collect NOT NULL constraint dependencies when the referencing table
	 * is on the "inner" side of a join:
	 * - INNER JOIN: both sides can be filtered, so always need NOT NULL
	 * - LEFT JOIN: right side is inner, track if referencing is on right
	 * - RIGHT JOIN: left side is inner, track if referencing is on left
	 * - FULL JOIN: both sides preserved, no NOT NULL dependency needed
	 */
	{
		bool		need_not_null_deps = false;

		switch (join->jointype)
		{
			case JOIN_INNER:
				need_not_null_deps = true;
				break;
			case JOIN_LEFT:
				need_not_null_deps = (referencing_arg == join->rarg);
				break;
			case JOIN_RIGHT:
				need_not_null_deps = (referencing_arg == join->larg);
				break;
			case JOIN_FULL:
				break;
			default:
				break;
		}

		if (need_not_null_deps && referencing_relid != InvalidOid)
		{
			(void) is_referencing_cols_not_null(referencing_relid,
												referencing_base_attnums,
												&notNullConstraints);
		}
	}

	/* Create the ForeignKeyJoinNode */
	fkjn_node = makeNode(ForeignKeyJoinNode);
	fkjn_node->fkdir = fkjn->fkdir;
	fkjn_node->referencingVarno = referencing_rel->p_rtindex;
	fkjn_node->referencingAttnums = referencing_attnums;
	fkjn_node->referencedVarno = referenced_rel->p_rtindex;
	fkjn_node->referencedAttnums = referenced_attnums;
	fkjn_node->constraint = fkoid;
	fkjn_node->notNullConstraints = notNullConstraints;

	join->fkJoin = (Node *) fkjn_node;
}

/*
 * validateForeignKeyJoins
 *		Validate FK joins after view expansion (fireRIRrules).
 *
 * This function enumerates base tables and computes uniqueness_preservation
 * and functional_dependencies for each node in the join tree. FK join
 * constraints are validated during enumeration when we have access to
 * child properties and can correlate them via the RTE->base_idx hash table.
 */
void
validateForeignKeyJoins(Query *query)
{
	FKJoinValidationContext context;
	Bitmapset  *uniqueness = NULL;
	List	   *funcdeps = NIL;
	HASHCTL		hash_ctl;

	/* Only validate SELECT queries with a join tree */
	if (query->commandType != CMD_SELECT)
		return;

	if (query->jointree == NULL || query->jointree->fromlist == NIL)
		return;

	/* Initialize validation context */
	context.next_base_index = 0;
	context.uniqueness_preservation = NULL;
	context.functional_dependencies = NIL;
	context.query = query;
	context.query_stack = list_make1(query);	/* Start with top-level query */

	/* Create hash table for RTE -> base index mapping */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = sizeof(RangeTblEntry *);
	hash_ctl.entrysize = sizeof(RTEBaseIndexEntry);
	hash_ctl.hcxt = CurrentMemoryContext;
	context.rte_to_base_index = hash_create("FK Join RTE to Base Index",
											32,
											&hash_ctl,
											HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

	/*
	 * Bottom-up enumeration, property computation, and validation.
	 * Process the from-list to compute uniqueness_preservation and
	 * functional_dependencies. FK joins are validated during this pass.
	 */
	if (list_length(query->jointree->fromlist) == 1)
	{
		fkjoin_enumerate_and_compute(linitial(query->jointree->fromlist),
									 &context, &uniqueness, &funcdeps);
	}
	else
	{
		/* Multiple from-list items - process each */
		ListCell   *lc;

		foreach(lc, query->jointree->fromlist)
		{
			Bitmapset  *item_uniqueness = NULL;
			List	   *item_funcdeps = NIL;

			fkjoin_enumerate_and_compute(lfirst(lc), &context,
										 &item_uniqueness, &item_funcdeps);

			/* Merge results - cross-product doesn't preserve uniqueness */
			uniqueness = bms_union(uniqueness, item_uniqueness);
			funcdeps = list_concat(funcdeps, item_funcdeps);
		}
	}

	/* Clean up hash table */
	hash_destroy(context.rte_to_base_index);
}

/*
 * jtnode_has_fk_joins
 *		Check if a join tree node contains any FK join nodes.
 */
static bool
jtnode_has_fk_joins(Node *jtnode)
{
	if (jtnode == NULL)
		return false;

	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		if (j->fkJoin != NULL)
			return true;
		if (jtnode_has_fk_joins(j->larg))
			return true;
		if (jtnode_has_fk_joins(j->rarg))
			return true;
	}
	else if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (jtnode_has_fk_joins(lfirst(lc)))
				return true;
		}
	}
	else if (IsA(jtnode, RangeTblRef))
	{
		/* Leaf node, no FK joins here */
	}

	return false;
}

/*
 * queryHasFKJoins
 *		Check if a query contains any FK join nodes in its join tree.
 *
 * This is used to avoid expensive view revalidation (fireRIRrules +
 * validateForeignKeyJoins) for views that don't use FK joins.
 */
bool
queryHasFKJoins(Query *query)
{
	if (query == NULL || query->jointree == NULL)
		return false;

	return jtnode_has_fk_joins((Node *) query->jointree);
}

/*
 * fkjoin_enumerate_and_compute
 *		Bottom-up pass: enumerate base tables and compute properties.
 *
 * For each node in the join tree:
 * - RangeTblRef: If it's a base table, assign a local index and set initial
 *   uniqueness preservation and functional dependencies.
 * - JoinExpr: Recursively process children, then merge properties based on
 *   join type and FK constraints.
 */
static void
fkjoin_enumerate_and_compute(Node *jtnode, FKJoinValidationContext *context,
							 Bitmapset **uniqueness_out,
							 List **funcdeps_out)
{
	*uniqueness_out = NULL;
	*funcdeps_out = NIL;

	if (jtnode == NULL)
		return;

	switch (nodeTag(jtnode))
	{
		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) jtnode;
				RangeTblEntry *rte = rt_fetch(rtr->rtindex, context->query->rtable);

				if (rte->rtekind == RTE_RELATION)
				{
					Relation	rel = table_open(rte->relid, AccessShareLock);

					if (rel->rd_rel->relkind == RELKIND_RELATION ||
						rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
					{
						/* Base table: assign index and set initial properties */
						int			base_idx = context->next_base_index++;
						RTEBaseIndexEntry *entry;
						bool		found;

						/* Store mapping from RTE pointer to base index */
						entry = hash_search(context->rte_to_base_index,
											&rte,
											HASH_ENTER,
											&found);
						entry->rte = rte;
						entry->base_idx = base_idx;

						*uniqueness_out = bms_add_member(*uniqueness_out, base_idx);
						*funcdeps_out = list_make1(makeFKJoinFuncDep(base_idx, base_idx));
					}
					else if (rel->rd_rel->relkind == RELKIND_VIEW)
					{
						/*
						 * View: views should have been expanded by fireRIRrules.
						 * If we still see a view here, it's an internal error.
						 */
						elog(ERROR, "unexpected view in FK join validation after fireRIRrules");
					}

					table_close(rel, AccessShareLock);
				}
				else if (rte->rtekind == RTE_SUBQUERY)
				{
					/*
					 * Subquery: recursively validate and propagate properties.
					 * For now, we conservatively don't preserve uniqueness
					 * through subqueries unless they're simple enough.
					 */
					Query	   *subquery = rte->subquery;

					if (subquery->jointree &&
						subquery->jointree->fromlist &&
						list_length(subquery->jointree->fromlist) == 1 &&
						!subquery->distinctClause &&
						!subquery->groupClause &&
						!subquery->groupingSets &&
						!subquery->hasTargetSRFs)
					{
						FKJoinValidationContext subcontext;

						subcontext.next_base_index = context->next_base_index;
						subcontext.uniqueness_preservation = NULL;
						subcontext.functional_dependencies = NIL;
						subcontext.query = subquery;
						/* Push subquery onto front of stack for CTE lookups */
						subcontext.query_stack = lcons(subquery, list_copy(context->query_stack));
						/* Share the hash table with parent context */
						subcontext.rte_to_base_index = context->rte_to_base_index;

						fkjoin_enumerate_and_compute(linitial(subquery->jointree->fromlist),
													 &subcontext,
													 uniqueness_out,
													 funcdeps_out);

						context->next_base_index = subcontext.next_base_index;

						/* If subquery has WHERE clause, remove self-dependencies */
						if (subquery->jointree->quals != NULL ||
							subquery->limitOffset != NULL ||
							subquery->limitCount != NULL ||
							subquery->havingQual != NULL)
						{
							ListCell   *lc;
							List	   *filtered_fds = NIL;

							foreach(lc, *funcdeps_out)
							{
								FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc);

								if (fd->determinant != fd->dependent)
									filtered_fds = lappend(filtered_fds, fd);
							}
							*funcdeps_out = filtered_fds;
						}
					}
					else if (subquery->groupClause &&
							 !subquery->distinctClause &&
							 !subquery->groupingSets &&
							 !subquery->hasTargetSRFs &&
							 subquery->jointree &&
							 subquery->jointree->fromlist &&
							 list_length(subquery->jointree->fromlist) == 1)
					{
						/*
						 * GROUP BY case: First enumerate the subquery contents
						 * to populate the hash table, then check if GROUP BY
						 * columns form a unique key AND the base table rows
						 * are preserved before the GROUP BY.
						 */
						FKJoinValidationContext subcontext;
						Bitmapset  *sub_uniqueness = NULL;
						List	   *sub_funcdeps = NIL;
						Bitmapset  *group_cols = NULL;
						Index		group_varno = 0;
						ListCell   *lc;
						bool		all_simple = true;

						/* First, enumerate subquery contents to populate hash table */
						subcontext.next_base_index = context->next_base_index;
						subcontext.uniqueness_preservation = NULL;
						subcontext.functional_dependencies = NIL;
						subcontext.query = subquery;
						subcontext.query_stack = lcons(subquery, list_copy(context->query_stack));
						subcontext.rte_to_base_index = context->rte_to_base_index;

						fkjoin_enumerate_and_compute(linitial(subquery->jointree->fromlist),
													 &subcontext,
													 &sub_uniqueness,
													 &sub_funcdeps);

						context->next_base_index = subcontext.next_base_index;

						/* If subquery has WHERE/LIMIT/HAVING, rows are not preserved */
						if (subquery->jointree->quals != NULL ||
							subquery->limitOffset != NULL ||
							subquery->limitCount != NULL ||
							subquery->havingQual != NULL)
						{
							ListCell   *fd_lc;
							List	   *filtered_fds = NIL;

							foreach(fd_lc, sub_funcdeps)
							{
								FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(fd_lc);

								if (fd->determinant != fd->dependent)
									filtered_fds = lappend(filtered_fds, fd);
							}
							sub_funcdeps = filtered_fds;
						}

						/* Analyze GROUP BY columns */
						foreach(lc, subquery->groupClause)
						{
							SortGroupClause *sgc = lfirst_node(SortGroupClause, lc);
							TargetEntry *tle = NULL;
							ListCell   *tlc;

							/* Find the target entry with matching tleSortGroupRef */
							foreach(tlc, subquery->targetList)
							{
								tle = (TargetEntry *) lfirst(tlc);
								if (tle->ressortgroupref == sgc->tleSortGroupRef)
									break;
								tle = NULL;
							}

							if (tle == NULL || !IsA(tle->expr, Var))
							{
								all_simple = false;
								break;
							}

							{
								Var		   *v = (Var *) tle->expr;

								if (group_varno == 0)
									group_varno = v->varno;
								else if (group_varno != v->varno)
								{
									all_simple = false;
									break;
								}

								group_cols = bms_add_member(group_cols, v->varattno);
							}
						}

						/*
						 * GROUP BY preserves uniqueness and rows only if:
						 * 1. GROUP BY columns form a unique key on the base table
						 * 2. That base table's rows were preserved before GROUP BY
						 *    (indicated by having a self-dependency in sub_funcdeps)
						 */
						if (all_simple && group_varno > 0)
						{
							RangeTblEntry *group_rte = rt_fetch(group_varno, subquery->rtable);
							RangeTblEntry *base_group_rte = NULL;
							Bitmapset  *base_group_cols = NULL;

							if (group_rte->rtekind == RTE_RELATION &&
								group_rte->relid != InvalidOid)
							{
								base_group_rte = group_rte;
								base_group_cols = group_cols;
							}
							else if (group_rte->rtekind == RTE_GROUP)
							{
								/*
								 * In modern PG, GROUP BY columns point to
								 * RTE_GROUP. Follow group expressions to find
								 * the actual base table columns.
								 */
								int			base_varno = 0;
								Bitmapset  *resolved_cols = NULL;
								bool		group_resolved = true;
								int			col;

								col = -1;
								while ((col = bms_next_member(group_cols, col)) >= 0)
								{
									Node	   *expr;
									Var		   *gvar;

									if (col <= 0 || col > list_length(group_rte->groupexprs))
									{
										group_resolved = false;
										break;
									}

									expr = (Node *) list_nth(group_rte->groupexprs, col - 1);
									if (!IsA(expr, Var))
									{
										group_resolved = false;
										break;
									}

									gvar = (Var *) expr;
									if (base_varno == 0)
										base_varno = gvar->varno;
									else if (base_varno != gvar->varno)
									{
										group_resolved = false;
										break;
									}
									resolved_cols = bms_add_member(resolved_cols, gvar->varattno);
								}

								if (group_resolved && base_varno > 0)
								{
									RangeTblEntry *resolved_rte = rt_fetch(base_varno, subquery->rtable);

									if (resolved_rte->rtekind == RTE_RELATION &&
										resolved_rte->relid != InvalidOid)
									{
										base_group_rte = resolved_rte;
										base_group_cols = resolved_cols;
									}
								}
							}

							if (base_group_rte != NULL)
							{
								Relation	rel = table_open(base_group_rte->relid, AccessShareLock);
								bool		has_unique_key = check_unique_index_covers_columns(rel, base_group_cols);

								table_close(rel, AccessShareLock);

								if (has_unique_key)
								{
									RTEBaseIndexEntry *entry;

									entry = hash_search(context->rte_to_base_index,
														&base_group_rte,
														HASH_FIND,
														NULL);
									if (entry != NULL)
									{
										/*
										 * GROUP BY on a unique key always preserves
										 * uniqueness. Propagate the base table's
										 * uniqueness regardless of row preservation.
										 */
										*uniqueness_out = bms_add_member(*uniqueness_out,
																		 entry->base_idx);

										/*
										 * Row preservation (self-dependency) is only
										 * propagated if the base table's rows were
										 * preserved before the GROUP BY.
										 */
										if (has_self_dependency(sub_funcdeps,
															   entry->base_idx))
										{
											*funcdeps_out = list_make1(makeFKJoinFuncDep(entry->base_idx,
																						 entry->base_idx));
										}
									}
								}
							}
						}

						bms_free(group_cols);
					}
				}
				else if (rte->rtekind == RTE_CTE)
				{
					/*
					 * CTE: find the CTE definition and enumerate its contents.
					 * This is needed to populate the hash table with base RTEs.
					 */
					CommonTableExpr *cte = NULL;
					ListCell   *qlc;

					/* Search through query stack for the CTE */
					foreach(qlc, context->query_stack)
					{
						Query	   *search_query = lfirst_node(Query, qlc);
						ListCell   *clc;

						foreach(clc, search_query->cteList)
						{
							CommonTableExpr *c = (CommonTableExpr *) lfirst(clc);

							if (strcmp(c->ctename, rte->ctename) == 0)
							{
								cte = c;
								break;
							}
						}

						if (cte != NULL)
							break;
					}

					if (cte != NULL && !cte->cterecursive)
					{
						Query	   *ctequery = castNode(Query, cte->ctequery);

						if (ctequery->jointree &&
							ctequery->jointree->fromlist &&
							list_length(ctequery->jointree->fromlist) == 1 &&
							!ctequery->distinctClause &&
							!ctequery->groupClause &&
							!ctequery->groupingSets &&
							!ctequery->hasTargetSRFs)
						{
							FKJoinValidationContext subcontext;

							subcontext.next_base_index = context->next_base_index;
							subcontext.uniqueness_preservation = NULL;
							subcontext.functional_dependencies = NIL;
							subcontext.query = ctequery;
							subcontext.query_stack = lcons(ctequery, list_copy(context->query_stack));
							subcontext.rte_to_base_index = context->rte_to_base_index;

							fkjoin_enumerate_and_compute(linitial(ctequery->jointree->fromlist),
														 &subcontext,
														 uniqueness_out,
														 funcdeps_out);

							context->next_base_index = subcontext.next_base_index;

							/* If CTE query has WHERE clause, remove self-dependencies */
							if (ctequery->jointree->quals != NULL ||
								ctequery->limitOffset != NULL ||
								ctequery->limitCount != NULL ||
								ctequery->havingQual != NULL)
							{
								ListCell   *fd_lc;
								List	   *filtered_fds = NIL;

								foreach(fd_lc, *funcdeps_out)
								{
									FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(fd_lc);

									if (fd->determinant != fd->dependent)
										filtered_fds = lappend(filtered_fds, fd);
								}
								*funcdeps_out = filtered_fds;
							}
						}
					}
				}
			}
			break;

		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) jtnode;
				Bitmapset  *left_uniqueness = NULL;
				List	   *left_funcdeps = NIL;
				Bitmapset  *right_uniqueness = NULL;
				List	   *right_funcdeps = NIL;

				/* Recursively process children */
				fkjoin_enumerate_and_compute(join->larg, context,
											 &left_uniqueness, &left_funcdeps);
				fkjoin_enumerate_and_compute(join->rarg, context,
											 &right_uniqueness, &right_funcdeps);

				if (join->fkJoin != NULL)
				{
					/* FK join: apply FK-specific property merging and validation */
					ForeignKeyJoinNode *fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);
					RangeTblEntry *referencing_rte;
					RangeTblEntry *referenced_rte;
					RangeTblEntry *base_referencing_rte;
					RangeTblEntry *base_referenced_rte;
					List	   *referencing_base_attnums;
					List	   *referenced_base_attnums;
					Oid			referencing_relid;
					bool		fk_cols_unique;
					bool		fk_cols_not_null;
					Bitmapset  *referencing_uniqueness;
					Bitmapset  *referenced_uniqueness;
					List	   *referencing_funcdeps;
					List	   *referenced_funcdeps;
					int			referencing_base_idx;
					int			referenced_base_idx;
					RTEBaseIndexEntry *ref_entry;
					RTEBaseIndexEntry *refing_entry;

					referencing_rte = rt_fetch(fkjn->referencingVarno, context->query->rtable);
					referenced_rte = rt_fetch(fkjn->referencedVarno, context->query->rtable);

					/* Drill down to base relations using query stack */
					base_referencing_rte = drill_down_to_base_rel(context->query_stack,
																  referencing_rte,
																  fkjn->referencingAttnums,
																  &referencing_base_attnums);
					base_referenced_rte = drill_down_to_base_rel(context->query_stack,
																 referenced_rte,
																 fkjn->referencedAttnums,
																 &referenced_base_attnums);

					referencing_relid = base_referencing_rte->relid;

					/*
					 * If the FK constraint was deferred during parse (e.g.
					 * because a CTE or JOIN RTE couldn't be resolved then),
					 * verify it now that we can drill down to base tables.
					 */
					if (fkjn->constraint == InvalidOid)
					{
						Oid		fkoid;

						fkoid = find_foreign_key(base_referencing_rte->relid,
												 base_referenced_rte->relid,
												 referencing_base_attnums,
												 referenced_base_attnums);
						if (fkoid == InvalidOid)
							ereport(ERROR,
									(errcode(ERRCODE_UNDEFINED_OBJECT),
									 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
											get_rel_name(base_referencing_rte->relid),
											attnum_list_to_string(base_referencing_rte->relid, referencing_base_attnums),
											get_rel_name(base_referenced_rte->relid),
											attnum_list_to_string(base_referenced_rte->relid, referenced_base_attnums))));
					}

					/* Determine which side is referencing/referenced */
					if (fkjn->fkdir == FKDIR_FROM)
					{
						referencing_uniqueness = left_uniqueness;
						referencing_funcdeps = left_funcdeps;
						referenced_uniqueness = right_uniqueness;
						referenced_funcdeps = right_funcdeps;
					}
					else
					{
						referencing_uniqueness = right_uniqueness;
						referencing_funcdeps = right_funcdeps;
						referenced_uniqueness = left_uniqueness;
						referenced_funcdeps = left_funcdeps;
					}

					/*
					 * Look up base indices from hash table using RTE pointers.
					 * The same RTE object is reached via both enumeration and
					 * drill-down, so the hash lookup gives us the correct index.
					 */
					ref_entry = hash_search(context->rte_to_base_index,
											&base_referenced_rte,
											HASH_FIND,
											NULL);
					refing_entry = hash_search(context->rte_to_base_index,
											   &base_referencing_rte,
											   HASH_FIND,
											   NULL);

					if (ref_entry == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("base RTE for referenced relation not found in mapping")));
					if (refing_entry == NULL)
						ereport(ERROR,
								(errcode(ERRCODE_INTERNAL_ERROR),
								 errmsg("base RTE for referencing relation not found in mapping")));

					referenced_base_idx = ref_entry->base_idx;
					referencing_base_idx = refing_entry->base_idx;

					/*
					 * Validate FK join constraints:
					 * 1. Referenced side must preserve uniqueness of keys
					 * 2. Referenced side must preserve all rows (self-dependency)
					 *
					 * When the referenced table is a direct base table
					 * (RTE_RELATION), its PK/UNIQUE constraints are inherently
					 * valid regardless of what other joins have been applied.
					 * After fireRIRrules, remaining RTE_RELATION entries are
					 * genuine base tables (views are already expanded).
					 */
					{
						bool	need_derived_checks =
							(referenced_rte->rtekind != RTE_RELATION);

						if (need_derived_checks)
						{
							if (!bms_is_member(referenced_base_idx,
											   referenced_uniqueness))
							{
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_FOREIGN_KEY),
										 errmsg("foreign key join violation"),
										 errdetail("referenced relation does not preserve uniqueness of keys")));
							}

							if (!has_self_dependency(referenced_funcdeps,
													 referenced_base_idx))
							{
								ereport(ERROR,
										(errcode(ERRCODE_INVALID_FOREIGN_KEY),
										 errmsg("foreign key join violation"),
										 errdetail("referenced relation does not preserve all rows")));
							}
						}
					}

					/* Check FK column properties */
					fk_cols_unique = is_referencing_cols_unique(referencing_relid,
																referencing_base_attnums);
					fk_cols_not_null = is_referencing_cols_not_null(referencing_relid,
																	referencing_base_attnums,
																	NULL);

					/* Update uniqueness preservation */
					*uniqueness_out = update_uniqueness_preservation(referencing_uniqueness,
																	 referenced_uniqueness,
																	 referencing_base_idx,
																	 fk_cols_unique);

					/* Update functional dependencies */
					*funcdeps_out = update_functional_dependencies(referencing_funcdeps,
																   referencing_base_idx,
																   referenced_funcdeps,
																   referenced_base_idx,
																   fk_cols_not_null,
																   join->jointype,
																   fkjn->fkdir);
				}
				else
				{
					/* Non-FK join: conservative merge */
					switch (join->jointype)
					{
						case JOIN_INNER:
							/* Inner join: both sides must match, uniqueness preserved */
							*uniqueness_out = bms_union(left_uniqueness, right_uniqueness);
							/* Functional dependencies broken unless it's a special case */
							*funcdeps_out = NIL;
							break;

						case JOIN_LEFT:
							/* Left join preserves left side rows */
							*uniqueness_out = bms_union(left_uniqueness, right_uniqueness);
							*funcdeps_out = list_copy(left_funcdeps);
							break;

						case JOIN_RIGHT:
							/* Right join preserves right side rows */
							*uniqueness_out = bms_union(left_uniqueness, right_uniqueness);
							*funcdeps_out = list_copy(right_funcdeps);
							break;

						case JOIN_FULL:
							/* Full join preserves both sides */
							*uniqueness_out = bms_union(left_uniqueness, right_uniqueness);
							*funcdeps_out = list_concat(list_copy(left_funcdeps),
														list_copy(right_funcdeps));
							break;

						default:
							*uniqueness_out = bms_union(left_uniqueness, right_uniqueness);
							*funcdeps_out = NIL;
							break;
					}
				}
			}
			break;

		case T_FromExpr:
			{
				FromExpr   *f = (FromExpr *) jtnode;
				ListCell   *lc;

				/* Process all from-list items */
				foreach(lc, f->fromlist)
				{
					Bitmapset  *item_uniqueness = NULL;
					List	   *item_funcdeps = NIL;

					fkjoin_enumerate_and_compute(lfirst(lc), context,
												 &item_uniqueness, &item_funcdeps);

					*uniqueness_out = bms_union(*uniqueness_out, item_uniqueness);
					*funcdeps_out = list_concat(*funcdeps_out, item_funcdeps);
				}

				/* WHERE clause breaks row preservation */
				if (f->quals != NULL)
				{
					ListCell   *lc2;
					List	   *filtered_fds = NIL;

					foreach(lc2, *funcdeps_out)
					{
						FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc2);

						if (fd->determinant != fd->dependent)
							filtered_fds = lappend(filtered_fds, fd);
					}
					*funcdeps_out = filtered_fds;
				}
			}
			break;

		default:
			elog(ERROR, "unrecognized node type in FK join validation: %d",
				 (int) nodeTag(jtnode));
			break;
	}
}

/*
 * makeFKJoinFuncDep
 *		Create a new FKJoinFuncDep node.
 */
static FKJoinFuncDep *
makeFKJoinFuncDep(int determinant, int dependent)
{
	FKJoinFuncDep *fd = palloc(sizeof(FKJoinFuncDep));

	fd->type = T_List;			/* Use T_List as a placeholder tag */
	fd->determinant = determinant;
	fd->dependent = dependent;

	return fd;
}

/*
 * has_self_dependency
 *		Check if a base index has a self-dependency in the functional dependencies list.
 */
static bool
has_self_dependency(List *funcdeps, int base_idx)
{
	ListCell   *lc;

	foreach(lc, funcdeps)
	{
		FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc);

		if (fd->determinant == base_idx && fd->dependent == base_idx)
			return true;
	}

	return false;
}

/*
 * update_uniqueness_preservation
 *		Update uniqueness preservation for an FK join.
 *
 * Uniqueness preservation is propagated from the referencing relation.
 * If the FK columns form a unique key and the referencing base table
 * preserves uniqueness, then uniqueness from the referenced relation
 * is also added.
 */
static Bitmapset *
update_uniqueness_preservation(Bitmapset *referencing_up,
							   Bitmapset *referenced_up,
							   int referencing_base_idx,
							   bool fk_cols_unique)
{
	Bitmapset  *result;

	/* Start with uniqueness from the referencing relation */
	result = bms_copy(referencing_up);

	/* If FK cols are unique and referencing preserves uniqueness, add referenced */
	if (fk_cols_unique && bms_is_member(referencing_base_idx, referencing_up))
		result = bms_union(result, referenced_up);

	return result;
}

/*
 * update_functional_dependencies
 *		Update functional dependencies for an FK join.
 */
static List *
update_functional_dependencies(List *referencing_fds,
							   int referencing_base_idx,
							   List *referenced_fds,
							   int referenced_base_idx,
							   bool fk_cols_not_null,
							   JoinType join_type,
							   ForeignKeyDirection fk_dir)
{
	List	   *result = NIL;
	bool		referenced_has_self_dep = false;
	bool		referencing_preserved_due_to_outer_join = false;
	ListCell   *lc;

	/*
	 * Step 1: Add functional dependencies from the referencing relation when
	 * an outer join preserves the referencing relation's tuples.
	 */
	if ((fk_dir == FKDIR_FROM && join_type == JOIN_LEFT) ||
		(fk_dir == FKDIR_TO && join_type == JOIN_RIGHT) ||
		join_type == JOIN_FULL)
	{
		foreach(lc, referencing_fds)
		{
			result = lappend(result, lfirst(lc));
		}
		referencing_preserved_due_to_outer_join = true;
	}

	/*
	 * Step 2: Add functional dependencies from the referenced relation when
	 * an outer join preserves the referenced relation's tuples.
	 */
	if ((fk_dir == FKDIR_TO && join_type == JOIN_LEFT) ||
		(fk_dir == FKDIR_FROM && join_type == JOIN_RIGHT) ||
		join_type == JOIN_FULL)
	{
		foreach(lc, referenced_fds)
		{
			result = lappend(result, lfirst(lc));
		}
	}

	/*
	 * Step 3: If any foreign key column permits NULL values, we cannot
	 * guarantee row preservation in an inner FK join.
	 */
	if (!fk_cols_not_null)
		return result;

	/*
	 * Step 4: Verify that the referenced relation preserves all its rows -
	 * indicated by a self-dependency.
	 */
	foreach(lc, referenced_fds)
	{
		FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc);

		if (fd->determinant == referenced_base_idx &&
			fd->dependent == referenced_base_idx)
		{
			referenced_has_self_dep = true;
			break;
		}
	}

	if (!referenced_has_self_dep)
		return result;

	/*
	 * Step 5: Preserve inherited functional dependencies from the referencing
	 * relation.
	 */
	if (!referencing_preserved_due_to_outer_join)
	{
		foreach(lc, referencing_fds)
		{
			FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc);

			if (fd->dependent == referencing_base_idx)
			{
				ListCell   *lc2;

				foreach(lc2, referencing_fds)
				{
					FKJoinFuncDep *fd2 = (FKJoinFuncDep *) lfirst(lc2);

					if (fd2->determinant == fd->determinant)
						result = lappend(result, fd2);
				}
			}
		}
	}

	/*
	 * Step 6: Establish transitive functional dependencies.
	 */
	foreach(lc, referencing_fds)
	{
		FKJoinFuncDep *fd = (FKJoinFuncDep *) lfirst(lc);

		if (fd->dependent == referencing_base_idx)
		{
			ListCell   *lc2;

			foreach(lc2, referenced_fds)
			{
				FKJoinFuncDep *fd2 = (FKJoinFuncDep *) lfirst(lc2);

				if (fd2->determinant == referenced_base_idx)
				{
					result = lappend(result,
									 makeFKJoinFuncDep(fd->determinant, fd2->dependent));
				}
			}
		}
	}

	return result;
}

/*
 * build_fk_join_on_clause
 *		Constructs the ON clause for the foreign key join
 */
static Node *
build_fk_join_on_clause(ParseState *pstate, ParseNamespaceColumn *l_nscols, List *l_attnums,
						ParseNamespaceColumn *r_nscols, List *r_attnums)
{
	Node	   *result;
	List	   *andargs = NIL;
	ListCell   *lc,
			   *rc;

	Assert(list_length(l_attnums) == list_length(r_attnums));

	forboth(lc, l_attnums, rc, r_attnums)
	{
		ParseNamespaceColumn *l_col = &l_nscols[lfirst_int(lc) - 1];
		ParseNamespaceColumn *r_col = &r_nscols[lfirst_int(rc) - 1];
		Var		   *l_var,
				   *r_var;
		A_Expr	   *e;

		l_var = makeVar(l_col->p_varno,
						l_col->p_varattno,
						l_col->p_vartype,
						l_col->p_vartypmod,
						l_col->p_varcollid,
						0);
		r_var = makeVar(r_col->p_varno,
						r_col->p_varattno,
						r_col->p_vartype,
						r_col->p_vartypmod,
						r_col->p_varcollid,
						0);

		e = makeSimpleA_Expr(AEXPR_OP, "=",
							 (Node *) copyObject(l_var),
							 (Node *) copyObject(r_var),
							 -1);

		andargs = lappend(andargs, e);
	}

	if (list_length(andargs) == 1)
		result = (Node *) linitial(andargs);
	else
		result = (Node *) makeBoolExpr(AND_EXPR, andargs, -1);

	result = transformExpr(pstate, result, EXPR_KIND_JOIN_ON);
	result = coerce_to_boolean(pstate, result, "FOREIGN KEY JOIN");

	return result;
}

/*
 * find_foreign_key
 *		Searches the system catalogs to locate the foreign key constraint
 */
static Oid
find_foreign_key(Oid referencing_relid, Oid referenced_relid,
				 List *referencing_attnums, List *referenced_attnums)
{
	Relation	rel = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			fkoid = InvalidOid;

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(referencing_relid));
	scan = systable_beginscan(rel, ConstraintRelidTypidNameIndexId, true, NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		bool		conkey_isnull,
					confkey_isnull;
		Datum		conkey_datum,
					confkey_datum;
		ArrayType  *conkey_arr,
				   *confkey_arr;
		int16	   *conkey,
				   *confkey;
		int			nkeys;
		bool		found = true;

		if (con->contype != CONSTRAINT_FOREIGN || con->confrelid != referenced_relid)
			continue;

		conkey_datum = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_conkey, &conkey_isnull);
		confkey_datum = SysCacheGetAttr(CONSTROID, tup, Anum_pg_constraint_confkey, &confkey_isnull);
		if (conkey_isnull || confkey_isnull)
			continue;

		conkey_arr = DatumGetArrayTypeP(conkey_datum);
		confkey_arr = DatumGetArrayTypeP(confkey_datum);
		nkeys = ArrayGetNItems(ARR_NDIM(conkey_arr), ARR_DIMS(conkey_arr));
		if (nkeys != ArrayGetNItems(ARR_NDIM(confkey_arr), ARR_DIMS(confkey_arr)) ||
			nkeys != list_length(referencing_attnums))
			continue;

		conkey = (int16 *) ARR_DATA_PTR(conkey_arr);
		confkey = (int16 *) ARR_DATA_PTR(confkey_arr);

		/*
		 * Check if each fk pair (conkey[i], confkey[i]) matches some
		 * (referencing_cols[j], referenced_cols[j])
		 */
		for (int i = 0; i < nkeys && found; i++)
		{
			bool		match = false;
			ListCell   *lc1,
					   *lc2;

			forboth(lc1, referencing_attnums, lc2, referenced_attnums)
				if (lfirst_int(lc1) == conkey[i] && lfirst_int(lc2) == confkey[i])
				match = true;

			if (!match)
				found = false;
		}

		if (found)
		{
			fkoid = con->oid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return fkoid;
}

/*
 * column_list_to_string
 *		Converts a list of column names to a comma-separated string
 */
static char *
column_list_to_string(const List *columns)
{
	StringInfoData string;
	ListCell   *l;
	bool		first = true;

	initStringInfo(&string);

	foreach(l, columns)
	{
		char	   *name = strVal(lfirst(l));

		if (!first)
			appendStringInfoString(&string, ", ");

		appendStringInfoString(&string, name);

		first = false;
	}

	return string.data;
}

/*
 * attnum_list_to_string
 *		Converts a list of attribute numbers to a comma-separated column name string
 */
static char *
attnum_list_to_string(Oid relid, List *attnums)
{
	StringInfoData string;
	ListCell   *lc;
	bool		first = true;

	initStringInfo(&string);

	foreach(lc, attnums)
	{
		AttrNumber	attnum = lfirst_int(lc);
		char	   *attname = get_attname(relid, attnum, false);

		if (!first)
			appendStringInfoString(&string, ", ");

		appendStringInfoString(&string, attname);

		first = false;
	}

	return string.data;
}

/*
 * subquery_has_cte_ref
 *		Check if a subquery RTE (recursively) references any CTEs.
 *		Used to defer FK validation during parse when CTE context is unavailable.
 */
static bool
subquery_has_cte_ref(RangeTblEntry *rte)
{
	ListCell   *lc;

	if (rte->rtekind != RTE_SUBQUERY)
		return false;

	foreach(lc, rte->subquery->rtable)
	{
		RangeTblEntry *sub_rte = lfirst_node(RangeTblEntry, lc);

		if (sub_rte->rtekind == RTE_CTE)
			return true;
		if (subquery_has_cte_ref(sub_rte))
			return true;
	}

	return false;
}

/*
 * drill_down_to_base_rel
 *		Resolves the base relation from a potentially derived relation
 *
 * query_stack is a list of Query nodes representing the query hierarchy,
 * with the innermost (current) query first and parent queries following.
 * This is needed for CTE lookups which may reference CTEs in parent queries.
 */
static RangeTblEntry *
drill_down_to_base_rel(List *query_stack, RangeTblEntry *rte,
					   List *attnums, List **base_attnums)
{
	RangeTblEntry *base_rte = NULL;
	Query	   *query = query_stack ? linitial_node(Query, query_stack) : NULL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);

				switch (rel->rd_rel->relkind)
				{
					case RELKIND_VIEW:
						base_rte = drill_down_to_base_rel_query(query_stack,
																get_view_query(rel),
																attnums,
																base_attnums);
						break;

					case RELKIND_RELATION:
					case RELKIND_PARTITIONED_TABLE:
						base_rte = rte;
						*base_attnums = attnums;
						break;

					default:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins involving this type of relation are not supported"),
								 errdetail_relkind_not_supported(rel->rd_rel->relkind)));
				}

				table_close(rel, AccessShareLock);
			}
			break;

		case RTE_SUBQUERY:
			base_rte = drill_down_to_base_rel_query(query_stack, rte->subquery,
												   attnums, base_attnums);
			break;

		case RTE_JOIN:
			{
				int			next_rtindex = 0;
				List	   *next_attnums = NIL;
				ListCell   *lc;

				foreach(lc, attnums)
				{
					int			attno = lfirst_int(lc);
					Node	   *node;
					Var		   *var;

					node = list_nth(rte->joinaliasvars, attno - 1);
					if (!IsA(node, Var))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins require direct column references, found expression")));

					var = castNode(Var, node);

					if (next_rtindex == 0)
						next_rtindex = var->varno;
					else if (next_rtindex != var->varno)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("all key columns must belong to the same table")));

					next_attnums = lappend_int(next_attnums, var->varattno);
				}

				Assert(next_rtindex != 0);

				if (query)
					base_rte = drill_down_to_base_rel(query_stack,
													  rt_fetch(next_rtindex, query->rtable),
													  next_attnums,
													  base_attnums);
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot resolve join RTE without query context")));
			}
			break;

		case RTE_GROUP:
			{
				int			next_rtindex = 0;
				List	   *next_attnums = NIL;
				ListCell   *lc;

				foreach(lc, attnums)
				{
					int			attno = lfirst_int(lc);
					Var		   *var = NULL;
					Node	   *expr;

					if (attno > 0 && attno <= list_length(rte->groupexprs))
					{
						expr = (Node *) list_nth(rte->groupexprs, attno - 1);

						if (IsA(expr, Var))
							var = (Var *) expr;
					}

					if (!var)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("GROUP BY column %d is not a simple column reference", attno)));

					if (next_rtindex == 0)
						next_rtindex = var->varno;
					else if (next_rtindex != var->varno)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("all key columns must belong to the same table")));

					next_attnums = lappend_int(next_attnums, var->varattno);
				}

				if (next_rtindex == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("no valid columns found in GROUP BY for foreign key join")));

				if (query)
					base_rte = drill_down_to_base_rel(query_stack,
													  rt_fetch(next_rtindex, query->rtable),
													  next_attnums,
													  base_attnums);
				else
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("cannot resolve GROUP RTE without query context")));
			}
			break;

		case RTE_CTE:
			{
				/*
				 * For CTE references, we need to find the CTE definition
				 * and drill down through its query.
				 *
				 * During parse (when query_stack is NULL), CTEs cannot be
				 * resolved and are handled specially by the caller.
				 */
				CommonTableExpr *cte = NULL;
				ListCell   *qlc;

				if (query_stack == NIL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key join with CTE \"%s\" requires query context",
									rte->ctename),
							 errhint("CTE-based foreign key joins are validated during query rewrite.")));

				/*
				 * Search through all queries in the stack for the CTE.
				 * We search all levels because the drill-down path may differ
				 * from the parse-time query hierarchy that set ctelevelsup.
				 */
				foreach(qlc, query_stack)
				{
					Query	   *search_query = lfirst_node(Query, qlc);
					ListCell   *clc;

					foreach(clc, search_query->cteList)
					{
						CommonTableExpr *c = (CommonTableExpr *) lfirst(clc);

						if (strcmp(c->ctename, rte->ctename) == 0)
						{
							cte = c;
							break;
						}
					}

					if (cte != NULL)
						break;
				}

				if (cte == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_UNDEFINED_OBJECT),
							 errmsg("CTE \"%s\" not found in query hierarchy",
									rte->ctename)));

				/* Recursive CTEs are not supported */
				if (cte->cterecursive)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins with recursive CTEs are not supported")));

				/*
				 * After parse analysis, ctequery is a Query node.
				 * Drill down through it like a subquery, preserving the
				 * query stack for potential nested CTE references.
				 */
				base_rte = drill_down_to_base_rel_query(query_stack,
														castNode(Query, cte->ctequery),
														attnums, base_attnums);
			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported")));
	}

	return base_rte;
}

/*
 * drill_down_to_base_rel_query
 *		Resolves the base relation from a query
 *
 * query_stack is the parent query stack (for CTE lookups).
 * We push subquery onto the front when drilling down.
 */
static RangeTblEntry *
drill_down_to_base_rel_query(List *query_stack, Query *subquery,
							 List *attnums, List **base_attnums)
{
	int			next_rtindex = 0;
	List	   *next_attnums = NIL;
	ListCell   *lc;
	RangeTblEntry *result;
	List	   *new_stack;

	if (subquery->setOperations != NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving set operations are not supported")));
	}

	if (subquery->commandType != CMD_SELECT ||
		subquery->distinctClause ||
		subquery->groupingSets ||
		subquery->hasTargetSRFs)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins not supported for these relations")));
	}

	foreach(lc, attnums)
	{
		int			attno = lfirst_int(lc);
		TargetEntry *matching_tle;
		Var		   *var;

		matching_tle = list_nth(subquery->targetList, attno - 1);

		if (!IsA(matching_tle->expr, Var))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("target entry \"%s\" is an expression, not a direct column reference",
							matching_tle->resname)));
		}

		var = castNode(Var, matching_tle->expr);

		if (next_rtindex == 0)
			next_rtindex = var->varno;
		else if (next_rtindex != var->varno)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("all key columns must belong to the same table")));
		}

		next_attnums = lappend_int(next_attnums, var->varattno);
	}

	Assert(next_rtindex != 0);

	/* Push this subquery onto the query stack for nested CTE lookups */
	new_stack = lcons(subquery, list_copy(query_stack));

	result = drill_down_to_base_rel(new_stack, rt_fetch(next_rtindex, subquery->rtable),
									next_attnums, base_attnums);

	return result;
}

/*
 * check_unique_index_covers_columns
 *		Check if the given columns are covered by a unique index on the relation
 */
static bool
check_unique_index_covers_columns(Relation rel, Bitmapset *columns)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		result = false;

	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;
		Form_pg_index indexForm;
		int			nindexattrs;
		Bitmapset  *index_cols = NULL;

		indexRel = index_open(indexoid, AccessShareLock);
		indexForm = indexRel->rd_index;

		if (!indexForm->indisunique)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		nindexattrs = indexForm->indnatts;
		for (int j = 0; j < nindexattrs; j++)
		{
			AttrNumber	attnum = indexForm->indkey.values[j];

			if (attnum > 0)
				index_cols = bms_add_member(index_cols, attnum);
		}

		index_close(indexRel, AccessShareLock);

		if (bms_is_subset(columns, index_cols))
		{
			result = true;
			bms_free(index_cols);
			break;
		}

		bms_free(index_cols);
	}

	list_free(indexoidlist);

	return result;
}

/*
 * is_referencing_cols_unique
 *		Determines if the foreign key columns in the referencing table
 *		are guaranteed to be unique by a constraint or index.
 */
static bool
is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums)
{
	Relation	rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		result = false;
	int			natts;

	natts = list_length(referencing_base_attnums);

	rel = table_open(referencing_relid, AccessShareLock);

	indexoidlist = RelationGetIndexList(rel);

	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;
		Form_pg_index indexForm;
		int			nindexattrs;
		bool		matches = true;
		ListCell   *lc;

		indexRel = index_open(indexoid, AccessShareLock);
		indexForm = indexRel->rd_index;

		if (!indexForm->indisunique)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		nindexattrs = indexForm->indnatts;

		if (natts != nindexattrs)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		foreach(lc, referencing_base_attnums)
		{
			AttrNumber	attnum = lfirst_int(lc);
			bool		col_found = false;

			for (int j = 0; j < nindexattrs; j++)
			{
				if (attnum == indexForm->indkey.values[j])
				{
					col_found = true;
					break;
				}
			}

			if (!col_found)
			{
				matches = false;
				break;
			}
		}

		index_close(indexRel, AccessShareLock);

		if (matches)
		{
			result = true;
			break;
		}
	}

	list_free(indexoidlist);
	table_close(rel, AccessShareLock);

	return result;
}

/*
 * is_referencing_cols_not_null
 *		Determines if all foreign key columns in the referencing table
 *		have NOT NULL constraints.
 */
static bool
is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums,
							 List **notNullConstraints)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ListCell   *lc;
	bool		all_not_null = true;
	List	   *constraints = NIL;

	rel = table_open(referencing_relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	foreach(lc, referencing_base_attnums)
	{
		AttrNumber	attnum = lfirst_int(lc);
		Form_pg_attribute attr;

		attr = TupleDescAttr(tupdesc, attnum - 1);

		if (!attr->attnotnull)
		{
			all_not_null = false;
			break;
		}

		if (notNullConstraints != NULL)
		{
			HeapTuple	conTup;

			conTup = findNotNullConstraintAttnum(referencing_relid, attnum);
			if (HeapTupleIsValid(conTup))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);

				constraints = lappend_oid(constraints, con->oid);
				heap_freetuple(conTup);
			}
		}
	}

	table_close(rel, AccessShareLock);

	if (notNullConstraints != NULL)
	{
		if (all_not_null)
			*notNullConstraints = constraints;
		else
		{
			list_free(constraints);
			*notNullConstraints = NIL;
		}
	}

	return all_not_null;
}
