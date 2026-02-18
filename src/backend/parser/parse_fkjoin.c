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

static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums,
										 List **notNullConstraints);
static RTEId *analyze_join_tree(ParseState *pstate, Node *n,
								Query *query, int location,
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
	RTEId	   *preserved;
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
	 * skip the preservation check for them.
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

	/*
	 * Analyze the referencing side to validate no non-FK joins inside
	 * derived relations (the return value is discarded).
	 */
	(void) analyze_join_tree(pstate, referencing_arg, NULL,
							 fkjn->location, NULL);

	/*
	 * Check preservation of the referenced base table — only needed for
	 * derived tables.  Base tables trivially preserve themselves.
	 */
	if (!referenced_is_base_table)
	{
		preserved = analyze_join_tree(pstate, referenced_arg, NULL,
									  fkjn->location, NULL);

		if (preserved == NULL || !equal(preserved, referenced_id))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("foreign key join violation"),
					 errdetail("referenced relation does not preserve the referenced base table"),
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

/*
 * analyze_join_tree
 *		Walk the join tree inside a derived relation and return the RTEId
 *		of the single base table that is "preserved" through the tree,
 *		or NULL if no single table can be guaranteed preserved.
 *
 * A base table is preserved when it is unfiltered and its uniqueness and
 * row completeness survive through the chain of FK joins.  This is a
 * simplified algorithm that tracks a single scalar rather than separate
 * sets for uniqueness, row preservation, null preservation, and FK chains.
 */
static RTEId *
analyze_join_tree(ParseState *pstate, Node *n,
				  Query *query, int location,
				  QueryStack *query_stack)
{
	RangeTblEntry *rte;
	Query	   *inner_query = NULL;

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
				RTEId	   *referencing_preserved;
				RTEId	   *referenced_preserved;
				List	   *referencing_base_attnums;
				List	   *referenced_base_attnums;
				RangeTblEntry *base_referencing_rte;
				RangeTblEntry *base_referenced_rte;
				RTEId	   *referencing_id;
				RTEId	   *referenced_id;
				bool		fk_cols_unique;
				bool		fk_cols_not_null;
				bool		P_p;
				bool		P_f;

				if (join->fkJoin == NULL)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving non-foreign-key joins are not supported"),
							 parser_errposition(pstate, location)));

				fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);

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

				referencing_preserved = analyze_join_tree(pstate, referencing_arg,
														  query, location,
														  query_stack);
				referenced_preserved = analyze_join_tree(pstate, referenced_arg,
														 query, location,
														 query_stack);

				base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
															  fkjn->referencingAttnums,
															  &referencing_base_attnums,
															  location, query_stack);
				base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
															 fkjn->referencedAttnums,
															 &referenced_base_attnums,
															 location, query_stack);

				referencing_id = base_referencing_rte->rteid;
				referenced_id = base_referenced_rte->rteid;

				fk_cols_unique = is_referencing_cols_unique(base_referencing_rte->relid,
														   referencing_base_attnums);
				fk_cols_not_null = is_referencing_cols_not_null(base_referencing_rte->relid,
															   referencing_base_attnums, NULL);

				P_p = (referenced_preserved != NULL &&
					   equal(referenced_preserved, referenced_id));
				P_f = (referencing_preserved != NULL &&
					   equal(referencing_preserved, referencing_id));

				/* Case 1: referenced side not preserved */
				if (!P_p)
					return NULL;

				/* Cases 2-3: INNER JOIN */
				if (join->jointype == JOIN_INNER)
					return fk_cols_not_null ? referencing_preserved : NULL;

				/* Cases 4-7: LEFT JOIN */
				if (join->jointype == JOIN_LEFT)
				{
					if (fkjn->fkdir == FKDIR_FROM)
						return referencing_preserved;		/* Case 4 */
					if (P_f && fk_cols_unique)
						return referenced_preserved;		/* Case 5 */
					return NULL;							/* Cases 6, 7 */
				}

				/* Cases 8-11: RIGHT JOIN */
				if (join->jointype == JOIN_RIGHT)
				{
					if (fkjn->fkdir == FKDIR_TO)
						return referencing_preserved;		/* Case 8 */
					if (P_f && fk_cols_unique)
						return referenced_preserved;		/* Case 9 */
					return NULL;							/* Cases 10, 11 */
				}

				/* Case 12: FULL JOIN */
				return NULL;
			}

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
								/*
								 * Base table: preserved if not filtered by
								 * WHERE/OFFSET/LIMIT/HAVING.
								 */
								table_close(rel, AccessShareLock);
								if (!query || (!query->jointree->quals &&
											   !query->limitOffset &&
											   !query->limitCount &&
											   !query->havingQual))
									return rte->rteid;
								return NULL;
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
						RTEId	   *result;
						QueryStack	new_stack = {.parent=query_stack, .query=inner_query};

						result = analyze_join_tree(pstate,
												   (Node *) linitial(inner_query->jointree->fromlist),
												   inner_query, location,
												   &new_stack);

						/*
						 * GROUP BY destroys preservation — the simplified
						 * algorithm cannot verify that grouping preserves
						 * uniqueness.
						 */
						if (inner_query->groupClause)
							return NULL;

						return result;
					}
				}

				return NULL;
			}

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported node type in foreign key join traversal"),
					 parser_errposition(pstate, location)));
			return NULL;
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
	 * GROUP BY is allowed here because drill_down_to_base_rel_query only
	 * traces column references to find the underlying base table — actual
	 * preservation checking happens in analyze_join_tree, which returns
	 * NULL for queries with GROUP BY.
	 *
	 * DISTINCT is still fatal — once duplicates are removed there is
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
 * is_referencing_cols_unique
 *      Determines if the foreign key columns in the referencing table
 *      are guaranteed to be unique by a constraint or index.
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
 * If notNullConstraints is not NULL, the function also collects the OIDs
 * of the NOT NULL constraints for each column that has one.
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
