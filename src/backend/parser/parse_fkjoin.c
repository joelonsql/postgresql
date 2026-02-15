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
#include "access/xact.h"
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
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

typedef struct QueryStack
{
	struct QueryStack *parent;
	Query	   *query;
} QueryStack;

static Node *build_fk_join_on_clause(ParseState *pstate,
									 ParseNamespaceColumn *l_nscols, List *l_attnums,
									 ParseNamespaceColumn *r_nscols, List *r_attnums);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid,
							 List *referencing_attnums, List *referenced_attnums);
static char *column_list_to_string(const List *columns);
static RangeTblEntry *drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
											 List *attnos, List **base_attnums,
											 int location, QueryStack *query_stack);
static CommonTableExpr *find_cte_for_rte(ParseState *pstate, QueryStack *query_stack,
										 RangeTblEntry *rte);
static RangeTblEntry *drill_down_to_base_rel_query(ParseState *pstate, Query *query,
												   List *attnos, List **base_attnums,
												   int location, QueryStack *query_stack);

static bool check_group_by_preserves_uniqueness(Query *query,
												 List **uniqueness_preservation,
												 List **null_injected_keys);
static bool check_unique_index_covers_columns(Relation rel, Bitmapset *columns,
											   bool *all_cols_not_null);
static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums,
										 List **notNullConstraints);
static List *update_uniqueness_preservation(List *referencing_uniqueness_preservation,
											List *referenced_uniqueness_preservation,
											RTEId *referencing_id,
											bool fk_cols_unique);
static void update_row_preserving(List *referencing_chains,
								  List *referencing_row_preserving,
								  RTEId *referencing_id,
								  List *referenced_chains,
								  List *referenced_row_preserving,
								  RTEId *referenced_id,
								  bool fk_cols_not_null,
								  JoinType join_type,
								  ForeignKeyDirection fk_dir,
								  List **result_chains,
								  List **result_row_preserving,
								  List *referencing_null_injected_keys,
								  List *referenced_null_injected_keys,
								  List **result_null_injected_keys);
