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

static Node *build_fk_join_on_clause(ParseState *pstate,
									 ParseNamespaceColumn *l_nscols, List *l_attnums,
									 ParseNamespaceColumn *r_nscols, List *r_attnums);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid,
							 List *referencing_attnums, List *referenced_attnums);
static char *column_list_to_string(const List *columns);
static RangeTblEntry *drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
											 List *attnos, List **base_attnums,
  int location);
static CommonTableExpr *find_cte_for_rte(ParseState *pstate, Query *query,
                                                                          RangeTblEntry *rte);
static RangeTblEntry *drill_down_to_base_rel_query(ParseState *pstate, Query *query,
												   List *attnos, List **base_attnums,
												   int location);
static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums);
static List *update_uniqueness_preservation(List *referencing_uniqueness_preservation,
											List *referenced_uniqueness_preservation,
											bool fk_cols_unique);
static List *update_functional_dependencies(List *referencing_fds,
											RTEId *referencing_id,
											List *referenced_fds,
											RTEId *referenced_id,
											bool fk_cols_not_null,
											JoinType join_type,
											ForeignKeyDirection fk_dir);
static void analyze_join_tree(ParseState *pstate, Node *n,
							  Query *query,
							  RTEId *rte_id,
							  List **uniqueness_preservation,
							  List **functional_dependencies,
							  bool *found,
							  int location);

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
	List	   *referencing_functional_dependencies = NIL;
	List	   *referenced_uniqueness_preservation = NIL;
	List	   *referenced_functional_dependencies = NIL;
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
	bool		found_fd = false;
	bool		referencing_found = false;
	bool		referenced_found = false;

	foreach(lc, l_namespace)
	{
		ParseNamespaceItem *nsi = (ParseNamespaceItem *) lfirst(lc);

		if (!nsi->p_rel_visible)
			continue;

		Assert(nsi->p_names->aliasname != NULL);
		if (strcmp(nsi->p_names->aliasname, fkjn->refAlias) == 0)
		{
			Assert(other_rel == NULL);
			other_rel = nsi;
		}
	}

	if (other_rel == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_TABLE),
				 errmsg("table reference \"%s\" not found", fkjn->refAlias),
				 parser_errposition(pstate, fkjn->location)));

	if (list_length(fkjn->refCols) != list_length(fkjn->localCols))
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("number of referencing and referenced columns must be the same"),
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
												  fkjn->location);
	base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
												 referenced_attnums,
												 &referenced_base_attnums,
												 fkjn->location);

	referencing_relid = base_referencing_rte->relid;
	referenced_relid = base_referenced_rte->relid;
	referenced_id = base_referenced_rte->rteid;

	Assert(referencing_relid != InvalidOid && referenced_relid != InvalidOid);

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

	analyze_join_tree(pstate, referencing_arg, NULL, referencing_rte->rteid, &referencing_uniqueness_preservation, &referencing_functional_dependencies, &referencing_found, fkjn->location);
	analyze_join_tree(pstate, referenced_arg, NULL, referenced_rte->rteid, &referenced_uniqueness_preservation, &referenced_functional_dependencies, &referenced_found, fkjn->location);

	/* Check uniqueness preservation */
	if (!list_member(referenced_uniqueness_preservation, referenced_id))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("referenced relation does not preserve uniqueness of keys"),
				 parser_errposition(pstate, fkjn->location)));
	}

	/*
	 * Check functional dependencies - looking for (referenced_id,
	 * referenced_id) pairs
	 */
	for (int i = 0; i < list_length(referenced_functional_dependencies); i += 2)
	{
		RTEId	   *fd_dep = (RTEId *) list_nth(referenced_functional_dependencies, i);
		RTEId	   *fd_dcy = (RTEId *) list_nth(referenced_functional_dependencies, i + 1);

		if (equal(fd_dep, referenced_id) && equal(fd_dcy, referenced_id))
		{
			found_fd = true;
			break;
		}
	}

	if (!found_fd)
	{
		/*
		 * This check ensures that the referenced relation is not filtered
		 * (e.g., by WHERE, LIMIT, OFFSET, HAVING, RLS). Foreign key joins
		 * require the referenced side to represent the complete set of rows
		 * from the underlying table(s). The presence of a functional
		 * dependency (referenced_id, referenced_id) indicates this row
		 * preservation property.
		 */
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("referenced relation does not preserve all rows"),
				 parser_errposition(pstate, fkjn->location)));
	}

	join->quals = build_fk_join_on_clause(pstate, referencing_rel->p_nscolumns, referencing_attnums, referenced_rel->p_nscolumns, referenced_attnums);

	fkjn_node = makeNode(ForeignKeyJoinNode);
	fkjn_node->fkdir = fkjn->fkdir;
	fkjn_node->referencingVarno = referencing_rel->p_rtindex;
	fkjn_node->referencingAttnums = referencing_attnums;
	fkjn_node->referencedVarno = referenced_rel->p_rtindex;
	fkjn_node->referencedAttnums = referenced_attnums;
	fkjn_node->constraint = fkoid;

	join->fkJoin = (Node *) fkjn_node;
}

