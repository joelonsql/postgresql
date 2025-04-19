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

/* Static variable for global base relation indexing */
static Index next_baserelindex = 1;

static Node *build_fk_join_on_clause(ParseState *pstate,
									 ParseNamespaceColumn *l_nscols, List *l_attnums,
									 ParseNamespaceColumn *r_nscols, List *r_attnums);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid,
							 List *referencing_attnums, List *referenced_attnums);
static char *column_list_to_string(const List *columns);
static RangeTblEntry *drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte, int rtindex,
											 List *attnos, List **base_attnums,
											 int *base_rte_id,
											 List **uniqueness_preservation, List **functional_dependencies,
											 int location);
static RangeTblEntry *drill_down_to_base_rel_query(ParseState *pstate, Query *query,
												   List *attnos, List **base_attnums,
												   int *base_rte_id,
												   List **uniqueness_preservation, List **functional_dependencies,
												   int location);
static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_attnums);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_attnums);
static List *update_uniqueness_preservation(List *referencing_uniqueness_preservation,
											List *referenced_uniqueness_preservation,
											bool fk_cols_unique);
static List *update_functional_dependencies(List *referencing_fds,
											int referencing_id,
											List *referenced_fds,
											int referenced_id,
											bool fk_cols_not_null,
											JoinType join_type,
											ForeignKeyDirection fk_dir);
static const char *getNodeTypeName(NodeTag tag);
void traverse_node(ParseState *pstate, Node *n, ParseNamespaceItem *r_nsitem, 
                  List *l_namespace, int indentation_depth, Query *query,
                  List *track_cols);

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
	List	   *referencing_base_attnums;
	List	   *referenced_base_attnums;
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;
	List	   *referencing_uniqueness_preservation = NIL;
	List	   *referencing_functional_dependencies = NIL;
	List	   *referenced_uniqueness_preservation = NIL;
	List	   *referenced_functional_dependencies = NIL;
	Oid			referencing_relid;
	Oid			referenced_relid;
	int			referencing_id;
	int			referenced_id;
	bool		found_fd = false;
	bool		fk_cols_unique;
	bool		fk_cols_not_null;

	elog(NOTICE, "XXXXXXX transformAndValidateForeignKeyJoin");

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
	}
	else
	{
		referenced_rel = other_rel;
		referencing_rel = r_nsitem;
		referenced_cols = fkjn->refCols;
		referencing_cols = fkjn->localCols;
	}

	/* Log information about the referenced and referencing columns for debugging */
	elog(NOTICE, "referencing_cols: %s", 
		 referencing_cols ? 
		 nodeToString(referencing_cols) : "NIL");
	elog(NOTICE, "referenced_cols: %s", 
		 referenced_cols ? 
		 nodeToString(referenced_cols) : "NIL");

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

	elog(NOTICE, "=> traverse_node larg");
	traverse_node(pstate, join->larg, r_nsitem, l_namespace, 0, NULL, fkjn->refCols);
	elog(NOTICE, "<= traverse_node larg");
	elog(NOTICE, "=> traverse_node rarg");
	traverse_node(pstate, join->rarg, r_nsitem, l_namespace, 0, NULL, fkjn->localCols);
	elog(NOTICE, "<= traverse_node rarg");

	base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte, referencing_rel->p_rtindex,
												  referencing_attnums,
												  &referencing_base_attnums,
												  &referencing_id,
												  &referencing_uniqueness_preservation,
												  &referencing_functional_dependencies,
												  fkjn->location);
	base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte, referenced_rel->p_rtindex,
												 referenced_attnums,
												 &referenced_base_attnums,
												 &referenced_id,
												 &referenced_uniqueness_preservation,
												 &referenced_functional_dependencies,
												 fkjn->location);

	/* Log information about the foreign key join for debugging */
	elog(NOTICE, "referencing_id: %d", referencing_id);
	elog(NOTICE, "referencing_uniqueness_preservation: %s", 
		 referencing_uniqueness_preservation ? 
		 nodeToString(referencing_uniqueness_preservation) : "NIL");
	elog(NOTICE, "referencing_functional_dependencies: %s", 
		 referencing_functional_dependencies ? 
		 nodeToString(referencing_functional_dependencies) : "NIL");
	elog(NOTICE, "referenced_id: %d", referenced_id);
	elog(NOTICE, "referenced_uniqueness_preservation: %s", 
		 referenced_uniqueness_preservation ? 
		 nodeToString(referenced_uniqueness_preservation) : "NIL");
	elog(NOTICE, "referenced_functional_dependencies: %s", 
		 referenced_functional_dependencies ? 
		 nodeToString(referenced_functional_dependencies) : "NIL");

	referencing_relid = base_referencing_rte->relid;
	referenced_relid = base_referenced_rte->relid;

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

	/* Check uniqueness preservation */
	// FIXME
	if (false && !list_member_int(referenced_uniqueness_preservation, referenced_id))
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
		int		fd_dep = list_nth_int(referenced_functional_dependencies, i);
		int		fd_dcy = list_nth_int(referenced_functional_dependencies, i + 1);

		if (fd_dep == referenced_id && fd_dcy == referenced_id)
		{
			found_fd = true;
			break;
		}
	}

	found_fd = true; // FIXME
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

	fk_cols_unique = is_referencing_cols_unique(referencing_relid, referencing_base_attnums);
	fk_cols_not_null = is_referencing_cols_not_null(referencing_relid, referencing_base_attnums);

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
 * drill_down_to_base_rel
 *		Resolves the base relation from a potentially derived relation
 */