static void analyze_join_tree(ParseState *pstate, Node *n,
							  Query *query,
							  RTEId *rte_id,
							  List **uniqueness_preservation,
							  List **notnull_fk_chains,
							  List **row_preserving,
							  List **null_injected_keys,
							  bool *found,
							  int location,
							  QueryStack *query_stack);

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
	List	   *referencing_uniqueness_preservation = NIL;
	List	   *referencing_notnull_fk_chains = NIL;
	List	   *referencing_row_preserving = NIL;
	List	   *referencing_null_injected_keys = NIL;
	List	   *referenced_uniqueness_preservation = NIL;
	List	   *referenced_notnull_fk_chains = NIL;
	List	   *referenced_row_preserving = NIL;
	List	   *referenced_null_injected_keys = NIL;
	Node	   *referencing_arg;
	Node	   *referenced_arg;
	List	   *referencing_base_attnums;
	List	   *referenced_base_attnums;
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;
	Oid			referencing_relid;
	Oid			referenced_relid;
	RTEId	   *referenced_id;
	bool		referencing_found = false;
	bool		referenced_found = false;
	bool		referenced_is_base_table = false;
	List	   *notNullConstraints = NIL;

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

	if (fkjn->fkdir == FKDIR_FROM)
	{
		referencing_rel = other_rel;
		referenced_rel = r_nsitem;
		referencing_cols = fkjn->refCols;
		referenced_cols = fkjn->localCols;
		referencing_arg = join->larg;
		referenced_arg = join->rarg;
	}
	else
	{
		referenced_rel = other_rel;
		referencing_rel = r_nsitem;
		referenced_cols = fkjn->refCols;
		referencing_cols = fkjn->localCols;
		referenced_arg = join->larg;
		referencing_arg = join->rarg;
	}

	referencing_rte = rt_fetch(referencing_rel->p_rtindex, pstate->p_rtable);
	referenced_rte = rt_fetch(referenced_rel->p_rtindex, pstate->p_rtable);

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

	base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
												  referencing_attnums,
												  &referencing_base_attnums,
												  fkjn->location, NULL);
	base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
												 referenced_attnums,
												 &referenced_base_attnums,
												 fkjn->location, NULL);

	referencing_relid = base_referencing_rte->relid;
	referenced_relid = base_referenced_rte->relid;
	referenced_id = base_referenced_rte->rteid;

	Assert(referencing_relid != InvalidOid && referenced_relid != InvalidOid);

	/*
	 * Check if referenced relation is a base table at same query level.
	 * Base tables always preserve their own uniqueness and rows, so we can
	 * skip the uniqueness/FD checks for them. These checks are only needed
	 * for derived tables (subqueries, views, CTEs) where the preservation
	 * properties may have been lost due to joins or filtering.
	 */
	if (referenced_rte->rtekind == RTE_RELATION && referenced_rte->relid != InvalidOid)
	{
		Relation	rel = table_open(referenced_rte->relid, AccessShareLock);

		if (rel->rd_rel->relkind == RELKIND_RELATION ||
			rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
		{
			referenced_is_base_table = true;
		}
		table_close(rel, AccessShareLock);
	}

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

	analyze_join_tree(pstate, referencing_arg, NULL, referencing_rte->rteid, &referencing_uniqueness_preservation, &referencing_notnull_fk_chains, &referencing_row_preserving, &referencing_null_injected_keys, &referencing_found, fkjn->location, NULL);

	/* Only analyze referenced side for derived tables - base tables always preserve uniqueness/rows */
	if (!referenced_is_base_table)
		analyze_join_tree(pstate, referenced_arg, NULL, referenced_rte->rteid, &referenced_uniqueness_preservation, &referenced_notnull_fk_chains, &referenced_row_preserving, &referenced_null_injected_keys, &referenced_found, fkjn->location, NULL);

	/*
	 * Check uniqueness preservation - only for derived tables.
	 * Base tables always preserve their own uniqueness.
	 */
	if (!referenced_is_base_table &&
		!list_member(referenced_uniqueness_preservation, referenced_id))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("referenced relation does not preserve uniqueness of keys"),
				 parser_errposition(pstate, fkjn->location)));
	}

	/*
	 * Check row preservation - only for derived tables.
	 * Base tables always preserve all their rows.
	 */
	if (!referenced_is_base_table &&
		!list_member(referenced_row_preserving, referenced_id))
	{
		/*
		 * This check ensures that the referenced relation is not filtered
		 * (e.g., by WHERE, LIMIT, OFFSET, HAVING, RLS). Foreign key joins
		 * require the referenced side to represent the complete set of rows
		 * from the underlying table(s).
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("referenced relation does not preserve all rows"),
				 parser_errposition(pstate, fkjn->location)));
	}

	/*
	 * Check that the referenced relation's key columns are not subject to
	 * NULL introduction within the derived relation.  Outer joins can
	 * produce "ghost" rows where the inner side's columns are all NULL,
	 * and GROUP BY on a nullable UNIQUE column can collapse NULLs into a
	 * single group.  Either case violates the assumption that the
	 * referenced side behaves like a primary key (unique and not null).
	 */
	if (!referenced_is_base_table &&
		list_member(referenced_null_injected_keys, referenced_id))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("NULL values may be introduced into the referenced relation's key columns"),
				 parser_errposition(pstate, fkjn->location)));
	}

	join->quals = build_fk_join_on_clause(pstate, referencing_rel->p_nscolumns, referencing_attnums, referenced_rel->p_nscolumns, referenced_attnums);

	/*
	 * For inner joins (and the inner side of outer joins), the row
	 * preservation guarantee depends on the referencing columns being NOT
	 * NULL.  Collect the NOT NULL constraint OIDs so we can record
	 * dependencies on them.
	 *
	 * We need NOT NULL dependencies when the referencing table is on the
	 * "inner" side of a join (the side that can have rows filtered out):
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
				/* Left join: right side is inner, track if referencing is on right */
				need_not_null_deps = (referencing_arg == join->rarg);
				break;
			case JOIN_RIGHT:
				/* Right join: left side is inner, track if referencing is on left */
				need_not_null_deps = (referencing_arg == join->larg);
				break;
			case JOIN_FULL:
				/* Full join: both sides preserved, no NOT NULL dependency needed */
				break;
		}

		if (need_not_null_deps)
		{
			(void) is_referencing_cols_not_null(referencing_relid,
												referencing_base_attnums,
												&notNullConstraints);
		}
	}

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

