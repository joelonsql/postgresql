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
static RangeTblEntry *drill_down_to_base_rel_query(ParseState *pstate, Query *query,
												   List *attnos, List **base_attnums,
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
static void traverse_query_node(ParseState *pstate, Node *node, int depth, List **uniqueness_preservation, List **functional_dependencies, int *baserel_index);

/* Helper function to convert node tag to string */
static const char *
get_node_tag_name(NodeTag tag)
{
	switch (tag)
	{
		case T_Invalid: return "T_Invalid";
		case T_Query: return "T_Query";
		case T_JoinExpr: return "T_JoinExpr";
		case T_RangeTblEntry: return "T_RangeTblEntry";
		case T_RangeTblRef: return "T_RangeTblRef";
		case T_FromExpr: return "T_FromExpr";
		case T_RangeVar: return "T_RangeVar";
		case T_ForeignKeyJoinNode: return "T_ForeignKeyJoinNode";
		/* Add additional cases as needed for node types you expect to encounter */
		default: return "unknown node type";
	}
}

/* Helper function to generate indentation string */
static char *
get_indentation(int depth)
{
	static char indentation[256];
	
	if (depth <= 0)
		return "";
	
	/* Limit the maximum indentation to prevent buffer overflow */
	if (depth > 60)
		depth = 60;
	
	memset(indentation, ' ', depth * 4);
	indentation[depth * 4] = '\0';
	
	return indentation;
}

/* Global variable to track unique base relation indices */
static int next_baserel_index = 1;

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
	Oid			referencing_relid;
	Oid			referenced_relid;
	int			referencing_id;
	int			referenced_id;
	bool		found_fd = false;
	bool		fk_cols_unique;
	bool		fk_cols_not_null;
	List       *uniqueness_preservation = NIL;
	List       *functional_dependencies = NIL;
	int         baserel_index = 0;

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

	traverse_query_node(pstate, (Node *) join, 0, &uniqueness_preservation, &functional_dependencies, &baserel_index);

	elog(NOTICE, "Uniqueness preservation after query traversal: %d entries", list_length(uniqueness_preservation));
	elog(NOTICE, "Functional dependencies after query traversal: %d entries", list_length(functional_dependencies));

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
/*
	if (!list_member(referenced_rte->uniqueness_preservation, referenced_id))
	{
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("foreign key join violation"),
				 errdetail("referenced relation does not preserve uniqueness of keys"),
				 parser_errposition(pstate, fkjn->location)));
	}
*/
	/*
	 * Check functional dependencies - looking for (referenced_id,
	 * referenced_id) pairs
	 */
/*
	for (int i = 0; i < list_length(referenced_rte->functional_dependencies); i += 2)
	{
		RTEId	   *fd_dep = (RTEId *) list_nth(referenced_rte->functional_dependencies, i);
		RTEId	   *fd_dcy = (RTEId *) list_nth(referenced_rte->functional_dependencies, i + 1);

		if (equal(fd_dep, referenced_id) && equal(fd_dcy, referenced_id))
		{
			found_fd = true;
			break;
		}
	}
*/
	found_fd = true;

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
/*
	fkjn_node->uniqueness_preservation = update_uniqueness_preservation(
																		referencing_rte->uniqueness_preservation,
																		referenced_rte->uniqueness_preservation,
																		fk_cols_unique
		);
	fkjn_node->functional_dependencies = update_functional_dependencies(
																		referencing_rte->functional_dependencies,
																		referencing_id,
																		referenced_rte->functional_dependencies,
																		referenced_id,
																		fk_cols_not_null,
																		join->jointype,
																		fkjn->fkdir
		);
*/
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
				Index		levelsup;
				CommonTableExpr *cte = scanNameSpaceForCTE(pstate, rte->ctename, &levelsup);

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

/*
 * Function to traverse a Query node with its own namespace context
 * This ensures that RangeTblRefs inside the query properly reference
 * the query's own range table rather than the parent query's.
 */
static void
traverse_query_standalone(Query *query, int depth, List **uniqueness_preservation, List **functional_dependencies, int *baserel_index)
{
	ParseState *subquery_pstate = NULL;
	ListCell *lc;
	
	if (query == NULL)
	{
		elog(NOTICE, "%straverse_query_standalone: NULL query", get_indentation(depth));
		return;
	}
	
	elog(NOTICE, "%straverse_query_standalone: processing Query (command type: %d)", 
		 get_indentation(depth), query->commandType);
	
	/* Create a new ParseState just to hold this query's range table */
	subquery_pstate = make_parsestate(NULL);
	subquery_pstate->p_rtable = query->rtable;
	
	/* Process the join tree if it exists */
	if (query->jointree)
	{
		elog(NOTICE, "%straverse_query_standalone: processing Query jointree", get_indentation(depth));
		if (query->jointree->fromlist)
		{
			int fromlist_length = list_length(query->jointree->fromlist);
			elog(NOTICE, "%straverse_query_standalone: processing Query jointree fromlist (%d entries)", 
				 get_indentation(depth), fromlist_length);
			
			if (fromlist_length == 1)
			{
				foreach(lc, query->jointree->fromlist)
				{
					traverse_query_node(subquery_pstate, (Node *) lfirst(lc), depth + 1, uniqueness_preservation, functional_dependencies, baserel_index);
					
					elog(NOTICE, "%straverse_query_standalone: completed processing Query jointree fromlist item", 
						 get_indentation(depth));
				}
				elog(NOTICE, "%straverse_query_standalone: completed processing Query jointree fromlist", 
					 get_indentation(depth));
			}
			else
			{
				ereport(WARNING,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("foreign key joins with multiple tables in fromlist not fully supported in query traversal")));
			}
		}
		elog(NOTICE, "%straverse_query_standalone: completed processing Query jointree", 
			 get_indentation(depth));
	}
	
	/* Process set operations (UNION, INTERSECT, etc.) */
	if (query->setOperations)
	{
		elog(NOTICE, "%straverse_query_standalone: Query has set operations", get_indentation(depth));
		ereport(WARNING,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set operations not fully supported in query traversal")));
	}
	
	/* Free the temporary ParseState */
	free_parsestate(subquery_pstate);
}

static void
traverse_query_node(ParseState *pstate, Node *node, int depth, List **uniqueness_preservation, List **functional_dependencies, int *baserel_index)
{
	if (node == NULL)
	{
		elog(NOTICE, "%straverse_query_node: NULL node", get_indentation(depth));
		return;
	}

	elog(NOTICE, "%straverse_query_node: entering node type %d (%s)", 
		 get_indentation(depth), nodeTag(node), get_node_tag_name(nodeTag(node)));

	switch (nodeTag(node))
	{
		case T_JoinExpr:
			{
				JoinExpr *join = (JoinExpr *) node;
				
				elog(NOTICE, "%straverse_query_node: processing JoinExpr", get_indentation(depth));
				
				if (join->fkJoin && IsA(join->fkJoin, ForeignKeyJoinNode))
				{
					ForeignKeyJoinNode *fkjn = (ForeignKeyJoinNode *) join->fkJoin;
					List *larg_uniqueness = NIL;
					List *larg_fds = NIL;
					List *rarg_uniqueness = NIL;
					List *rarg_fds = NIL;
					List *referencing_uniqueness = NIL;
					List *referencing_fds = NIL;
					List *referenced_uniqueness = NIL;
					List *referenced_fds = NIL;
					int referencing_id, referenced_id;
					bool fk_cols_not_null, fk_cols_unique;
					int larg_baserel_index = 0;
					int rarg_baserel_index = 0;
					
					/* Process the left and right args of the join */
					if (join->larg)
					{
						elog(NOTICE, "%straverse_query_node: descending into JoinExpr left arg", get_indentation(depth));
						traverse_query_node(pstate, join->larg, depth + 1, &larg_uniqueness, &larg_fds, &larg_baserel_index);
						elog(NOTICE, "%straverse_query_node: returned from JoinExpr left arg", get_indentation(depth));
					}
						
					if (join->rarg)
					{
						elog(NOTICE, "%straverse_query_node: descending into JoinExpr right arg", get_indentation(depth));
						traverse_query_node(pstate, join->rarg, depth + 1, &rarg_uniqueness, &rarg_fds, &rarg_baserel_index);
						elog(NOTICE, "%straverse_query_node: returned from JoinExpr right arg", get_indentation(depth));
					}
					
					/* Determine which is referencing and which is referenced */
					if (fkjn->fkdir == FKDIR_FROM)
					{
						/* Left side is referenced, right side is referencing */
						referencing_id = rarg_baserel_index;
						referenced_id = larg_baserel_index;
						referencing_uniqueness = rarg_uniqueness;
						referencing_fds = rarg_fds;
						referenced_uniqueness = larg_uniqueness;
						referenced_fds = larg_fds;
					}
					else /* FKDIR_TO */
					{
						/* Left side is referencing, right side is referenced */
						referencing_id = larg_baserel_index;
						referenced_id = rarg_baserel_index;
						referencing_uniqueness = larg_uniqueness;
						referencing_fds = larg_fds;
						referenced_uniqueness = rarg_uniqueness;
						referenced_fds = rarg_fds;
					}
					
					/* Use the proper functions to determine uniqueness and not-null constraints */
					RangeTblEntry *referencing_rte = rt_fetch(fkjn->referencingVarno, pstate->p_rtable);
					Oid referencing_relid = referencing_rte->relid;
					fk_cols_unique = is_referencing_cols_unique(referencing_relid, fkjn->referencingAttnums);
					fk_cols_not_null = is_referencing_cols_not_null(referencing_relid, fkjn->referencingAttnums);
					
					/* For testing uniqueness preservation */
					if (list_member_int(referenced_uniqueness, referenced_id))
					{
						elog(NOTICE, "%straverse_query_node: referenced relation preserves uniqueness", 
							 get_indentation(depth));
					}
					
					/* Update uniqueness preservation */
					*uniqueness_preservation = update_uniqueness_preservation(
												referencing_uniqueness,
												referenced_uniqueness,
												fk_cols_unique);
					
					/* Update functional dependencies */
					*functional_dependencies = update_functional_dependencies(
												referencing_fds,
												referencing_id,
												referenced_fds,
												referenced_id,
												fk_cols_not_null,
												join->jointype,
												fkjn->fkdir);
					
					elog(NOTICE, "%straverse_query_node: processed foreign key join", get_indentation(depth));
				}
			}
			break;
			
		case T_RangeTblRef:
			{
				RangeTblRef *rtr = (RangeTblRef *) node;
				
				elog(NOTICE, "%straverse_query_node: processing RangeTblRef (rtindex: %d)", 
					 get_indentation(depth), rtr->rtindex);
				
				/* If we have the parse state, we can access the referenced RTE */
				if (pstate && rtr->rtindex > 0 && rtr->rtindex <= list_length(pstate->p_rtable))
				{
					RangeTblEntry *referenced_rte;
					
					referenced_rte = rt_fetch(rtr->rtindex, pstate->p_rtable);
					elog(NOTICE, "%straverse_query_node: RangeTblRef references RTE kind: %d", 
						 get_indentation(depth), referenced_rte->rtekind);
					
					/* Recursively process the referenced RTE */
					elog(NOTICE, "%straverse_query_node: descending into referenced RTE", get_indentation(depth));
					traverse_query_node(pstate, (Node *) referenced_rte, depth + 1, uniqueness_preservation, functional_dependencies, baserel_index);
					elog(NOTICE, "%straverse_query_node: returned from referenced RTE", get_indentation(depth));
				}
				else
				{
					elog(NOTICE, "%straverse_query_node: cannot access referenced RTE (missing parse state or invalid rtindex)", 
						 get_indentation(depth));
				}
			}
			break;
			
		case T_Query:
			{
				Query *query = (Query *) node;
				/* Use the standalone function to traverse queries with their own context */
				traverse_query_standalone(query, depth, uniqueness_preservation, functional_dependencies, baserel_index);
			}
			break;
			
		case T_RangeTblEntry:
			{
				RangeTblEntry *rte = (RangeTblEntry *) node;
				Relation rel;
				
				elog(NOTICE, "%straverse_query_node: processing RangeTblEntry (kind: %d)", 
					 get_indentation(depth), rte->rtekind);
				
				switch (rte->rtekind)
				{
					case RTE_RELATION:
						elog(NOTICE, "%straverse_query_node: processing RTE_RELATION (relid: %u, relkind: %c)", 
							 get_indentation(depth), rte->relid, rte->relkind);
						
						/* For base tables, set up uniqueness and functional dependency properties */
						if (rte->relkind == RELKIND_RELATION || rte->relkind == RELKIND_PARTITIONED_TABLE)
						{
							/* Use the passed baserel_index parameter */
							*baserel_index = next_baserel_index++;
							char *rel_name = get_rel_name(rte->relid);
							
							elog(NOTICE, "%straverse_query_node: base table '%s' assigned index %d", 
								 get_indentation(depth), rel_name ? rel_name : "unknown", *baserel_index);
							
							/* Initialize uniqueness preservation to include this base relation */
							*uniqueness_preservation = list_make1_int(*baserel_index);
							
							/* Initialize functional dependencies with (baserel_index, baserel_index) */
							*functional_dependencies = list_make2_int(*baserel_index, *baserel_index);
							
							elog(NOTICE, "%straverse_query_node: initialized uniqueness and FDs for base relation", 
								 get_indentation(depth));
						}
						/* For VIEWs, get the view's query and traverse it */
						else if (rte->relkind == RELKIND_VIEW)
						{
							elog(NOTICE, "%straverse_query_node: processing VIEW", get_indentation(depth));
							rel = table_open(rte->relid, AccessShareLock);
							Query *viewQuery = get_view_query(rel);
							table_close(rel, AccessShareLock);
							
							elog(NOTICE, "%straverse_query_node: descending into VIEW query", get_indentation(depth));
							traverse_query_standalone(viewQuery, depth + 1, uniqueness_preservation, functional_dependencies, baserel_index);
							elog(NOTICE, "%straverse_query_node: returned from VIEW query", get_indentation(depth));
						}
						break;
						
					case RTE_SUBQUERY:
						elog(NOTICE, "%straverse_query_node: processing RTE_SUBQUERY", get_indentation(depth));
						/* Traverse the subquery using the standalone function */
						elog(NOTICE, "%straverse_query_node: descending into subquery", get_indentation(depth));
						traverse_query_standalone(rte->subquery, depth + 1, uniqueness_preservation, functional_dependencies, baserel_index);
						elog(NOTICE, "%straverse_query_node: returned from subquery", get_indentation(depth));
						break;
						
					case RTE_JOIN:
						elog(NOTICE, "%straverse_query_node: found RTE_JOIN, handled during jointree traversal", 
							 get_indentation(depth));
						/* Join RTEs are handled during jointree traversal */
						break;
						
					case RTE_CTE:
						{
							elog(NOTICE, "%straverse_query_node: processing RTE_CTE (name: %s)", 
								 get_indentation(depth), rte->ctename);
							/* For CTEs, find the CTE query and traverse it */
							Index levelsup;
							CommonTableExpr *cte = scanNameSpaceForCTE(pstate, rte->ctename, &levelsup);
							
							if (cte && !cte->cterecursive && IsA(cte->ctequery, Query))
							{
								elog(NOTICE, "%straverse_query_node: descending into CTE query", get_indentation(depth));
								traverse_query_standalone(castNode(Query, cte->ctequery), depth + 1, 
														 uniqueness_preservation, functional_dependencies, baserel_index);
								elog(NOTICE, "%straverse_query_node: returned from CTE query", get_indentation(depth));
							}
							else if (cte && cte->cterecursive)
							{
								elog(NOTICE, "%straverse_query_node: found recursive CTE, not fully supported", 
									 get_indentation(depth));
								ereport(WARNING,
										(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
										 errmsg("recursive CTEs not fully supported in query traversal")));
							}
						}
						break;
						
					default:
						elog(NOTICE, "%straverse_query_node: unsupported RTE kind: %d", 
							 get_indentation(depth), rte->rtekind);
						ereport(WARNING,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("unsupported RTE kind in query traversal: %d", rte->rtekind)));
						break;
				}
			}
			break;
			
		default:
			elog(NOTICE, "%straverse_query_node: unsupported node type: %d", 
				 get_indentation(depth), nodeTag(node));
			ereport(WARNING,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported node type in query traversal: %d", nodeTag(node))));
			break;
	}

	elog(NOTICE, "%straverse_query_node: exiting node type %d (%s)", 
		 get_indentation(depth), nodeTag(node), get_node_tag_name(nodeTag(node)));
}