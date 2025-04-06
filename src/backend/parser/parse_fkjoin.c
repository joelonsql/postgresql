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
							 List *referencing_cols, List *referenced_cols);
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
static List *update_functional_dependencies(List *referencing_functional_dependencies,
											RTEId *referencing_id,
											List *referenced_functional_dependencies,
											RTEId *referenced_id,
											bool fk_cols_not_null,
											JoinType join_type,
											ForeignKeyDirection fk_dir);

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
	List	   *referencing_base_cols;
	List	   *referenced_base_cols;
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;
	Oid			referencing_relid;
	Oid			referenced_relid;
	RTEId	   *referencing_id;
	RTEId	   *referenced_id;
	bool		found_fd = false;
	bool		fk_cols_unique;
	bool		fk_cols_not_null;

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

	base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
												  referencing_attnums,
												  &referencing_base_cols,
												  fkjn->location);
	base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
												 referenced_attnums,
												 &referenced_base_cols,
												 fkjn->location);

	referencing_relid = base_referencing_rte->relid;
	referenced_relid = base_referenced_rte->relid;
	referencing_id = base_referencing_rte->rteid;
	referenced_id = base_referenced_rte->rteid;

	Assert(referencing_relid != InvalidOid && referenced_relid != InvalidOid);

	fkoid = find_foreign_key(referencing_relid, referenced_relid,
							 referencing_base_cols, referenced_base_cols);

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
	if (referenced_rte->uniqueness_preservation == NIL ||
		!list_member(referenced_rte->uniqueness_preservation, referenced_id))
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

	fk_cols_unique = is_referencing_cols_unique(referencing_relid, referencing_base_cols);
	fk_cols_not_null = is_referencing_cols_not_null(referencing_relid, referencing_base_cols);

	join->quals = build_fk_join_on_clause(pstate, referencing_rel->p_nscolumns, referencing_attnums, referenced_rel->p_nscolumns, referenced_attnums);

	fkjn_node = makeNode(ForeignKeyJoinNode);
	fkjn_node->fkdir = fkjn->fkdir;
	fkjn_node->referencingVarno = referencing_rel->p_rtindex;
	fkjn_node->referencingAttnums = referencing_attnums;
	fkjn_node->referencedVarno = referenced_rel->p_rtindex;
	fkjn_node->referencedAttnums = referenced_attnums;
	fkjn_node->constraint = fkoid;
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
				 List *referencing_cols, List *referenced_cols)
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
			nkeys != list_length(referencing_cols))
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

			forboth(lc1, referencing_cols, lc2, referenced_cols)
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
		result = list_concat(result,
							 list_copy(referenced_uniqueness_preservation));
	}

	return result;
}

/*
 * update_functional_dependencies
 *      Updates the functional dependencies for a foreign key join
 */
static List *
update_functional_dependencies(List *referencing_functional_dependencies,
							   RTEId *referencing_id,
							   List *referenced_functional_dependencies,
							   RTEId *referenced_id,
							   bool fk_cols_not_null,
							   JoinType join_type,
							   ForeignKeyDirection fk_dir)
{
	List	   *result = NIL;

	/* Part 1: Chain dependencies through the join if FK cols are NOT NULL */
	if (fk_cols_not_null)
	{
		/*
		 * First check if (referenced_id, referenced_id) is in
		 * referenced_functional_dependencies
		 */
		bool		referenced_self_dep_exists = false;

		for (int i = 0; i < list_length(referenced_functional_dependencies); i += 2)
		{
			RTEId	   *ref_dep = (RTEId *) list_nth(referenced_functional_dependencies, i);
			RTEId	   *ref_dcy = (RTEId *) list_nth(referenced_functional_dependencies, i + 1);

			if (equal(ref_dep, referenced_id) && equal(ref_dcy, referenced_id))
			{
				referenced_self_dep_exists = true;
				break;
			}
		}

		/* If self-dependency exists, process the dependencies */
		if (referenced_self_dep_exists)
		{
			/* Loop 1: Find all items where dcy_id matches referencing_id */
			for (int i = 0; i < list_length(referencing_functional_dependencies); i += 2)
			{
				RTEId	   *ref_dep = (RTEId *) list_nth(referencing_functional_dependencies, i);
				RTEId	   *ref_dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

				if (equal(ref_dcy, referencing_id))
				{
					/*
					 * Loop 2: Find all items where dep_id matches Loop 1's
					 * dep_id and add them to the result
					 */
					for (int j = 0; j < list_length(referencing_functional_dependencies); j += 2)
					{
						RTEId	   *source_dep = (RTEId *) list_nth(referencing_functional_dependencies, j);
						RTEId	   *source_dcy = (RTEId *) list_nth(referencing_functional_dependencies, j + 1);

						if (equal(source_dep, ref_dep))
						{
							result = lappend(result, copyObject(source_dep));
							result = lappend(result, copyObject(source_dcy));
						}
					}
				}
			}
		}

		/* Part 2: Create transitive dependencies */
		for (int i = 0; i < list_length(referencing_functional_dependencies); i += 2)
		{
			RTEId	   *ref_dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

			if (equal(ref_dcy, referencing_id))
			{
				RTEId	   *ref_dep = (RTEId *) list_nth(referencing_functional_dependencies, i);

				/* Create transitive dependencies */
				for (int j = 0; j < list_length(referenced_functional_dependencies); j += 2)
				{
					RTEId	   *refed_dep = (RTEId *) list_nth(referenced_functional_dependencies, j);
					RTEId	   *refed_dcy = (RTEId *) list_nth(referenced_functional_dependencies, j + 1);

					if (equal(refed_dep, referenced_id))
					{
						/*
						 * Add a new transitive dependency: ref_dep ->
						 * refed_dcy
						 */
						result = lappend(result, copyObject(ref_dep));
						result = lappend(result, copyObject(refed_dcy));
					}
				}
			}
		}
	}

	/*
	 * Part 3: Add all dependencies from referencing side if it's preserved in
	 * an outer join (LEFT join with fk_dir = FKDIR_FROM, RIGHT join with
	 * fk_dir = FKDIR_TO, or FULL join)
	 */
	if ((fk_dir == FKDIR_FROM && join_type == JOIN_LEFT) ||
		(fk_dir == FKDIR_TO && join_type == JOIN_RIGHT) ||
		join_type == JOIN_FULL)
	{
		for (int i = 0; i < list_length(referencing_functional_dependencies); i += 2)
		{
			RTEId	   *dep = (RTEId *) list_nth(referencing_functional_dependencies, i);
			RTEId	   *dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

			result = lappend(result, copyObject(dep));
			result = lappend(result, copyObject(dcy));
		}
	}

	/*
	 * Part 4: Add all dependencies from referenced side if it's preserved in
	 * an outer join (LEFT join with fk_dir = FKDIR_TO, RIGHT join with fk_dir
	 * = FKDIR_FROM, or FULL join)
	 */
	if ((fk_dir == FKDIR_TO && join_type == JOIN_LEFT) ||
		(fk_dir == FKDIR_FROM && join_type == JOIN_RIGHT) ||
		join_type == JOIN_FULL)
	{
		for (int i = 0; i < list_length(referenced_functional_dependencies); i += 2)
		{
			RTEId	   *dep = (RTEId *) list_nth(referenced_functional_dependencies, i);
			RTEId	   *dcy = (RTEId *) list_nth(referenced_functional_dependencies, i + 1);

			result = lappend(result, copyObject(dep));
			result = lappend(result, copyObject(dcy));
		}
	}

	return result;
}