static void
analyze_join_tree(ParseState *pstate, Node *n,
				  Query *query,
				  RTEId *rte_id,
				  List **uniqueness_preservation,
				  List **notnull_fk_chains,
				  List **row_preserving,
				  List **null_injected_keys,
				  bool *found,
				  int location,
				  QueryStack *query_stack)
{
	RangeTblEntry *rte;
	Query	   *inner_query = NULL;
	List	   *referencing_uniqueness_preservation = NIL;
	List	   *referencing_notnull_fk_chains = NIL;
	List	   *referencing_row_preserving = NIL;
	List	   *referencing_null_injected_keys = NIL;
	List	   *referenced_uniqueness_preservation = NIL;
	List	   *referenced_notnull_fk_chains = NIL;
	List	   *referenced_row_preserving = NIL;
	List	   *referenced_null_injected_keys = NIL;
	List	   *referencing_base_attnums;
	List	   *referenced_base_attnums;
	RangeTblEntry *base_referencing_rte;
	RangeTblEntry *base_referenced_rte;
	Oid			referencing_relid;
	RTEId	   *referencing_id;
	RTEId	   *referenced_id;
	bool		referencing_found = false;
	bool		referenced_found = false;

	switch (nodeTag(n))
	{
		case T_JoinExpr:
			{
				JoinExpr   *join = (JoinExpr *) n;
				List	   *rtable;
				Node	   *referencing_arg;
				Node	   *referenced_arg;
				RangeTblEntry *referencing_rte;
				RangeTblEntry *referenced_rte;
				ForeignKeyJoinNode *fkjn;

				if (join->fkJoin == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving non-foreign-key joins are not supported"),
							 parser_errposition(pstate, location)));

				fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);
				bool		fk_cols_unique;
				bool		fk_cols_not_null;

				if (query)
					rtable = query->rtable;
				else
					rtable = pstate->p_rtable;

				if (fkjn->fkdir == FKDIR_FROM)
				{
					referencing_arg = join->larg;
					referenced_arg = join->rarg;
				}
				else
				{
					referenced_arg = join->larg;
					referencing_arg = join->rarg;
				}

				referencing_rte = rt_fetch(fkjn->referencingVarno, rtable);
				referenced_rte = rt_fetch(fkjn->referencedVarno, rtable);

				analyze_join_tree(pstate, referencing_arg, query, rte_id, &referencing_uniqueness_preservation, &referencing_notnull_fk_chains, &referencing_row_preserving, &referencing_null_injected_keys, &referencing_found, location, query_stack);
				analyze_join_tree(pstate, referenced_arg, query, rte_id, &referenced_uniqueness_preservation, &referenced_notnull_fk_chains, &referenced_row_preserving, &referenced_null_injected_keys, &referenced_found, location, query_stack);

				base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
															  fkjn->referencingAttnums,
															  &referencing_base_attnums,
															  location, query_stack);
				base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
															 fkjn->referencedAttnums,
															 &referenced_base_attnums,
															 location, query_stack);

				referencing_relid = base_referencing_rte->relid;
				referencing_id = base_referencing_rte->rteid;
				referenced_id = base_referenced_rte->rteid;

				fk_cols_unique = is_referencing_cols_unique(referencing_relid, referencing_base_attnums);
				fk_cols_not_null = is_referencing_cols_not_null(referencing_relid, referencing_base_attnums, NULL);

				*uniqueness_preservation = update_uniqueness_preservation(
																		  referencing_uniqueness_preservation,
																		  referenced_uniqueness_preservation,
																		  referencing_id,
																		  fk_cols_unique
					);
				update_row_preserving(
									 referencing_notnull_fk_chains,
									 referencing_row_preserving,
									 referencing_id,
									 referenced_notnull_fk_chains,
									 referenced_row_preserving,
									 referenced_id,
									 fk_cols_not_null,
									 join->jointype,
									 fkjn->fkdir,
									 notnull_fk_chains,
									 row_preserving,
									 referencing_null_injected_keys,
									 referenced_null_injected_keys,
									 null_injected_keys);

				/* Set found based on whether rte_id was in either subtree or matches either relation */
				if (referencing_found || referenced_found ||
					equal(referencing_rte->rteid, rte_id) || equal(referenced_rte->rteid, rte_id))
				{
					*found = true;
				}

			}
			break;

		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) n;
				int			rtindex = rtr->rtindex;

				/* Use the appropriate range table for lookups */
				if (query)
					rte = rt_fetch(rtindex, query->rtable);
				else
					rte = rt_fetch(rtindex, pstate->p_rtable);

				/* Process the referenced RTE */
				switch (rte->rtekind)
				{
					case RTE_RELATION:
						{
							Relation	rel;

							/* Open the relation to check its type */
							rel = table_open(rte->relid, AccessShareLock);

							if (rel->rd_rel->relkind == RELKIND_VIEW)
							{
								inner_query = get_view_query(rel);
							}
							else if (rel->rd_rel->relkind == RELKIND_RELATION ||
									 rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
							{
								*uniqueness_preservation = list_make1(rte->rteid);

								/*
								 * Mark as row-preserving if not filtered by
								 * WHERE/OFFSET/LIMIT/HAVING.
								 */
								if (!query || (!query->jointree->quals &&
											   !query->limitOffset &&
											   !query->limitCount &&
											   !query->havingQual))
									*row_preserving = list_make1(rte->rteid);
							}

							/* Close the relation */
							table_close(rel, AccessShareLock);
						}
						break;

					case RTE_SUBQUERY:
						inner_query = rte->subquery;
						break;

					case RTE_CTE:
						{
							CommonTableExpr *cte;

							cte = find_cte_for_rte(pstate, query_stack, rte);
							if (!cte)
								elog(ERROR, "could not find CTE \"%s\" (analyze_join_tree)", rte->ctename);

							if (!cte->cterecursive && IsA(cte->ctequery, Query))
								inner_query = (Query *) cte->ctequery;
						}
						break;

					default:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins involving this RTE kind are not supported"),
								 parser_errposition(pstate, location)));
						break;
				}

				/* Common path for processing any inner query */
				if (inner_query != NULL)
				{
					/*
					 * Traverse the inner query if it has a single fromlist
					 * item
					 */
					if (inner_query->jointree && inner_query->jointree->fromlist &&
						list_length(inner_query->jointree->fromlist) == 1)
					{
						QueryStack	new_stack = {.parent=query_stack, .query=inner_query};

						analyze_join_tree(pstate,
										  (Node *) linitial(inner_query->jointree->fromlist),
										  inner_query, rte_id, uniqueness_preservation, notnull_fk_chains, row_preserving, null_injected_keys, found, location,
										  &new_stack);

						/*
						 * If the inner query has GROUP BY, check if it
						 * preserves uniqueness. If it does, add the current
						 * RTE to uniqueness preservation.
						 */
						if (inner_query->groupClause)
						{
							if (!check_group_by_preserves_uniqueness(inner_query, uniqueness_preservation, null_injected_keys))
								*uniqueness_preservation = NIL;
						}
					}
				}
			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported node type in foreign key join traversal"),
					 parser_errposition(pstate, location)));
			break;
	}
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
 * find_cte_for_rte
 *              Locate the CTE referenced by an RTE either in the supplied
 *              stack of queries or in the ParseState's namespace.
 */