static void
analyze_join_tree(ParseState *pstate, Node *n,
				  Query *query,
				  RTEId *rte_id,
				  List **uniqueness_preservation,
				  List **functional_dependencies,
				  bool *found,
				  int location)
{
	RangeTblEntry *rte;
	Query	   *inner_query = NULL;
	List	   *referencing_uniqueness_preservation = NIL;
	List	   *referencing_functional_dependencies = NIL;
	List	   *referenced_uniqueness_preservation = NIL;
	List	   *referenced_functional_dependencies = NIL;
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
				ForeignKeyJoinNode *fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);
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

				analyze_join_tree(pstate, referencing_arg, query, rte_id, &referencing_uniqueness_preservation, &referencing_functional_dependencies, &referencing_found, location);
				if (referencing_found || equal(referencing_rte->rteid, rte_id))
				{
					*found = true;
					*uniqueness_preservation = referencing_uniqueness_preservation;
					*functional_dependencies = referencing_functional_dependencies;
					break;
				}

				analyze_join_tree(pstate, referenced_arg, query, rte_id, &referenced_uniqueness_preservation, &referenced_functional_dependencies, &referenced_found, location);
				if (referenced_found || equal(referenced_rte->rteid, rte_id))
				{
					*found = true;
					*uniqueness_preservation = referenced_uniqueness_preservation;
					*functional_dependencies = referenced_functional_dependencies;
					break;
				}

				base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
															  fkjn->referencingAttnums,
															  &referencing_base_attnums,
															  location);
				base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
															 fkjn->referencedAttnums,
															 &referenced_base_attnums,
															 location);

				referencing_relid = base_referencing_rte->relid;
				referencing_id = base_referencing_rte->rteid;
				referenced_id = base_referenced_rte->rteid;

				fk_cols_unique = is_referencing_cols_unique(referencing_relid, referencing_base_attnums);
				fk_cols_not_null = is_referencing_cols_not_null(referencing_relid, referencing_base_attnums);

				*uniqueness_preservation = update_uniqueness_preservation(
																		  referencing_uniqueness_preservation,
																		  referenced_uniqueness_preservation,
																		  fk_cols_unique
					);
				*functional_dependencies = update_functional_dependencies(
																		  referencing_functional_dependencies,
																		  referencing_id,
																		  referenced_functional_dependencies,
																		  referenced_id,
																		  fk_cols_not_null,
																		  join->jointype,
																		  fkjn->fkdir
					);

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
								 * Check if filtered, either by RLS or
								 * WHERE/OFFSET/LIMIT
								 */
								if (!rel->rd_rel->relrowsecurity &&
									(!query || (!query->jointree->quals &&
												!query->limitOffset &&
												!query->limitCount)))
									*functional_dependencies = list_make2(rte->rteid, rte->rteid);
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

						cte = find_cte_for_rte(pstate, query, rte);
						if (!cte)
							elog(ERROR, "could not find CTE \"%s\"", rte->ctename);

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
						analyze_join_tree(pstate,
										  (Node *) linitial(inner_query->jointree->fromlist),
										  inner_query, rte_id, uniqueness_preservation, functional_dependencies, found, location);
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
 *              Locate the CTE referenced by an RTE either in the supplied Query
 *              or, failing that, in the ParseState's CTE namespace.
 */
static CommonTableExpr *
find_cte_for_rte(ParseState *pstate, Query *query, RangeTblEntry *rte)
{
        ListCell   *lc;
        Index           levelsup;

        Assert(rte->rtekind == RTE_CTE);

        if (query != NULL && rte->ctelevelsup == 0)
        {
                foreach(lc, query->cteList)
                {
                        CommonTableExpr *cte = (CommonTableExpr *) lfirst(lc);

                        if (strcmp(cte->ctename, rte->ctename) == 0)
                                return cte;
                }
        }

        if (pstate != NULL)
        {
                CommonTableExpr *cte = scanNameSpaceForCTE(pstate, rte->ctename,
                                                                                         &levelsup);

                if (cte && levelsup == rte->ctelevelsup)
                        return cte;
        }

        return NULL;
}

/*
 * drill_down_to_base_rel
 *		Resolves the base relation from a potentially derived relation
 */