static RangeTblEntry *
drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte, int rtindex,
					   List *attnums, List **base_attnums,
					   int *base_rte_id,
					   List **uniqueness_preservation, List **functional_dependencies,
					   int location)
{
	RangeTblEntry *base_rte = NULL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);
				elog(NOTICE, "RTE_RELATION");
				switch (rel->rd_rel->relkind)
				{
					case RELKIND_VIEW:
						base_rte = drill_down_to_base_rel_query(pstate,
																get_view_query(rel),
																attnums,
																base_attnums,
																base_rte_id,
																uniqueness_preservation,
																functional_dependencies,
																location);
						break;

					case RELKIND_RELATION:
					case RELKIND_PARTITIONED_TABLE:
						base_rte = rte;
						*base_attnums = attnums;
						*base_rte_id = next_baserelindex++;
						*uniqueness_preservation = list_make1_int(*base_rte_id);
						if (!rel->rd_rel->relrowsecurity)
							*functional_dependencies = list_make2_int(*base_rte_id, *base_rte_id);
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
			elog(NOTICE, "RTE_SUBQUERY");
			base_rte = drill_down_to_base_rel_query(pstate, rte->subquery,
													attnums, base_attnums,
													base_rte_id,
													uniqueness_preservation,
													functional_dependencies,
													location);
			break;

		case RTE_CTE:
			{
				Index		levelsup;
				CommonTableExpr *cte = scanNameSpaceForCTE(pstate, rte->ctename, &levelsup);

				elog(NOTICE, "RTE_CTE");
				Assert(cte != NULL);

				if (cte->cterecursive)
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving this type of relation are not supported"),
							 parser_errposition(pstate, location)));

				base_rte = drill_down_to_base_rel_query(pstate,
														castNode(Query, cte->ctequery),
														attnums,
														base_attnums,
														base_rte_id,
														uniqueness_preservation,
														functional_dependencies,
														location);
			}
			break;

		case RTE_JOIN:
			{
				int			next_rtindex = 0;
				List	   *next_attnums = NIL;
				ListCell   *lc;

				elog(NOTICE, "RTE_JOIN");
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

				/* Find the JoinExpr in p_joinexprs */
				if (pstate->p_joinexprs)
				{
					JoinExpr *join_expr = NULL;
					join_expr = (JoinExpr *) list_nth(pstate->p_joinexprs, rtindex - 1);
					if (join_expr->fkJoin)
					{
						elog(NOTICE, "fkJoin is set");
						if (IsA(join_expr->fkJoin,ForeignKeyClause))
						{
							elog(NOTICE, "fkJoin is a ForeignKeyClause");
						}
						if (IsA(join_expr->fkJoin,ForeignKeyJoinNode))
						{
							elog(NOTICE, "fkJoin is a ForeignKeyJoinNode");

						ForeignKeyJoinNode *fkjn_node = (ForeignKeyJoinNode *) join_expr->fkJoin;
						
						/* Log the types of larg and rarg for debugging */
						if (join_expr->larg)
						{
							elog(NOTICE, "larg type: %d (%s)", 
								 (int)nodeTag(join_expr->larg), 
								 getNodeTypeName(nodeTag(join_expr->larg)));
						}
						else
						{
							elog(NOTICE, "larg is NULL");
						}
						
						if (join_expr->rarg)
						{
							elog(NOTICE, "rarg type: %d (%s)", 
								 (int)nodeTag(join_expr->rarg), 
								 getNodeTypeName(nodeTag(join_expr->rarg)));
						}
						else
						{
							elog(NOTICE, "rarg is NULL");
						}


						}
					}
					else
					{
						elog(NOTICE, "fkJoin is NULL");
					}
				}
				else
				{
					elog(NOTICE, "p_joinexprs is NULL");
				}

				Assert(next_rtindex != 0);
/*
	fkjn_node->uniqueness_preservation = update_uniqueness_preservation(
																		referencing_uniqueness_preservation,
																		referenced_uniqueness_preservation,
																		fk_cols_unique
		);
	fkjn_node->functional_dependencies = update_functional_dependencies(
																		referencing_functional_dependencies,
																		referencing_id,
																		referenced_functional_dependencies,
																		referenced_id,
																		fk_cols_not_null,
																		join->jointype,
																		fkjn->fkdir
		);
*/

				base_rte = drill_down_to_base_rel(pstate,
												  rt_fetch(next_rtindex, pstate->p_rtable), next_rtindex,
												  next_attnums,
												  base_attnums,
												  base_rte_id,
												  uniqueness_preservation,
												  functional_dependencies,
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
							 int *base_rte_id,
							 List **uniqueness_preservation, List **functional_dependencies,
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

	return drill_down_to_base_rel(pstate, rt_fetch(next_rtindex, query->rtable), next_rtindex, next_attnums,
								  base_attnums, base_rte_id, uniqueness_preservation, functional_dependencies, location);
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
							   int referencing_id,
							   List *referenced_fds,
							   int referenced_id,
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
		int det = list_nth_int(referenced_fds, i);
		int dep = list_nth_int(referenced_fds, i + 1);

		if (det == referenced_id && dep == referenced_id)
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
			int referencing_det = list_nth_int(referencing_fds, i);
			int referencing_dep = list_nth_int(referencing_fds, i + 1);

			if (referencing_dep == referencing_id)
			{
				for (int j = 0; j < list_length(referencing_fds); j += 2)
				{
					int source_det = list_nth_int(referencing_fds, j);
					int source_dep = list_nth_int(referencing_fds, j + 1);

					if (source_det == referencing_det)
					{
						result = lappend_int(result, source_det);
						result = lappend_int(result, source_dep);
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
		int referencing_det = list_nth_int(referencing_fds, i);
		int referencing_dep = list_nth_int(referencing_fds, i + 1);

		if (referencing_dep == referencing_id)
		{
			for (int j = 0; j < list_length(referenced_fds); j += 2)
			{
				int referenced_det = list_nth_int(referenced_fds, j);
				int referenced_dep = list_nth_int(referenced_fds, j + 1);

				if (referenced_det == referenced_id)
				{
					result = lappend_int(result, referencing_det);
					result = lappend_int(result, referenced_dep);
				}
			}
		}
	}

	return result;
}

static const char *
getNodeTypeName(NodeTag tag)
{
	static char buf[100];
	
	switch (tag)
	{
		/* Only include node types we know are defined in our context */
		case T_Invalid:           return "Invalid";
		case T_RangeVar:          return "RangeVar";
		case T_RangeTblRef:       return "RangeTblRef";
		case T_JoinExpr:          return "JoinExpr";
		case T_FromExpr:          return "FromExpr";
		case T_Query:             return "Query";
		case T_ForeignKeyClause:  return "ForeignKeyClause";
		case T_ForeignKeyJoinNode: return "ForeignKeyJoinNode";
		
		default:
			/* For unknown types, return a string with the numeric value */
			snprintf(buf, sizeof(buf), "NodeType_%d", (int)tag);
			return buf;
	}
}

/*
 * traverse_node
 *     Recursively traverses a node tree, handling JoinExpr nodes specially.
 *     For subquery/CTE/view nodes, it only traverses deeper if the fromlist has length one.
 *     Logs the traversal with visual indentation to show recursion depth.
 *
 * pstate: The parser state
 * n: The node to traverse
 * r_nsitem: Current right-side namespace item
 * l_namespace: List of available namespace items
 * indentation_depth: Current depth of recursion for indentation in logs
 * query: If non-NULL, use this query's range table for looking up RTEs; otherwise use pstate
 */
void
traverse_node(ParseState *pstate, Node *n, ParseNamespaceItem *r_nsitem, 
             List *l_namespace, int indentation_depth, Query *query,
             List *track_cols)
{
    #define MAX_TRAVERSE_DEPTH 100
    
    char indent[MAX_TRAVERSE_DEPTH + 1];
    RangeTblEntry *rte;
    int rtindex;
    List *output_columns = NIL;
    char *columns_str = NULL;
    StringInfoData columns_buf;
    List *mapped_cols = NIL;
    bool columns_tracked = false;

    if (indentation_depth > MAX_TRAVERSE_DEPTH)
    {
        elog(NOTICE, "indentation_depth exceeded maximum of %d", MAX_TRAVERSE_DEPTH);
        return;
    }

    /* Create indentation string */
    int i;
    for (i = 0; i < indentation_depth; i++)
        indent[i] = ' ';
    indent[i] = '\0';
    
    if (n == NULL)
    {
        elog(NOTICE, "%s[NULL node]", indent);
        return;
    }
    
    /* Log entry into this node */
    elog(NOTICE, "%s=> Processing node type: %s", indent, getNodeTypeName(nodeTag(n)));
    
    /* Log tracked columns status */
    if (track_cols)
    {
        initStringInfo(&columns_buf);
        appendStringInfoString(&columns_buf, "Tracking columns: ");
        
        ListCell *lc;
        bool first = true;
        foreach(lc, track_cols)
        {
            if (!first)
                appendStringInfoString(&columns_buf, ", ");
            appendStringInfoString(&columns_buf, strVal(lfirst(lc)));
            first = false;
        }
        
        elog(NOTICE, "%s  %s", indent, columns_buf.data);
        pfree(columns_buf.data);
    }
    else
    {
        elog(NOTICE, "%s  Not tracking any columns", indent);
    }
    
    switch (nodeTag(n))
    {
        case T_JoinExpr:
        {
            JoinExpr *join = (JoinExpr *) n;
            List *larg_cols = NIL;
            List *rarg_cols = NIL;
            
            /* Log join information */
            elog(NOTICE, "%s  Join type: %d", indent, join->jointype);
            
            /* Get output columns if the join has an rtindex */
            if (join->rtindex > 0)
            {
                RangeTblEntry *join_rte;
                if (query)
                    join_rte = rt_fetch(join->rtindex, query->rtable);
                else
                    join_rte = rt_fetch(join->rtindex, pstate->p_rtable);
                
                /* Get column list from RTE */
                if (join_rte->eref && join_rte->eref->colnames)
                {
                    output_columns = join_rte->eref->colnames;
                    
                    initStringInfo(&columns_buf);
                    appendStringInfoString(&columns_buf, "Output columns: ");
                    
                    ListCell *lc;
                    bool first = true;
                    foreach(lc, output_columns)
                    {
                        if (!first)
                            appendStringInfoString(&columns_buf, ", ");
                        appendStringInfoString(&columns_buf, strVal(lfirst(lc)));
                        first = false;
                    }
                    
                    elog(NOTICE, "%s  %s", indent, columns_buf.data);
                    pfree(columns_buf.data);
                    
                    /* Check if we're tracking columns and they exist in this join's output */
                    if (track_cols && join_rte->rtekind == RTE_JOIN && join_rte->joinaliasvars)
                    {
                        /* For each tracked column, see if it exists in the output and which side it comes from */
                        int larg_side = -1;  /* RTE index for left side */
                        int rarg_side = -1;  /* RTE index for right side */
                        int target_side = -1;  /* Side that our tracked columns map to */
                        bool all_same_side = true;
                        
                        initStringInfo(&columns_buf);
                        appendStringInfoString(&columns_buf, "Column mappings: ");
                        
                        ListCell *col_lc;
                        bool first = true;
                        
                        foreach(col_lc, track_cols)
                        {
                            char *col_name = strVal(lfirst(col_lc));
                            bool col_found = false;
                            int col_side = -1;
                            int col_attno = -1;
                            
                            /* Find this column in the output columns */
                            int att_pos = 0;
                            ListCell *oc_lc;
                            foreach(oc_lc, output_columns)
                            {
                                if (strcmp(col_name, strVal(lfirst(oc_lc))) == 0)
                                {
                                    col_found = true;
                                    
                                    /* Found the column - see which input it maps to */
                                    Node *map_node = list_nth(join_rte->joinaliasvars, att_pos);
                                    if (IsA(map_node, Var))
                                    {
                                        Var *var = (Var *) map_node;
                                        col_side = var->varno;
                                        col_attno = var->varattno;
                                        
                                        /* Determine which side this is (larg or rarg) */
                                        if (larg_side < 0)
                                            larg_side = col_side;
                                        else if (rarg_side < 0 && col_side != larg_side)
                                            rarg_side = col_side;
                                            
                                        /* All columns should map to the same side for tracking */
                                        if (target_side < 0)
                                            target_side = col_side;
                                        else if (col_side != target_side)
                                            all_same_side = false;
                                            
                                        /* Add to the mapped columns list, keeping track of attnum */
                                        if (col_side == target_side)
                                        {
                                            /* Find actual column name in the target relation */
                                            RangeTblEntry *target_rte = rt_fetch(col_side, 
                                                                               query ? query->rtable : pstate->p_rtable);
                                            if (target_rte->eref && target_rte->eref->colnames && 
                                                col_attno > 0 && col_attno <= list_length(target_rte->eref->colnames))
                                            {
                                                char *mapped_name = strVal(list_nth(target_rte->eref->colnames, col_attno - 1));
                                                mapped_cols = lappend(mapped_cols, makeString(pstrdup(mapped_name)));
                                            }
                                        }
                                        
                                        if (!first)
                                            appendStringInfoString(&columns_buf, ", ");
                                        appendStringInfo(&columns_buf, "%s -> (rel:%d, att:%d)", 
                                                       col_name, col_side, col_attno);
                                        first = false;
                                    }
                                    break;
                                }
                                att_pos++;
                            }
                            
                            if (!col_found)
                            {
                                if (!first)
                                    appendStringInfoString(&columns_buf, ", ");
                                appendStringInfo(&columns_buf, "%s -> NOT FOUND", col_name);
                                first = false;
                            }
                        }
                        
                        elog(NOTICE, "%s  %s", indent, columns_buf.data);
                        pfree(columns_buf.data);
                        
                        columns_tracked = all_same_side && list_length(mapped_cols) > 0;
                        
                        /* Determine if we're going down larg or rarg with our columns */
                        if (columns_tracked)
                        {
                            if (target_side == larg_side)
                            {
                                larg_cols = mapped_cols;
                                elog(NOTICE, "%s  Tracked columns map to left side of join", indent);
                            }
                            else if (target_side == rarg_side)
                            {
                                rarg_cols = mapped_cols;
                                elog(NOTICE, "%s  Tracked columns map to right side of join", indent);
                            }
                        }
                        else
                        {
                            elog(NOTICE, "%s  Failed to track columns (all_same_side=%d, mapped_count=%d)", 
                                 indent, all_same_side, list_length(mapped_cols));
                        }
                    }
                }
                else
                {
                    elog(NOTICE, "%s  No column information available", indent);
                }
            }
            else
            {
                elog(NOTICE, "%s  Join does not have an rtindex yet", indent);
            }
            
            /* Process left arg */
            if (join->larg)
            {
                elog(NOTICE, "%s  Descending into left arg with %d tracked columns", 
                     indent, larg_cols ? list_length(larg_cols) : 0);
                traverse_node(pstate, join->larg, r_nsitem, l_namespace, indentation_depth + 2, query, larg_cols);
            }
            
            /* Process right arg */
            if (join->rarg)
            {
                elog(NOTICE, "%s  Descending into right arg with %d tracked columns", 
                     indent, rarg_cols ? list_length(rarg_cols) : 0);
                traverse_node(pstate, join->rarg, r_nsitem, l_namespace, indentation_depth + 2, query, rarg_cols);
            }
        }
        break;
        
        case T_RangeTblRef:
        {
            RangeTblRef *rtr = (RangeTblRef *) n;
            rtindex = rtr->rtindex;
            
            /* Use the appropriate range table for lookups */
            if (query)
                rte = rt_fetch(rtindex, query->rtable);
            else
                rte = rt_fetch(rtindex, pstate->p_rtable);
                
            elog(NOTICE, "%s  RangeTblRef to RTE index: %d in %s", indent, rtindex, 
                 query ? "view's query" : "outer query");
            
            /* Log column information */
            if (rte->eref && rte->eref->colnames)
            {
                output_columns = rte->eref->colnames;
                
                initStringInfo(&columns_buf);
                appendStringInfoString(&columns_buf, "Columns: ");
                
                ListCell *lc;
                bool first = true;
                foreach(lc, output_columns)
                {
                    if (!first)
                        appendStringInfoString(&columns_buf, ", ");
                    appendStringInfoString(&columns_buf, strVal(lfirst(lc)));
                    first = false;
                }
                
                elog(NOTICE, "%s  %s", indent, columns_buf.data);
                pfree(columns_buf.data);
                
                /* Check if we're tracking columns and they exist in this RTE's output */
                if (track_cols)
                {
                    initStringInfo(&columns_buf);
                    appendStringInfoString(&columns_buf, "Tracking status: ");
                    
                    bool all_found = true;
                    bool first = true;
                    ListCell *col_lc;
                    foreach(col_lc, track_cols)
                    {
                        char *col_name = strVal(lfirst(col_lc));
                        bool col_found = false;
                        
                        /* Find this column in the output columns */
                        ListCell *oc_lc;
                        foreach(oc_lc, output_columns)
                        {
                            if (strcmp(col_name, strVal(lfirst(oc_lc))) == 0)
                            {
                                col_found = true;
                                break;
                            }
                        }
                        
                        if (!first)
                            appendStringInfoString(&columns_buf, ", ");
                        appendStringInfo(&columns_buf, "%s: %s", col_name, col_found ? "FOUND" : "NOT FOUND");
                        first = false;
                        
                        if (!col_found)
                            all_found = false;
                    }
                    
                    elog(NOTICE, "%s  %s", indent, columns_buf.data);
                    pfree(columns_buf.data);
                    
                    columns_tracked = all_found;
                    
                    if (columns_tracked)
                        mapped_cols = list_copy(track_cols);
                    else
                        elog(NOTICE, "%s  Some tracked columns not found in this RTE", indent);
                }
            }
            
            /* Process the referenced RTE */
            switch (rte->rtekind)
            {
                case RTE_RELATION:
                {
                    Relation rel;
                    
                    elog(NOTICE, "%s  RTE is a relation (relid: %u)", indent, rte->relid);
                    
                    /* Open the relation to check its type */
                    rel = table_open(rte->relid, AccessShareLock);
                    
                    if (rel->rd_rel->relkind == RELKIND_VIEW)
                    {
                        Query *viewQuery = get_view_query(rel);
                        List *view_mapped_cols = NIL;
                        
                        elog(NOTICE, "%s  Relation is a VIEW", indent);
                        
                        /* Map tracked columns to the view's underlying query */
                        if (columns_tracked && mapped_cols)
                        {
                            view_mapped_cols = NIL;
                            ListCell *col_lc;
                            foreach(col_lc, mapped_cols)
                            {
                                char *col_name = strVal(lfirst(col_lc));
                                
                                /* Find this column in the view's target list */
                                ListCell *tl_lc;
                                int targetno = 0;
                                foreach(tl_lc, viewQuery->targetList)
                                {
                                    TargetEntry *te = (TargetEntry *) lfirst(tl_lc);
                                    if (!te->resjunk && strcmp(te->resname, col_name) == 0)
                                    {
                                        /* Found matching target entry - check if it's a simple column reference */
                                        if (IsA(te->expr, Var))
                                        {
                                            Var *var = (Var *) te->expr;
                                            /* Get the original column name from the referenced RTE */
                                            RangeTblEntry *ref_rte = rt_fetch(var->varno, viewQuery->rtable);
                                            if (ref_rte->eref && ref_rte->eref->colnames && 
                                                var->varattno > 0 && var->varattno <= list_length(ref_rte->eref->colnames))
                                            {
                                                char *orig_name = strVal(list_nth(ref_rte->eref->colnames, var->varattno - 1));
                                                view_mapped_cols = lappend(view_mapped_cols, makeString(pstrdup(orig_name)));
                                                elog(NOTICE, "%s  Mapped column %s to underlying column %s", 
                                                     indent, col_name, orig_name);
                                            }
                                        }
                                        else
                                        {
                                            elog(NOTICE, "%s  Column %s maps to an expression, not tracking", indent, col_name);
                                        }
                                        break;
                                    }
                                    targetno++;
                                }
                            }
                            
                            if (list_length(view_mapped_cols) == list_length(mapped_cols))
                                elog(NOTICE, "%s  Successfully mapped all columns to view query", indent);
                            else
                                elog(NOTICE, "%s  Failed to map some columns (%d of %d mapped)", 
                                     indent, list_length(view_mapped_cols), list_length(mapped_cols));
                        }
                        
                        /* Only traverse view query if fromlist has length 1 */
                        if (viewQuery->jointree && 
                            viewQuery->jointree->fromlist && 
                            list_length(viewQuery->jointree->fromlist) == 1)
                        {
                            elog(NOTICE, "%s  VIEW has single fromlist item, traversing deeper", indent);
                            /* Pass the view query as the context for the next level of traversal */
                            traverse_node(pstate, 
                                        (Node *) linitial(viewQuery->jointree->fromlist), 
                                        r_nsitem, l_namespace, indentation_depth + 2, 
                                        viewQuery, view_mapped_cols);
                        }
                        else
                        {
                            elog(NOTICE, "%s  VIEW has multiple or no fromlist items, stopping here", indent);
                        }
                    }
                    else if (rel->rd_rel->relkind == RELKIND_RELATION || 
                             rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
                    {
                        /* Base table - this is our base case */
                        /* Get actual column names from the system catalogs */
                        TupleDesc tupdesc = RelationGetDescr(rel);
                        int natts = tupdesc->natts;
                        
                        initStringInfo(&columns_buf);
                        appendStringInfoString(&columns_buf, "Base table columns: ");
                        
                        bool first = true;
                        for (i = 0; i < natts; i++)
                        {
                            Form_pg_attribute att = TupleDescAttr(tupdesc, i);
                            
                            /* Skip dropped columns */
                            if (att->attisdropped)
                                continue;
                                
                            if (!first)
                                appendStringInfoString(&columns_buf, ", ");
                            appendStringInfoString(&columns_buf, NameStr(att->attname));
                            first = false;
                        }
                        
                        /* Check if tracked columns exist in this base table */
                        bool tracked_in_base = false;
                        if (columns_tracked && mapped_cols)
                        {
                            StringInfoData tracked_buf;
                            initStringInfo(&tracked_buf);
                            appendStringInfoString(&tracked_buf, "Tracked columns in base table: ");
                            
                            bool all_in_base = true;
                            bool first = true;
                            ListCell *col_lc;
                            foreach(col_lc, mapped_cols)
                            {
                                char *col_name = strVal(lfirst(col_lc));
                                bool found_in_base = false;
                                
                                /* Find column in base table's attributes */
                                for (i = 0; i < natts; i++)
                                {
                                    Form_pg_attribute att = TupleDescAttr(tupdesc, i);
                                    if (!att->attisdropped && 
                                        strcmp(NameStr(att->attname), col_name) == 0)
                                    {
                                        found_in_base = true;
                                        break;
                                    }
                                }
                                
                                if (!first)
                                    appendStringInfoString(&tracked_buf, ", ");
                                appendStringInfo(&tracked_buf, "%s: %s", col_name, found_in_base ? "FOUND" : "NOT FOUND");
                                first = false;
                                
                                if (!found_in_base)
                                    all_in_base = false;
                            }
                            
                            tracked_in_base = all_in_base;
                            
                            if (tracked_in_base)
                                appendStringInfoString(&tracked_buf, " (ALL FOUND)");
                            else
                                appendStringInfoString(&tracked_buf, " (SOME NOT FOUND)");
                                
                            elog(NOTICE, "%s  %s", indent, tracked_buf.data);
                            pfree(tracked_buf.data);
                        }
                        
                        elog(NOTICE, "%s*** Found base table: %s with %s %s***", indent, 
                             get_rel_name(rte->relid) ? get_rel_name(rte->relid) : "unknown",
                             columns_buf.data,
                             tracked_in_base ? "- TRACKED COLUMNS FOUND HERE" : "");
                        pfree(columns_buf.data);
                    }
                    else
                    {
                        elog(NOTICE, "%s  Relation has unsupported relkind: %c", indent, rel->rd_rel->relkind);
                    }
                    
                    /* Close the relation */
                    table_close(rel, AccessShareLock);
                }
                break;
                
                case RTE_SUBQUERY:
                {
                    Query *subquery = rte->subquery;
                    List *subq_mapped_cols = NIL;
                    
                    elog(NOTICE, "%s  RTE is a SUBQUERY", indent);
                    
                    /* Log subquery output columns */
                    initStringInfo(&columns_buf);
                    appendStringInfoString(&columns_buf, "Subquery output columns: ");
                    
                    ListCell *lc;
                    bool first = true;
                    foreach(lc, subquery->targetList)
                    {
                        TargetEntry *tle = (TargetEntry *) lfirst(lc);
                        
                        /* Skip resjunk columns */
                        if (tle->resjunk)
                            continue;
                            
                        if (!first)
                            appendStringInfoString(&columns_buf, ", ");
                        appendStringInfoString(&columns_buf, tle->resname ? tle->resname : "<unnamed>");
                        first = false;
                    }
                    
                    elog(NOTICE, "%s  %s", indent, columns_buf.data);
                    pfree(columns_buf.data);
                    
                    /* Map tracked columns to the subquery's underlying query */
                    if (columns_tracked && mapped_cols)
                    {
                        subq_mapped_cols = NIL;
                        ListCell *col_lc;
                        foreach(col_lc, mapped_cols)
                        {
                            char *col_name = strVal(lfirst(col_lc));
                            
                            /* Find this column in the subquery's target list */
                            ListCell *tl_lc;
                            int targetno = 0;
                            foreach(tl_lc, subquery->targetList)
                            {
                                TargetEntry *te = (TargetEntry *) lfirst(tl_lc);
                                if (!te->resjunk && strcmp(te->resname, col_name) == 0)
                                {
                                    /* Found matching target entry - check if it's a simple column reference */
                                    if (IsA(te->expr, Var))
                                    {
                                        Var *var = (Var *) te->expr;
                                        /* Get the original column name from the referenced RTE */
                                        RangeTblEntry *ref_rte = rt_fetch(var->varno, subquery->rtable);
                                        if (ref_rte->eref && ref_rte->eref->colnames && 
                                            var->varattno > 0 && var->varattno <= list_length(ref_rte->eref->colnames))
                                        {
                                            char *orig_name = strVal(list_nth(ref_rte->eref->colnames, var->varattno - 1));
                                            subq_mapped_cols = lappend(subq_mapped_cols, makeString(pstrdup(orig_name)));
                                            elog(NOTICE, "%s  Mapped column %s to underlying column %s", 
                                                 indent, col_name, orig_name);
                                        }
                                    }
                                    else
                                    {
                                        elog(NOTICE, "%s  Column %s maps to an expression, not tracking", indent, col_name);
                                    }
                                    break;
                                }
                                targetno++;
                            }
                        }
                        
                        if (list_length(subq_mapped_cols) == list_length(mapped_cols))
                            elog(NOTICE, "%s  Successfully mapped all columns to subquery", indent);
                        else
                            elog(NOTICE, "%s  Failed to map some columns (%d of %d mapped)", 
                                 indent, list_length(subq_mapped_cols), list_length(mapped_cols));
                    }
                    
                    /* Only traverse subquery if fromlist has length 1 */
                    if (subquery->jointree && 
                        subquery->jointree->fromlist && 
                        list_length(subquery->jointree->fromlist) == 1)
                    {
                        elog(NOTICE, "%s  SUBQUERY has single fromlist item, traversing deeper", indent);
                        /* Pass the subquery as the context for the next level of traversal */
                        traverse_node(pstate, 
                                     (Node *) linitial(subquery->jointree->fromlist), 
                                     r_nsitem, l_namespace, indentation_depth + 2,
                                     subquery, subq_mapped_cols);
                    }
                    else
                    {
                        elog(NOTICE, "%s  SUBQUERY has multiple or no fromlist items, stopping here", indent);
                    }
                }
                break;
                
                case RTE_CTE:
                {
                    Index levelsup;
                    CommonTableExpr *cte;
                    List *cte_mapped_cols = NIL;
                    
                    elog(NOTICE, "%s  RTE is a CTE (name: %s)", indent, rte->ctename);
                    
                    /* Find the CTE */
                    cte = scanNameSpaceForCTE(pstate, rte->ctename, &levelsup);
                    
                    if (cte && !cte->cterecursive && IsA(cte->ctequery, Query))
                    {
                        Query *cteQuery = (Query *) cte->ctequery;
                        
                        /* Log CTE output columns */
                        initStringInfo(&columns_buf);
                        appendStringInfoString(&columns_buf, "CTE output columns: ");
                        
                        ListCell *lc;
                        bool first = true;
                        foreach(lc, cteQuery->targetList)
                        {
                            TargetEntry *tle = (TargetEntry *) lfirst(lc);
                            
                            /* Skip resjunk columns */
                            if (tle->resjunk)
                                continue;
                                
                            if (!first)
                                appendStringInfoString(&columns_buf, ", ");
                            appendStringInfoString(&columns_buf, tle->resname ? tle->resname : "<unnamed>");
                            first = false;
                        }
                        
                        elog(NOTICE, "%s  %s", indent, columns_buf.data);
                        pfree(columns_buf.data);
                        
                        /* Map tracked columns to the CTE's underlying query */
                        if (columns_tracked && mapped_cols)
                        {
                            cte_mapped_cols = NIL;
                            ListCell *col_lc;
                            foreach(col_lc, mapped_cols)
                            {
                                char *col_name = strVal(lfirst(col_lc));
                                
                                /* Find this column in the CTE's target list */
                                ListCell *tl_lc;
                                int targetno = 0;
                                foreach(tl_lc, cteQuery->targetList)
                                {
                                    TargetEntry *te = (TargetEntry *) lfirst(tl_lc);
                                    if (!te->resjunk && strcmp(te->resname, col_name) == 0)
                                    {
                                        /* Found matching target entry - check if it's a simple column reference */
                                        if (IsA(te->expr, Var))
                                        {
                                            Var *var = (Var *) te->expr;
                                            /* Get the original column name from the referenced RTE */
                                            RangeTblEntry *ref_rte = rt_fetch(var->varno, cteQuery->rtable);
                                            if (ref_rte->eref && ref_rte->eref->colnames && 
                                                var->varattno > 0 && var->varattno <= list_length(ref_rte->eref->colnames))
                                            {
                                                char *orig_name = strVal(list_nth(ref_rte->eref->colnames, var->varattno - 1));
                                                cte_mapped_cols = lappend(cte_mapped_cols, makeString(pstrdup(orig_name)));
                                                elog(NOTICE, "%s  Mapped column %s to underlying column %s", 
                                                     indent, col_name, orig_name);
                                            }
                                        }
                                        else
                                        {
                                            elog(NOTICE, "%s  Column %s maps to an expression, not tracking", indent, col_name);
                                        }
                                        break;
                                    }
                                    targetno++;
                                }
                            }
                            
                            if (list_length(cte_mapped_cols) == list_length(mapped_cols))
                                elog(NOTICE, "%s  Successfully mapped all columns to CTE query", indent);
                            else
                                elog(NOTICE, "%s  Failed to map some columns (%d of %d mapped)", 
                                     indent, list_length(cte_mapped_cols), list_length(mapped_cols));
                        }
                        
                        /* Only traverse CTE query if fromlist has length 1 */
                        if (cteQuery->jointree && 
                            cteQuery->jointree->fromlist && 
                            list_length(cteQuery->jointree->fromlist) == 1)
                        {
                            elog(NOTICE, "%s  CTE has single fromlist item, traversing deeper", indent);
                            /* Pass the CTE query as the context for the next level of traversal */
                            traverse_node(pstate, 
                                         (Node *) linitial(cteQuery->jointree->fromlist), 
                                         r_nsitem, l_namespace, indentation_depth + 2,
                                         cteQuery, cte_mapped_cols);
                        }
                        else
                        {
                            elog(NOTICE, "%s  CTE has multiple or no fromlist items, stopping here", indent);
                        }
                    }
                    else if (cte && cte->cterecursive)
                    {
                        elog(NOTICE, "%s  CTE is recursive, stopping here", indent);
                    }
                }
                break;

                default:
                    elog(NOTICE, "%s  RTE has unsupported rtekind: %d", indent, rte->rtekind);
                    break;
            }
        }
        break;
        
        default:
            elog(NOTICE, "%s  Unsupported node type: %s", indent, getNodeTypeName(nodeTag(n)));
            break;
    }
    
    /* Log exit from this node */
    elog(NOTICE, "%s<= Returning from node type: %s", indent, getNodeTypeName(nodeTag(n)));
}