static CommonTableExpr *
find_cte_for_rte(ParseState *pstate, QueryStack *query_stack, RangeTblEntry *rte)
{
	Index		levelsup = rte->ctelevelsup;

	Assert(rte->rtekind == RTE_CTE);

	for (QueryStack *qs = query_stack; qs; qs = qs->parent)
	{
		if (levelsup == 0)
		{
			ListCell   *lc;

			foreach(lc, qs->query->cteList)
			{
				CommonTableExpr *cte = castNode(CommonTableExpr, lfirst(lc));

				if (strcmp(cte->ctename, rte->ctename) == 0)
					return cte;
			}

			/* shouldn't happen */
			elog(ERROR, "could not find CTE \"%s\"", rte->ctename);
		}
		levelsup--;
	}

	return GetCTEForRTE(pstate, rte, levelsup - rte->ctelevelsup);
}

/*
 * drill_down_to_base_rel
 *		Resolves the base relation from a potentially derived relation
 */
static RangeTblEntry *
drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
					   List *attnums, List **base_attnums,
					   int location, QueryStack *query_stack)
{
	RangeTblEntry *base_rte = NULL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);

				switch (rel->rd_rel->relkind)
				{
					case RELKIND_VIEW:
						base_rte = drill_down_to_base_rel_query(pstate,
																get_view_query(rel),
																attnums,
																base_attnums,
																location,
																query_stack);
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
								 errdetail_relkind_not_supported(rel->rd_rel->relkind),
								 parser_errposition(pstate, location)));
				}

				table_close(rel, AccessShareLock);
			}
			break;

		case RTE_SUBQUERY:
			base_rte = drill_down_to_base_rel_query(pstate, rte->subquery,
													attnums, base_attnums,
													location, query_stack);
			break;

		case RTE_CTE:
			{
				CommonTableExpr *cte;

				cte = find_cte_for_rte(pstate, query_stack, rte);
				if (!cte)
					elog(ERROR, "could not find CTE \"%s\" (drill_down_to_base_rel)", rte->ctename);

				if (cte->cterecursive)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving this type of relation are not supported"),
							 parser_errposition(pstate, location)));

				base_rte = drill_down_to_base_rel_query(pstate,
														castNode(Query, cte->ctequery),
														attnums,
														base_attnums,
														location,
														query_stack);
			}
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
								 errmsg("foreign key joins require direct column references, found expression"),
								 parser_errposition(pstate, location)));

					var = castNode(Var, node);

					/* Check that all columns map to the same rte */
					if (next_rtindex == 0)
						next_rtindex = var->varno;
					else if (next_rtindex != var->varno)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("all key columns must belong to the same table"),
								 parser_errposition(pstate, location)));

					next_attnums = lappend_int(next_attnums, var->varattno);
				}

				Assert(next_rtindex != 0);

				base_rte = drill_down_to_base_rel(pstate,
												  rt_fetch(next_rtindex, (query_stack ? query_stack->query->rtable : pstate->p_rtable)),
												  next_attnums,
												  base_attnums,
												  location,
												  query_stack);

			}
			break;

		case RTE_GROUP:
			{
				/*
				 * RTE_GROUP represents a GROUP BY operation. We need to map
				 * the requested columns to the underlying relation being
				 * grouped. The GROUP BY expressions should be available in
				 * rte->groupexprs.
				 */
				int			next_rtindex = 0;
				List	   *next_attnums = NIL;
				ListCell   *lc;

				/*
				 * For RTE_GROUP, we need to find which base relation the
				 * requested columns come from. The groupexprs list should
				 * contain Vars pointing to the underlying relation.
				 */
				foreach(lc, attnums)
				{
					int			attno = lfirst_int(lc);
					Var		   *var = NULL;
					Node	   *expr;

					/*
					 * For RTE_GROUP, the attribute number corresponds to the
					 * position in the groupexprs list (1-based). Get the
					 * expression at that position.
					 */
					if (attno > 0 && attno <= list_length(rte->groupexprs))
					{
						expr = (Node *) list_nth(rte->groupexprs, attno - 1);

						if (IsA(expr, Var))
						{
							var = (Var *) expr;
						}
					}

					if (!var)
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("GROUP BY column %d is not a simple column reference", attno),
								 parser_errposition(pstate, location)));

					/* Check that all columns map to the same rte */
					if (next_rtindex == 0)
						next_rtindex = var->varno;
					else if (next_rtindex != var->varno)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("all key columns must belong to the same table"),
								 parser_errposition(pstate, location)));

					next_attnums = lappend_int(next_attnums, var->varattno);
				}

				if (next_rtindex == 0)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("no valid columns found in GROUP BY for foreign key join"),
							 parser_errposition(pstate, location)));

				base_rte = drill_down_to_base_rel(pstate,
												  rt_fetch(next_rtindex, (query_stack ? query_stack->query->rtable : pstate->p_rtable)),
												  next_attnums,
												  base_attnums,
												  location,
												  query_stack);

			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));
	}

	return base_rte;
}