static RangeTblEntry *
drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
					   List *attnums, List **base_attnums,
					   int location)
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
																location);
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
													location);
			break;

		case RTE_CTE:
			{
				CommonTableExpr *cte;

				cte = find_cte_for_rte(pstate, NULL, rte);
				if (!cte)
					elog(ERROR, "could not find CTE \"%s\"", rte->ctename);

				if (cte->cterecursive)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving this type of relation are not supported"),
							 parser_errposition(pstate, location)));

				base_rte = drill_down_to_base_rel_query(pstate,
							 castNode(Query, cte->ctequery),
							 attnums,
							 base_attnums,
							 location);
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
								 errmsg("key columns must all come from the same table"),
								 parser_errposition(pstate, location)));

					next_attnums = lappend_int(next_attnums, var->varattno);
				}

				Assert(next_rtindex != 0);

				base_rte = drill_down_to_base_rel(pstate,
												  rt_fetch(next_rtindex, pstate->p_rtable),
												  next_attnums,
												  base_attnums,
												  location);

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
							 int location)
{
	int			next_rtindex = 0;
	List	   *next_attnums = NIL;
	ListCell   *lc;

	if (query->setOperations != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving set operations are not supported"),
				 parser_errposition(pstate, location)));

	/* XXX: Overly aggressive disallowing */
	if (query->commandType != CMD_SELECT ||
		query->groupClause ||
		query->distinctClause ||
		query->groupingSets ||
		query->hasTargetSRFs ||
		query->havingQual)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins not supported for these relations"),
				 parser_errposition(pstate, location)));

	foreach(lc, attnums)
	{
		int			attno = lfirst_int(lc);
		TargetEntry *matching_tle;
		Var		   *var;

		matching_tle = list_nth(query->targetList, attno - 1);

		if (!IsA(matching_tle->expr, Var))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("target entry \"%s\" is an expression, not a direct column reference",
							matching_tle->resname),
					 parser_errposition(pstate, location)));

		var = castNode(Var, matching_tle->expr);

		/* Check that all columns map to the same rte */
		if (next_rtindex == 0)
			next_rtindex = var->varno;
		else if (next_rtindex != var->varno)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("key columns must all come from the same table"),
					 parser_errposition(pstate,
										exprLocation((Node *) matching_tle->expr))));

		next_attnums = lappend_int(next_attnums, var->varattno);
	}

	Assert(next_rtindex != 0);

	return drill_down_to_base_rel(pstate, rt_fetch(next_rtindex, query->rtable), next_attnums,
								  base_attnums, location);
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
 */
static bool
is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ListCell   *lc;
	bool		all_not_null = true;

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
	}

	/* Close the relation */
	table_close(rel, AccessShareLock);

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
 * if the foreign key columns form a unique key, then uniqueness preservation
 * from the referenced relation is also added.
 */
static List *
update_uniqueness_preservation(List *referencing_uniqueness_preservation,
							   List *referenced_uniqueness_preservation,
							   bool fk_cols_unique)
{
	List	   *result = NIL;

	/* Start with uniqueness preservation from the referencing relation */
	if (referencing_uniqueness_preservation)
	{
		result = list_copy(referencing_uniqueness_preservation);
	}

	/*
	 * If the foreign key columns form a unique key, we can also preserve
	 * uniqueness from the referenced relation
	 */
	if (fk_cols_unique && referenced_uniqueness_preservation)
	{
		result = list_concat(result, referenced_uniqueness_preservation);
	}

	return result;
}

/*
 * update_functional_dependencies
 *      Updates the functional dependencies for a foreign key join
 */
