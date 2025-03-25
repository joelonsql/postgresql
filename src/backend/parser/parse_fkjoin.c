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

static Node *build_fk_join_on_clause(ParseState *pstate, List *referencingVars,
									 List *referencedVars);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid,
							 List *referencing_cols, List *referenced_cols);
static char *column_list_to_string(const List *columns);
static RangeTblEntry *drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
											 List **colnames_out, List *colnames,
											 int location);
static bool is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_cols);
static bool is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_cols);
static List *update_uniqueness_preservation(List *referencing_uniqueness_preservation,
											List *referenced_uniqueness_preservation,
											bool fk_cols_unique);
static List *update_functional_dependencies(List *referencing_functional_dependencies,
											RTEId * referencing_id,
											List *referenced_functional_dependencies,
											RTEId * referenced_id,
											bool fk_cols_not_null,
											JoinType join_type,
											ForeignKeyDirection fk_dir);

void
transformAndValidateForeignKeyJoin(ParseState *pstate, JoinExpr *join,
								   ParseNamespaceItem *r_nsitem,
								   List *l_namespace)
{
	ForeignKeyClause *fkjn = castNode(ForeignKeyClause, join->fkJoin);
	List	   *referencingVars = NIL;
	List	   *referencedVars = NIL;
	ListCell   *lc,
			   *rc;
	RangeTblEntry *referencing_rte,
			   *referenced_rte;
	RangeTblEntry *base_referencing_rte;
	RangeTblEntry *base_referenced_rte;
	ParseNamespaceItem *referencing_rel,
			   *referenced_rel,
			   *other_rel;
	List	   *referencing_cols,
			   *referenced_cols;
	List	   *referencing_base_cols = NIL;
	List	   *referenced_base_cols = NIL;
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;
	Oid			referencing_relid;
	Oid			referenced_relid;
	RTEId	   *referencing_id;
	RTEId	   *referenced_id;
	bool		found_fd = false;
	bool		fk_cols_unique = false;
	bool		fk_cols_not_null = false;

	other_rel = NULL;

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

	base_referencing_rte = drill_down_to_base_rel(pstate, referencing_rte,
												  &referencing_base_cols,
												  referencing_cols,
												  fkjn->location);
	base_referenced_rte = drill_down_to_base_rel(pstate, referenced_rte,
												 &referenced_base_cols,
												 referenced_cols,
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

	forboth(lc, referencing_cols, rc, referenced_cols)
	{
		char	   *referencing_col = strVal(lfirst(lc));
		char	   *referenced_col = strVal(lfirst(rc));
		Var		   *referencing_var;
		Var		   *referenced_var;

		referencing_var = (Var *) scanNSItemForColumn(pstate, referencing_rel, 0,
													  referencing_col, fkjn->location);
		referenced_var = (Var *) scanNSItemForColumn(pstate, referenced_rel, 0,
													 referenced_col, fkjn->location);

		referencingVars = lappend(referencingVars, referencing_var);
		referencedVars = lappend(referencedVars, referenced_var);

		referencing_attnums = lappend_int(referencing_attnums,
										  referencing_var->varattno);
		referenced_attnums = lappend_int(referenced_attnums,
										 referenced_var->varattno);
	}

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

		if (fd_dep->baserelindex == referenced_id->baserelindex &&
			fd_dep->fxid == referenced_id->fxid &&
			fd_dep->procnumber == referenced_id->procnumber &&
			fd_dcy->baserelindex == referenced_id->baserelindex &&
			fd_dcy->fxid == referenced_id->fxid &&
			fd_dcy->procnumber == referenced_id->procnumber)
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

	join->quals = build_fk_join_on_clause(pstate, referencingVars, referencedVars);

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
build_fk_join_on_clause(ParseState *pstate, List *referencingVars,
						List *referencedVars)
{
	Node	   *result;
	List	   *andargs = NIL;
	ListCell   *referencingvar,
			   *referencedvar;

	Assert(list_length(referencingVars) == list_length(referencedVars));

	forboth(referencingvar, referencingVars, referencedvar, referencedVars)
	{
		Var		   *referencing_var = (Var *) lfirst(referencingvar);
		Var		   *referenced_var = (Var *) lfirst(referencedvar);
		A_Expr	   *e;

		e = makeSimpleA_Expr(AEXPR_OP, "=",
							 (Node *) copyObject(referencing_var),
							 (Node *) copyObject(referenced_var),
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
	int			ncols = list_length(referencing_cols);
	Relation	rel = table_open(ConstraintRelationId, AccessShareLock);
	SysScanDesc scan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	Oid			fkoid = InvalidOid;
	int			i,
				j;
	AttrNumber *ref_attnums = palloc(sizeof(AttrNumber) * ncols);
	AttrNumber *refd_attnums = palloc(sizeof(AttrNumber) * ncols);
	ListCell   *lc;
	int			pos = 0;

	/* Convert referencing and referenced column lists to arrays of attnums */
	foreach(lc, referencing_cols)
		ref_attnums[pos++] = get_attnum(referencing_relid, strVal(lfirst(lc)));
	pos = 0;
	foreach(lc, referenced_cols)
		refd_attnums[pos++] = get_attnum(referenced_relid, strVal(lfirst(lc)));

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
			nkeys != ncols)
			continue;

		conkey = (int16 *) ARR_DATA_PTR(conkey_arr);
		confkey = (int16 *) ARR_DATA_PTR(confkey_arr);

		/*
		 * Check if each fk pair (conkey[i], confkey[i]) matches some
		 * (ref_attnums[j], refd_attnums[j])
		 */
		for (i = 0; i < nkeys && found; i++)
		{
			bool		match = false;

			for (j = 0; j < ncols && !match; j++)
				if (ref_attnums[j] == conkey[i] && refd_attnums[j] == confkey[i])
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
	pfree(ref_attnums);
	pfree(refd_attnums);

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
					   List **colnames_out, List *colnames,
					   int location)
{
	RangeTblEntry *base_rte = NULL;
	Query	   *query = NULL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);

				switch (rel->rd_rel->relkind)
				{
					case RELKIND_VIEW:
						query = get_view_query(rel);
						break;

					case RELKIND_RELATION:
						*colnames_out = colnames;
						base_rte = rte;
						break;

					default:
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins involving relation of type '%c' are not supported",
										rel->rd_rel->relkind),
								 parser_errposition(pstate, location)));
				}

				table_close(rel, AccessShareLock);
			}
			break;

		case RTE_SUBQUERY:
			query = rte->subquery;
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

				query = castNode(Query, cte->ctequery);
			}
			break;

		case RTE_JOIN:
			{
				ListCell   *lc_col;
				Node	   *aliasnode;
				RangeTblEntry *childrte = NULL;
				List	   *child_colnames = NIL;

				/*
				 * For each requested column, find its position in the join
				 * RTE's output (erefname), then locate the corresponding Var
				 * in joinaliasvars. This Var references one of the input
				 * relations of the join. We then recursively resolve that
				 * down to a base rel.
				 */
				foreach(lc_col, colnames)
				{
					char	   *colname = strVal(lfirst(lc_col));
					int			colpos = 0;
					int			matches = 0;
					ListCell   *lc_alias;
					Var		   *aliasvar;
					RangeTblEntry *aliasrte;

					/*
					 * Locate the requested column in the join's output
					 * aliases and check for ambiguity at the same time
					 */
					foreach(lc_alias, rte->eref->colnames)
					{
						char	   *aliasname = strVal(lfirst(lc_alias));

						if (strcmp(aliasname, colname) == 0)
						{
							if (matches == 0)	/* First match */
								colpos = foreach_current_index(lc_alias);
							matches++;
						}
					}

					if (matches == 0)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column reference \"%s\" not found", colname),
								 parser_errposition(pstate, location)));

					if (matches > 1)
						ereport(ERROR,
								(errcode(ERRCODE_AMBIGUOUS_COLUMN),
								 errmsg("column reference \"%s\" is ambiguous", colname),
								 parser_errposition(pstate, location)));

					aliasnode = list_nth(rte->joinaliasvars, colpos);
					if (!IsA(aliasnode, Var))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins require direct column references, found expression"),
								 parser_errposition(pstate, location)));

					aliasvar = castNode(Var, aliasnode);

					aliasrte = rt_fetch(aliasvar->varno, pstate->p_rtable);

					/* Check that all columns map to the same rte */
					if (childrte == NULL)
						childrte = aliasrte;
					else if (childrte != aliasrte)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("key columns must all come from the same table"),
								 parser_errposition(pstate, location)));

					child_colnames = lappend(child_colnames, makeString(get_rte_attribute_name(childrte, aliasvar->varattno)));
				}

				base_rte = drill_down_to_base_rel(pstate,
												  childrte,
												  colnames_out,
												  child_colnames,
												  location);

				return base_rte;
			}
			break;

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));
	}

	if (query)
	{
		RangeTblEntry *trunk_rte = NULL;
		List	   *base_colnames = NIL;
		Index		first_varno = InvalidOid;
		ListCell   *lc_colname;

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

		/*
		 * Determine the trunk_rte, which is the relation in query->targetList
		 * the colaliases refer to, which must be one and the same.
		 */
		foreach(lc_colname, colnames)
		{
			char	   *colname = strVal(lfirst(lc_colname));
			TargetEntry *matching_tle = NULL;
			int			matches = 0;
			Var		   *var;
			char	   *base_colname;
			ListCell   *lc_tle,
					   *lc_alias;

			lc_alias = list_head(rte->eref->colnames);

			foreach(lc_tle, query->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc_tle);

				if (tle->resjunk)
					continue;

				if (strcmp(strVal(lfirst(lc_alias)), colname) == 0)
				{
					matches++;
					matching_tle = tle;
				}

				lc_alias = lnext(rte->eref->colnames, lc_alias);
			}

			if (matches == 0)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column reference \"%s\" not found", colname),
						 parser_errposition(pstate, location)));
			else if (matches > 1)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_COLUMN),
						 errmsg("column reference \"%s\" is ambiguous", colname),
						 parser_errposition(pstate, location)));

			Assert(matching_tle != NULL);

			if (!IsA(matching_tle->expr, Var))
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("target entry \"%s\" is an expression, not a direct column reference",
								matching_tle->resname),
						 parser_errposition(pstate, location)));

			var = (Var *) matching_tle->expr;

			if (first_varno == InvalidOid)
			{
				first_varno = var->varno;
				trunk_rte = rt_fetch(first_varno, query->rtable);
			}
			else if (first_varno != var->varno)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_TABLE),
						 errmsg("key columns must all come from the same table"),
						 parser_errposition(pstate,
											exprLocation((Node *) matching_tle->expr))));

			base_colname = get_rte_attribute_name(trunk_rte, var->varattno);
			base_colnames = lappend(base_colnames, makeString(base_colname));
		}

		Assert(trunk_rte != NULL);

		/*
		 * Once the trunk_rte is determined, we drill down to the base
		 * relation, which is then returned.
		 */
		base_rte = drill_down_to_base_rel(pstate, trunk_rte, colnames_out,
										  base_colnames, location);

	}

	return base_rte;
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
is_referencing_cols_unique(Oid referencing_relid, List *referencing_base_cols)
{
	Relation	rel;
	List	   *indexoidlist;
	ListCell   *indexoidscan;
	bool		result = false;
	int			natts;
	AttrNumber *attnums;
	ListCell   *lc;
	int			i;

	/* Convert column names to attribute numbers */
	natts = list_length(referencing_base_cols);
	attnums = (AttrNumber *) palloc(natts * sizeof(AttrNumber));

	i = 0;
	foreach(lc, referencing_base_cols)
	{
		char	   *colname = strVal(lfirst(lc));

		attnums[i++] = get_attnum(referencing_relid, colname);
	}

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
		for (i = 0; i < natts; i++)
		{
			bool		col_found = false;

			for (int j = 0; j < nindexattrs; j++)
			{
				if (attnums[i] == indexForm->indkey.values[j])
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
	pfree(attnums);

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
is_referencing_cols_not_null(Oid referencing_relid, List *referencing_base_cols)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ListCell   *lc;
	bool		all_not_null = true;

	/* Open the relation to get its tuple descriptor */
	rel = table_open(referencing_relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	/* Check each column for NOT NULL constraint */
	foreach(lc, referencing_base_cols)
	{
		char	   *colname = strVal(lfirst(lc));
		AttrNumber	attnum;
		Form_pg_attribute attr;

		/* Get the attribute number for this column */
		attnum = get_attnum(referencing_relid, colname);

		/* Get attribute info */
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
							   RTEId * referencing_id,
							   List *referenced_functional_dependencies,
							   RTEId * referenced_id,
							   bool fk_cols_not_null,
							   JoinType join_type,
							   ForeignKeyDirection fk_dir)
{
	List	   *result = NIL;
	int			i,
				j;

	/* Part 1: Chain dependencies through the join if FK cols are NOT NULL */
	if (fk_cols_not_null)
	{
		/*
		 * First check if (referenced_id, referenced_id) is in
		 * referenced_functional_dependencies
		 */
		bool		referenced_self_dep_exists = false;

		for (i = 0; i < list_length(referenced_functional_dependencies); i += 2)
		{
			RTEId	   *ref_dep = (RTEId *) list_nth(referenced_functional_dependencies, i);
			RTEId	   *ref_dcy = (RTEId *) list_nth(referenced_functional_dependencies, i + 1);

			if (ref_dep->baserelindex == referenced_id->baserelindex &&
				ref_dep->fxid == referenced_id->fxid &&
				ref_dep->procnumber == referenced_id->procnumber &&
				ref_dcy->baserelindex == referenced_id->baserelindex &&
				ref_dcy->fxid == referenced_id->fxid &&
				ref_dcy->procnumber == referenced_id->procnumber)
			{
				referenced_self_dep_exists = true;
				break;
			}
		}

		/* If self-dependency exists, process the dependencies */
		if (referenced_self_dep_exists)
		{
			/* Loop 1: Find all items where dcy_id matches referencing_id */
			for (i = 0; i < list_length(referencing_functional_dependencies); i += 2)
			{
				RTEId	   *ref_dep = (RTEId *) list_nth(referencing_functional_dependencies, i);
				RTEId	   *ref_dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

				if (ref_dcy->baserelindex == referencing_id->baserelindex &&
					ref_dcy->fxid == referencing_id->fxid &&
					ref_dcy->procnumber == referencing_id->procnumber)
				{
					/*
					 * Loop 2: Find all items where dep_id matches Loop 1's
					 * dep_id and add them to the result
					 */
					for (j = 0; j < list_length(referencing_functional_dependencies); j += 2)
					{
						RTEId	   *source_dep = (RTEId *) list_nth(referencing_functional_dependencies, j);
						RTEId	   *source_dcy = (RTEId *) list_nth(referencing_functional_dependencies, j + 1);

						if (source_dep->baserelindex == ref_dep->baserelindex &&
							source_dep->fxid == ref_dep->fxid &&
							source_dep->procnumber == ref_dep->procnumber)
						{
							RTEId	   *dep_copy = makeNode(RTEId);
							RTEId	   *dcy_copy = makeNode(RTEId);

							dep_copy->fxid = source_dep->fxid;
							dep_copy->baserelindex = source_dep->baserelindex;
							dep_copy->procnumber = source_dep->procnumber;

							dcy_copy->fxid = source_dcy->fxid;
							dcy_copy->baserelindex = source_dcy->baserelindex;
							dcy_copy->procnumber = source_dcy->procnumber;

							result = lappend(result, dep_copy);
							result = lappend(result, dcy_copy);
						}
					}
				}
			}
		}

		/* Part 2: Create transitive dependencies */
		for (i = 0; i < list_length(referencing_functional_dependencies); i += 2)
		{
			RTEId	   *ref_dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

			if (ref_dcy->baserelindex == referencing_id->baserelindex &&
				ref_dcy->fxid == referencing_id->fxid &&
				ref_dcy->procnumber == referencing_id->procnumber)
			{
				RTEId	   *ref_dep = (RTEId *) list_nth(referencing_functional_dependencies, i);

				/* Create transitive dependencies */
				for (j = 0; j < list_length(referenced_functional_dependencies); j += 2)
				{
					RTEId	   *refed_dep = (RTEId *) list_nth(referenced_functional_dependencies, j);
					RTEId	   *refed_dcy = (RTEId *) list_nth(referenced_functional_dependencies, j + 1);

					if (refed_dep->baserelindex == referenced_id->baserelindex &&
						refed_dep->fxid == referenced_id->fxid &&
						refed_dep->procnumber == referenced_id->procnumber)
					{
						/*
						 * Add a new transitive dependency: ref_dep ->
						 * refed_dcy
						 */
						RTEId	   *dep_copy = makeNode(RTEId);
						RTEId	   *dcy_copy = makeNode(RTEId);

						dep_copy->fxid = ref_dep->fxid;
						dep_copy->baserelindex = ref_dep->baserelindex;
						dep_copy->procnumber = ref_dep->procnumber;

						dcy_copy->fxid = refed_dcy->fxid;
						dcy_copy->baserelindex = refed_dcy->baserelindex;
						dcy_copy->procnumber = refed_dcy->procnumber;

						result = lappend(result, dep_copy);
						result = lappend(result, dcy_copy);
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
		for (i = 0; i < list_length(referencing_functional_dependencies); i += 2)
		{
			RTEId	   *dep = (RTEId *) list_nth(referencing_functional_dependencies, i);
			RTEId	   *dcy = (RTEId *) list_nth(referencing_functional_dependencies, i + 1);

			RTEId	   *dep_copy = makeNode(RTEId);
			RTEId	   *dcy_copy = makeNode(RTEId);

			dep_copy->fxid = dep->fxid;
			dep_copy->baserelindex = dep->baserelindex;
			dep_copy->procnumber = dep->procnumber;

			dcy_copy->fxid = dcy->fxid;
			dcy_copy->baserelindex = dcy->baserelindex;
			dcy_copy->procnumber = dcy->procnumber;

			result = lappend(result, dep_copy);
			result = lappend(result, dcy_copy);
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
		for (i = 0; i < list_length(referenced_functional_dependencies); i += 2)
		{
			RTEId	   *dep = (RTEId *) list_nth(referenced_functional_dependencies, i);
			RTEId	   *dcy = (RTEId *) list_nth(referenced_functional_dependencies, i + 1);

			RTEId	   *dep_copy = makeNode(RTEId);
			RTEId	   *dcy_copy = makeNode(RTEId);

			dep_copy->fxid = dep->fxid;
			dep_copy->baserelindex = dep->baserelindex;
			dep_copy->procnumber = dep->procnumber;

			dcy_copy->fxid = dcy->fxid;
			dcy_copy->baserelindex = dcy->baserelindex;
			dcy_copy->procnumber = dcy->procnumber;

			result = lappend(result, dep_copy);
			result = lappend(result, dcy_copy);
		}
	}

	return result;
}