/*
 * drill_down_to_base_rel_query
 *		Resolves the base relation from a query
 */
static RangeTblEntry *
drill_down_to_base_rel_query(ParseState *pstate, Query *query,
							 List *attnums, List **base_attnums,
							 int location, QueryStack *query_stack)
{
	int			next_rtindex = 0;
	List	   *next_attnums = NIL;
	ListCell   *lc;
	RangeTblEntry *result;
	QueryStack	new_stack = {.parent=query_stack, .query=query};

	if (query->setOperations != NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving set operations are not supported"),
				 parser_errposition(pstate, location)));
	}

	/*
	 * We allow GROUP BY if the grouping preserves uniqueness, but we check
	 * this in analyze_join_tree where we build uniqueness preservation info.
	 *
	 * DISTINCT is still fatal here – once duplicates are removed there is
	 * no way to re-establish determinism for FK-checking.
	 */
	if (query->commandType != CMD_SELECT ||
		query->distinctClause ||
		query->groupingSets ||
		query->hasTargetSRFs)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins not supported for these relations"),
				 parser_errposition(pstate, location)));
	}

	foreach(lc, attnums)
	{
		int			attno = lfirst_int(lc);
		TargetEntry *matching_tle;
		Var		   *var;

		matching_tle = list_nth(query->targetList, attno - 1);

		if (!IsA(matching_tle->expr, Var))
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("target entry \"%s\" is an expression, not a direct column reference",
							matching_tle->resname),
					 parser_errposition(pstate, location)));
		}

		var = castNode(Var, matching_tle->expr);

		/* Check that all columns map to the same rte */
		if (next_rtindex == 0)
			next_rtindex = var->varno;
		else if (next_rtindex != var->varno)
		{
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("all key columns must belong to the same table"),
					 parser_errposition(pstate,
										exprLocation((Node *) matching_tle->expr))));
		}

		next_attnums = lappend_int(next_attnums, var->varattno);
	}

	Assert(next_rtindex != 0);

	result = drill_down_to_base_rel(pstate, rt_fetch(next_rtindex, query->rtable), next_attnums,
									base_attnums, location, &new_stack);

	return result;
}



/*
 * check_group_by_preserves_uniqueness
 *		Check if a GROUP BY clause preserves uniqueness by verifying that
 *		the grouped columns cover a unique index on the underlying base table.
 *
 *		On success, sets *uniqueness_preservation to {base_rteid} (GROUP BY
 *		collapses rows across all tables, so only one table can remain unique).
 *		If any grouped column is nullable, appends base_rteid to
 *		*null_injected_keys because GROUP BY collapses all NULLs into one
 *		group, injecting a NULL key value that may not exist in the base data.
 *
 *		Caller guarantees query->groupClause is non-empty.  PG17+ guarantees
 *		query->hasGroupRTE is set, meaning an RTE_GROUP entry exists in the
 *		range table whose groupexprs list contains the actual GROUP BY
 *		expressions (Vars referencing the underlying base relation).
 */