static List *
update_functional_dependencies(List *referencing_fds,
							   RTEId *referencing_id,
							   List *referenced_fds,
							   RTEId *referenced_id,
							   bool fk_cols_not_null,
							   JoinType join_type,
							   ForeignKeyDirection fk_dir)
{
	List	   *result = NIL;
	bool		referenced_has_self_dep = false;
	bool		referencing_preserved_due_to_outer_join = false;

	/*
	 * Step 1: Add functional dependencies from the referencing relation when
	 * an outer join preserves the referencing relation's tuples.
	 */
	if ((fk_dir == FKDIR_FROM && join_type == JOIN_LEFT) ||
		(fk_dir == FKDIR_TO && join_type == JOIN_RIGHT) ||
		join_type == JOIN_FULL)
	{
		result = list_concat(result, referencing_fds);
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
		result = list_concat(result, referenced_fds);
	}

	/*
	 * In the following steps we handle functional dependencies introduced by
	 * inner joins. Even for outer joins, we must compute these dependencies
	 * to predict which relations will preserve all their rows in subsequent
	 * joins. Relations that appear as determinants in functional dependencies
	 * (det, X) are guaranteed to preserve all their rows.
	 */

	/*
	 * Step 3: If any foreign key column permits NULL values, we cannot
	 * guarantee at compile time that all rows will be preserved in an inner
	 * foreign key join. In this case, we cannot derive additional functional
	 * dependencies and cannot infer which other relations will preserve all
	 * their rows.
	 */
	if (!fk_cols_not_null)
		return result;

	/*
	 * Step 4: Verify that the referenced relation preserves all its rows -
	 * indicated by a self-dependency (referenced_id → referenced_id). This
	 * self-dependency confirms that the referenced relation is a determinant
	 * relation that preserves all its rows. Without this guarantee, we cannot
	 * derive additional functional dependencies.
	 */
	for (int i = 0; i < list_length(referenced_fds); i += 2)
	{
		RTEId	   *det = list_nth(referenced_fds, i);
		RTEId	   *dep = list_nth(referenced_fds, i + 1);

		if (equal(det, referenced_id) && equal(dep, referenced_id))
		{
			referenced_has_self_dep = true;
			break;
		}
	}

	if (!referenced_has_self_dep)
		return result;

	/*
	 * Step 5: Preserve inherited functional dependencies from the referencing
	 * relation. Skip if the referencing relation is already fully preserved
	 * by an outer join.
	 *
	 * At this point, we know that referencing_id will be preserved in the
	 * join. We include all functional dependencies where referencing_id
	 * appears as the dependent attribute (X → referencing_id). This
	 * maintains the property that all determinant relations (X) will continue
	 * to preserve all their rows after the join.
	 */
	if (!referencing_preserved_due_to_outer_join)
	{
		for (int i = 0; i < list_length(referencing_fds); i += 2)
		{
			RTEId	   *referencing_det = list_nth(referencing_fds, i);
			RTEId	   *referencing_dep = list_nth(referencing_fds, i + 1);

			if (equal(referencing_dep, referencing_id))
			{
				for (int j = 0; j < list_length(referencing_fds); j += 2)
				{
					RTEId	   *source_det = list_nth(referencing_fds, j);
					RTEId	   *source_dep = list_nth(referencing_fds, j + 1);

					if (equal(source_det, referencing_det))
					{
						result = lappend(result, source_det);
						result = lappend(result, source_dep);
					}
				}
			}
		}
	}

	/*
	 * Step 6: Establish transitive functional dependencies by applying the
	 * transitivity axiom across the foreign key relationship. This identifies
	 * additional relations that will preserve all their rows after the join.
	 *
	 * By the Armstrong's axioms of functional dependencies, specifically
	 * transitivity: If X → Y and Y → Z, then X → Z
	 *
	 * In our context, for each pair of dependencies: - X → referencing_id
	 * (from referencing relation) - referenced_id → Z (from referenced
	 * relation)
	 *
	 * We derive the transitive dependency: - X → Z
	 *
	 * This identifies that relation X is a determinant relation that will
	 * preserve all its rows, and it now functionally determines relation Z as
	 * well.
	 *
	 * This operation can be conceptualized as a join between two sets of
	 * dependencies:
	 *
	 * SELECT referencing_fds.det AS new_det, referenced_fds.dep AS new_dep
	 * FROM referencing_fds JOIN referenced_fds ON referencing_fds.dep =
	 * referencing_id AND referenced_fds.det = referenced_id
	 *
	 * In formal set notation: Let R = {(X, Y)} be the set of referencing
	 * functional dependencies Let S = {(A, B)} be the set of referenced
	 * functional dependencies Let r = referencing_id Let s = referenced_id
	 *
	 * The new transitive dependencies are defined as:
	 *
	 * T = {(X, B) | (X, r) ∈ R ∧ (s, B) ∈ S}
	 *
	 * The correctness of this derivation relies on the fact that
	 * referenced_id is preserved in this join (as verified in previous
	 * steps). This preservation ensures that for each value of determinant X
	 * that functionally determines referencing_id, there exists precisely one
	 * value of dependent B associated with referenced_id, thereby
	 * establishing X as a determinant relation that preserves all its rows
	 * and functionally determines B.
	 */
	for (int i = 0; i < list_length(referencing_fds); i += 2)
	{
		RTEId	   *referencing_det = list_nth(referencing_fds, i);
		RTEId	   *referencing_dep = list_nth(referencing_fds, i + 1);

		if (equal(referencing_dep, referencing_id))
		{
			for (int j = 0; j < list_length(referenced_fds); j += 2)
			{
				RTEId	   *referenced_det = list_nth(referenced_fds, j);
				RTEId	   *referenced_dep = list_nth(referenced_fds, j + 1);

				if (equal(referenced_det, referenced_id))
				{
					result = lappend(result, referencing_det);
					result = lappend(result, referenced_dep);
				}
			}
		}
	}

	return result;
}