static bool
check_group_by_preserves_uniqueness(Query *query, List **uniqueness_preservation,
									List **null_injected_keys)
{
	RangeTblEntry *group_rte = NULL;
	RangeTblEntry *base_rte;
	Index		underlying_varno = 0;
	Bitmapset  *columns = NULL;
	ListCell   *lc;
	Relation	rel;
	bool		all_cols_not_null;
	bool		result;

	Assert(query->groupClause);
	Assert(query->hasGroupRTE);
	Assert(null_injected_keys != NULL);

	/* Find the RTE_GROUP entry in the range table */
	foreach(lc, query->rtable)
	{
		RangeTblEntry *rte = lfirst_node(RangeTblEntry, lc);

		if (rte->rtekind == RTE_GROUP)
		{
			group_rte = rte;
			break;
		}
	}
	Assert(group_rte != NULL);

	/*
	 * Walk the RTE_GROUP's groupexprs to find the underlying base relation
	 * and collect the set of grouped column attnos.  Every expression must be
	 * a simple Var referencing the same relation.
	 */
	foreach(lc, group_rte->groupexprs)
	{
		Node	   *expr = (Node *) lfirst(lc);
		Var		   *v;

		if (!IsA(expr, Var))
			return false;

		v = (Var *) expr;

		if (underlying_varno == 0)
			underlying_varno = v->varno;
		else if (underlying_varno != v->varno)
			return false;

		columns = bms_add_member(columns, v->varattno);
	}

	/* The underlying RTE must be a base relation */
	base_rte = rt_fetch(underlying_varno, query->rtable);
	if (base_rte->rtekind != RTE_RELATION)
	{
		bms_free(columns);
		return false;
	}

	/* Check if the grouped columns cover a unique index */
	rel = table_open(base_rte->relid, AccessShareLock);
	result = check_unique_index_covers_columns(rel, columns, &all_cols_not_null);
	table_close(rel, AccessShareLock);
	bms_free(columns);

	if (result)
	{
		*uniqueness_preservation = list_make1(base_rte->rteid);

		/*
		 * If any grouped column is nullable, record it so the caller can
		 * reject using this relation as the referenced side of an FK join.
		 */
		if (!all_cols_not_null &&
			!list_member(*null_injected_keys, base_rte->rteid))
			*null_injected_keys = lappend(*null_injected_keys, base_rte->rteid);
	}

	return result;
}

/*
 * check_unique_index_covers_columns
 *		Check if the given columns are covered by a unique index on the relation.
 *		When a matching index is found, *all_cols_not_null is set to true when
 *		every column in the GROUP BY list has a NOT NULL constraint (or the
 *		matching index is a primary key, which implies NOT NULL).
 */
static bool
check_unique_index_covers_columns(Relation rel, Bitmapset *columns,
								  bool *all_cols_not_null)
{
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		result = false;

	Assert(all_cols_not_null != NULL);

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

		/* Skip if not a unique index */
		if (!indexForm->indisunique)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		/* Build a bitmapset of the index columns */
		nindexattrs = indexForm->indnatts;
		for (int j = 0; j < nindexattrs; j++)
		{
			AttrNumber	attnum = indexForm->indkey.values[j];

			if (attnum > 0)		/* skip expressions */
				index_cols = bms_add_member(index_cols, attnum);
		}

		/* Check if the grouped columns cover all unique index columns */
		if (bms_is_subset(index_cols, columns))
		{
			result = true;

			/*
			 * Report whether all grouped columns are NOT NULL.  Primary
			 * key columns are always NOT NULL; otherwise check attnotnull
			 * for each column in the GROUP BY list.
			 */
			if (indexForm->indisprimary)
			{
				*all_cols_not_null = true;
			}
			else
			{
				int			attnum;
				TupleDesc	tupdesc = RelationGetDescr(rel);

				*all_cols_not_null = true;
				attnum = -1;
				while ((attnum = bms_next_member(columns, attnum)) >= 0)
				{
					if (!TupleDescAttr(tupdesc, attnum - 1)->attnotnull)
					{
						*all_cols_not_null = false;
						break;
					}
				}
			}

			index_close(indexRel, AccessShareLock);
			bms_free(index_cols);
			break;
		}

		index_close(indexRel, AccessShareLock);
		bms_free(index_cols);
	}

	list_free(indexoidlist);

	return result;
}

/*
 * is_referencing_cols_unique
 *      Determines if the foreign key columns in the referencing table
 *      are guaranteed to be unique by a constraint or index.
 *
 * This function checks if the columns forming the foreign key in the referencing
 * table are covered by a unique index or primary key constraint, which would
 * guarantee their uniqueness.
 */
static bool
is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums)
{
	Relation	rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		result = false;
	int			natts;

	/* Get number of attributes for validation */
	natts = list_length(referencing_base_attnums);

	/* Open the relation */
	rel = table_open(referencing_relid, AccessShareLock);

	/* Get a list of index OIDs for this relation */
	indexoidlist = RelationGetIndexList(rel);

	/* Scan through the indexes */
	foreach(indexoidscan, indexoidlist)
	{
		Oid			indexoid = lfirst_oid(indexoidscan);
		Relation	indexRel;
		Form_pg_index indexForm;
		int			nindexattrs;
		bool		matches = true;
		ListCell   *lc;

		/* Open the index relation */
		indexRel = index_open(indexoid, AccessShareLock);
		indexForm = indexRel->rd_index;

		/* Skip if not a unique index */
		if (!indexForm->indisunique)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		/* For uniqueness to apply, all our columns must be in the index's key */
		nindexattrs = indexForm->indnatts;

		/* Must have same number of attributes */
		if (natts != nindexattrs)
		{
			index_close(indexRel, AccessShareLock);
			continue;
		}

		/* Check if our columns match the index columns (in any order) */
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
 *      Determines if all foreign key columns in the referencing table
 *      have NOT NULL constraints.
 *
 * This function checks if each column in the foreign key has a NOT NULL
 * constraint, which is important for correct join semantics and for
 * preserving functional dependencies across joins.
 *
 * If notNullConstraints is not NULL, the function also collects the OIDs
 * of the NOT NULL constraints for each column that has one. This is used
 * to establish dependencies on those constraints.
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

	/* Open the relation to get its tuple descriptor */
	rel = table_open(referencing_relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	/* Check each column for NOT NULL constraint */
	foreach(lc, referencing_base_attnums)
	{
		AttrNumber	attnum = lfirst_int(lc);
		Form_pg_attribute attr;

		/* Get attribute info - attnum is 1-based, array is 0-based */
		attr = TupleDescAttr(tupdesc, attnum - 1);

		/* Check if the column allows nulls */
		if (!attr->attnotnull)
		{
			all_not_null = false;
			break;
		}

		/*
		 * If requested, look up the NOT NULL constraint OID for this column.
		 * We only do this if all columns so far have been NOT NULL.
		 */
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

	/* Close the relation */
	table_close(rel, AccessShareLock);

	/* Return the collected constraint OIDs if all columns are NOT NULL */
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

/*
 * update_uniqueness_preservation
 *      Updates the uniqueness preservation properties for a foreign key join
 *
 * This function calculates the uniqueness preservation for a join based on
 * the uniqueness preservation properties of the input relations and the
 * uniqueness of the foreign key columns.
 *
 * Uniqueness preservation is propagated from the referencing relation, and
 * if both the foreign key columns form a unique key, and the referencing
 * base table preserves uniqueness, then uniqueness preservation
 * from the referenced relation is also added.
 */
static List *
update_uniqueness_preservation(List *referencing_uniqueness_preservation,
							   List *referenced_uniqueness_preservation,
							   RTEId *referencing_id,
							   bool fk_cols_unique)
{
	List	   *result = NIL;
	bool		referencing_preserves_uniqueness;

	/* Start with uniqueness preservation from the referencing relation */
	if (referencing_uniqueness_preservation)
		result = list_copy(referencing_uniqueness_preservation);

	referencing_preserves_uniqueness = list_member(referencing_uniqueness_preservation, referencing_id);

	if (fk_cols_unique && referencing_preserves_uniqueness && referenced_uniqueness_preservation)
		result = list_concat(result, referenced_uniqueness_preservation);

	return result;
}

/*
 * update_row_preserving
 *      Compute the row-preserving set (R) and NOT NULL FK chain set (C)
 *      produced by a single foreign key join, given the R and C from each
 *      input side.
 *
 * Three properties are tracked through the join tree (U is handled
 * separately by update_uniqueness_preservation):
 *
 *   U (uniqueness_preservation) - tables whose PK/UNIQUE uniqueness is
 *       preserved through the join tree.
 *   R (row_preserving) - tables whose complete set of rows is guaranteed
 *       to appear in the join result.  Stored as a plain List of RTEId*.
 *   C (notnull_fk_chains) - pairs (A, B) with A != B meaning "A is
 *       row-preserving and reaches B through NOT NULL FK joins".  Stored
 *       as a flat List of alternating RTEId* pairs.
 *
 * Both result_chains and result_row_preserving are written as out-parameters.
 */
static void
update_row_preserving(List *referencing_chains,
					  List *referencing_row_preserving,
					  RTEId *referencing_id,
					  List *referenced_chains,
					  List *referenced_row_preserving,
					  RTEId *referenced_id,
					  bool fk_cols_not_null,
					  JoinType join_type,
					  ForeignKeyDirection fk_dir,
					  List **result_chains,
					  List **result_row_preserving,
					  List *referencing_null_injected_keys,
					  List *referenced_null_injected_keys,
					  List **result_null_injected_keys)
{
	List	   *result_C = NIL;
	List	   *result_R = NIL;
	List	   *result_N = NIL;
	List	   *anchor_set = NIL;
	List	   *target_set = NIL;
	ListCell   *lc_anchor;
	ListCell   *lc_target;
	bool		preserves_referencing;
	bool		preserves_referenced;

	/* Phase 1: Outer Join Preservation */
	preserves_referencing = (join_type == JOIN_FULL ||
							 (fk_dir == FKDIR_FROM && join_type == JOIN_LEFT) ||
							 (fk_dir == FKDIR_TO && join_type == JOIN_RIGHT));

	preserves_referenced = (join_type == JOIN_FULL ||
							(fk_dir == FKDIR_TO && join_type == JOIN_LEFT) ||
							(fk_dir == FKDIR_FROM && join_type == JOIN_RIGHT));

	if (preserves_referencing)
	{
		result_R = list_concat(result_R, referencing_row_preserving);
		result_C = list_concat(result_C, referencing_chains);
	}

	if (preserves_referenced)
	{
		result_R = list_concat(result_R, referenced_row_preserving);
		result_C = list_concat(result_C, referenced_chains);
	}

	/*
	 * Phase 1b: Null-injected-keys (NULL introduction) tracking.
	 *
	 * Start by propagating each side's null_injected_keys.  Then clear
	 * entries for tables whose ghost rows are filtered by the join's
	 * inner side: the FK equi-join condition ensures NULL join columns
	 * never match, so ghost rows for the specific base table in the
	 * join condition are eliminated.  Only that table is cleared —
	 * other tables in null_injected_keys may have NULLs in non-join columns
	 * and their ghost rows can survive.
	 *
	 * Finally, the preserved (outer) side of an outer join may
	 * introduce NEW ghost rows on the inner side.  When all FK columns
	 * are NOT NULL the FK guarantee ensures every referencing row
	 * matches a referenced row, so no ghost rows are produced on the
	 * referenced side.
	 */
	result_N = list_concat(list_copy(referencing_null_injected_keys),
							list_copy(referenced_null_injected_keys));

	/* Clear: inner side's ghost rows filtered by FK equi-join */
	if (!preserves_referencing)
		result_N = list_delete(result_N, referencing_id);
	if (!preserves_referenced)
		result_N = list_delete(result_N, referenced_id);

	/* Add: preserved side may introduce new ghost rows on inner side */
	if (preserves_referenced &&
		!list_member(result_N, referencing_id))
		result_N = lappend(result_N, referencing_id);
	if (preserves_referencing &&
		!fk_cols_not_null &&
		!list_member(result_N, referenced_id))
		result_N = lappend(result_N, referenced_id);

	*result_null_injected_keys = result_N;

	/*
	 * We can only derive new chains across the FK relationship when the FK
	 * columns are NOT NULL and the referenced side is row-preserving.  If
	 * the FK columns are nullable, a referencing row with NULL FK values
	 * won't match any referenced row.  If the referenced side is filtered
	 * (not row-preserving), FK values might not find a match even when
	 * non-null.
	 */
	if (!fk_cols_not_null ||
		!list_member(referenced_row_preserving, referenced_id))
	{
		*result_chains = result_C;
		*result_row_preserving = result_R;
		return;
	}

	/*
	 * At this point the FK columns are NOT NULL and the referenced relation
	 * preserves all rows.  Because every non-null FK value must find a match
	 * in the referenced relation (that's what a foreign key constraint
	 * guarantees), and we know the FK columns cannot be null, every
	 * referencing row will successfully join.  This means every row that
	 * reaches referencing_id from earlier in the join tree is preserved
	 * through this join as well.
	 *
	 * Build anchor_set: the set of tables that reach referencing_id via
	 * chains, plus referencing_id itself if it is row-preserving.
	 */
	for (int i = 0; i < list_length(referencing_chains); i += 2)
	{
		RTEId	   *det = list_nth(referencing_chains, i);
		RTEId	   *dep = list_nth(referencing_chains, i + 1);

		if (equal(dep, referencing_id))
			anchor_set = lappend(anchor_set, det);
	}

	if (list_member(referencing_row_preserving, referencing_id))
		anchor_set = lappend(anchor_set, referencing_id);

	/* Precompute target_set = {p} ∪ { Z : (p, Z) ∈ C_p } for step 3c */
	target_set = list_make1(referenced_id);
	for (int i = 0; i < list_length(referenced_chains); i += 2)
	{
		RTEId	   *det = list_nth(referenced_chains, i);
		RTEId	   *dep = list_nth(referenced_chains, i + 1);

		if (equal(det, referenced_id))
			target_set = lappend(target_set, dep);
	}

	/*
	 * R'_inherit: propagate row-preserving status for anchor_set members.
	 * C'_inherit: propagate chains whose determinant is in anchor_set.
	 *
	 * Skip if an outer join already copied them above.
	 *
	 * Every member of anchor_set is guaranteed to be in R_f by the
	 * invariant on C: each (A, f) pair in C_f implies A ∈ R_f, and f
	 * is only added to anchor_set when f ∈ R_f (explicit guard above).
	 */
	if (!preserves_referencing)
	{
		/* R'_inherit = anchor_set */
		result_R = list_concat(result_R, list_copy(anchor_set));

		/* C'_inherit = { (A, Y) ∈ C_f : A ∈ anchor_set } */
		for (int i = 0; i < list_length(referencing_chains); i += 2)
		{
			RTEId	   *det = list_nth(referencing_chains, i);
			RTEId	   *dep = list_nth(referencing_chains, i + 1);

			if (list_member(anchor_set, det))
			{
				result_C = lappend(result_C, det);
				result_C = lappend(result_C, dep);
			}
		}
	}

	/*
	 * C'_extend = { (A, Y) : A ∈ anchor_set, Y ∈ target_set }
	 *
	 * Each table in anchor_set reaches referenced_id (guaranteed
	 * row-preserving by the early return above), and transitively reaches
	 * everything referenced_id reaches.  Form the cross product.
	 */
	foreach(lc_anchor, anchor_set)
	{
		RTEId	   *a = (RTEId *) lfirst(lc_anchor);

		foreach(lc_target, target_set)
		{
			result_C = lappend(result_C, a);
			result_C = lappend(result_C, (RTEId *) lfirst(lc_target));
		}
	}

	*result_chains = result_C;
	*result_row_preserving = result_R;
}
