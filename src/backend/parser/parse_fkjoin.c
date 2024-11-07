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
static Oid	drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
								   List **colnames_out, List *colnames,
								   bool is_referenced, int location);
static Oid	validate_and_resolve_derived_rel(ParseState *pstate, Query *query,
											 RangeTblEntry *rte,
											 List *colnames,
											 List **colnames_out,
											 bool is_referenced, int location);
static void validate_derived_rel_joins(ParseState *pstate, Query *query,
									   JoinExpr *join, RangeTblEntry *trunk_rte);

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

	referencing_relid = drill_down_to_base_rel(pstate, referencing_rte,
											   &referencing_base_cols,
											   referencing_cols, false,
											   fkjn->location);
	referenced_relid = drill_down_to_base_rel(pstate, referenced_rte,
											  &referenced_base_cols,
											  referenced_cols, true,
											  fkjn->location);

	Assert(referencing_relid != InvalidOid && referenced_relid != InvalidOid);

	fkoid = find_foreign_key(referencing_relid, referenced_relid,
							 referencing_base_cols, referenced_base_cols);

	if (fkoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
						referencing_rte->eref->aliasname,
						column_list_to_string(referencing_cols),
						referenced_rte->eref->aliasname,
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

	join->quals = build_fk_join_on_clause(pstate, referencingVars, referencedVars);

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
	HeapTuple	tuple;
	ScanKeyData skey[1];
	SysScanDesc scan;
	Relation	relation;
	Oid			fkoid = InvalidOid;

	relation = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(referencing_relid));

	scan = systable_beginscan(relation, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tuple = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tuple);
		Datum		conkey_datum;
		Datum		confkey_datum;
		bool		conkey_isnull;
		bool		confkey_isnull;
		ArrayType  *conkey_array;
		ArrayType  *confkey_array;
		int16	   *conkey;
		int16	   *confkey;
		int			nkeys;
		bool		found = true;

		if (con->contype != CONSTRAINT_FOREIGN ||
			con->confrelid != referenced_relid)
			continue;

		conkey_datum = SysCacheGetAttr(CONSTROID, tuple,
									   Anum_pg_constraint_conkey,
									   &conkey_isnull);
		confkey_datum = SysCacheGetAttr(CONSTROID, tuple,
										Anum_pg_constraint_confkey,
										&confkey_isnull);

		if (conkey_isnull || confkey_isnull)
			continue;

		conkey_array = DatumGetArrayTypeP(conkey_datum);
		confkey_array = DatumGetArrayTypeP(confkey_datum);

		nkeys = ArrayGetNItems(ARR_NDIM(conkey_array), ARR_DIMS(conkey_array));

		if (nkeys != ArrayGetNItems(ARR_NDIM(confkey_array),
									ARR_DIMS(confkey_array)))
			continue;

		if (nkeys != list_length(referencing_cols) ||
			nkeys != list_length(referenced_cols))
			continue;

		conkey = (int16 *) ARR_DATA_PTR(conkey_array);
		confkey = (int16 *) ARR_DATA_PTR(confkey_array);

		for (int i = 0; i < nkeys; i++)
		{
			char	   *ref_col = strVal(list_nth(referencing_cols, i));
			char	   *refd_col = strVal(list_nth(referenced_cols, i));
			AttrNumber	ref_attnum = get_attnum(referencing_relid, ref_col);
			AttrNumber	refd_attnum = get_attnum(referenced_relid, refd_col);

			if (ref_attnum == InvalidAttrNumber ||
				refd_attnum == InvalidAttrNumber ||
				conkey[i] != ref_attnum ||
				confkey[i] != refd_attnum)
			{
				found = false;
				break;
			}
		}

		if (found)
		{
			fkoid = con->oid;
			break;
		}
	}

	systable_endscan(scan);
	table_close(relation, AccessShareLock);

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
		Node	   *name = (Node *) lfirst(l);

		if (!first)
			appendStringInfoString(&string, ", ");

		appendStringInfoString(&string, strVal(name));

		first = false;
	}

	return string.data;
}

/*
 * drill_down_to_base_rel
 *		Resolves the base relation from a potentially derived relation
 */
static Oid
drill_down_to_base_rel(ParseState *pstate, RangeTblEntry *rte,
					   List **colnames_out, List *colnames,
					   bool is_referenced, int location)
{
	Oid			base_relid;
	Query	   *query = NULL;

	switch (rte->rtekind)
	{
		case RTE_RELATION:
			{
				Relation	rel = table_open(rte->relid, AccessShareLock);
				bool		is_view = rel->rd_rel->relkind == RELKIND_VIEW;

				if (is_view)
					query = get_view_query(rel);
				else if (is_referenced && rel->rd_rel->relrowsecurity)
					ereport(ERROR,
							(errcode(ERRCODE_SYNTAX_ERROR),
							 errmsg("cannot use table \"%s\" with row level security enabled as referenced table in foreign key join",
									get_rel_name(rel->rd_id)),
							 errdetail("Using a table with row level security as the referenced table would violate referential integrity."),
							 parser_errposition(pstate, location)));
				else
				{
					*colnames_out = colnames;
					base_relid = rte->relid;
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

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));
	}

	if (query)
		base_relid = validate_and_resolve_derived_rel(pstate, query,
													  rte,
													  colnames,
													  colnames_out,
													  is_referenced,
													  location);

	return base_relid;
}

/*
 * validate_and_resolve_derived_rel
 *		Ensures that derived tables uphold virtual foreign key integrity
 */
static Oid
validate_and_resolve_derived_rel(ParseState *pstate, Query *query, RangeTblEntry *rte,
								 List *colnames, List **colnames_out,
								 bool is_referenced, int location)
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
	 * Determine the trunk_rte, which is the relation in query->targetList the
	 * colaliases refer to, which must be one and the same.
	 */
	foreach(lc_colname, colnames)
	{
		char	   *colname = strVal(lfirst(lc_colname));
		TargetEntry *matching_tle = NULL;
		int			matches = 0;
		TargetEntry *tle;
		Var		   *var;
		char	   *base_colname;
		ListCell   *lc_tle,
				   *lc_alias;

		lc_alias = list_head(rte->eref->colnames);

		foreach(lc_tle, query->targetList)
		{
			TargetEntry *cur_tle = (TargetEntry *) lfirst(lc_tle);

			if (cur_tle->resjunk)
				continue;

			if (strcmp(strVal(lfirst(lc_alias)), colname) == 0)
			{
				matches++;
				matching_tle = cur_tle;
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

		tle = matching_tle;

		if (!IsA(tle->expr, Var))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("target entry \"%s\" is an expression, not a direct column reference",
							tle->resname),
					 parser_errposition(pstate, location)));

		var = (Var *) tle->expr;

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
										exprLocation((Node *) tle->expr))));

		base_colname = get_rte_attribute_name(trunk_rte, var->varattno);
		base_colnames = lappend(base_colnames, makeString(base_colname));
	}

	Assert(trunk_rte != NULL);

	/*
	 * If this is the referenced side, we need to ensure it's not filtered,
	 * and if there are any joins, they must all use the trunk_rte as their
	 * referencing table, and the referencing columns must not be nullable,
	 * since otherwise the virtual foreign key integrity would not be upheld.
	 */
	if (is_referenced)
	{
		if (query->jointree->quals != NULL ||
			query->limitOffset != NULL ||
			query->limitCount != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("cannot use filtered query as referenced table in foreign key join"),
					 errdetail("Using a filtered query as the referenced table would violate referential integrity."),
					 parser_errposition(pstate, location)));

		if (list_length(query->rtable) > 1 &&
			IsA(query->jointree->fromlist, List))
		{
			ListCell   *lc;

			foreach(lc, query->jointree->fromlist)
			{
				JoinExpr   *join = castNode(JoinExpr, lfirst(lc));

				validate_derived_rel_joins(pstate, query, join, trunk_rte);
			}
		}

	}

	/*
	 * Once the trunk_rte is determined, we drill down to the base relation,
	 * which is then returned.
	 */
	return drill_down_to_base_rel(pstate, trunk_rte, colnames_out,
								  base_colnames, is_referenced, location);
}

/*
 * validate_derived_rel_joins
 *		Ensures that all joins uphold virtual foreign key integrity
 */
static void
validate_derived_rel_joins(ParseState *pstate, Query *query, JoinExpr *join,
						   RangeTblEntry *trunk_rte)
{
	ForeignKeyJoinNode *fkjn;
	RangeTblEntry *referencing_rte;
	List	   *referencing_attnums;
	ListCell   *lc;
	List	   *base_colnames = NIL;
	List	   *colaliases = NIL;
	Oid			base_relid;

	if (join == NULL)
		return;

	if (join->fkJoin == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("virtual foreign key constraint violation"),
				 errdetail("The derived table contains a join that is not a foreign key join")));

	Assert(IsA(join->fkJoin, ForeignKeyJoinNode));
	fkjn = (ForeignKeyJoinNode *) join->fkJoin;

	Assert(query->rtable != NIL);
	Assert(fkjn->referencingVarno > 0 &&
		   fkjn->referencingVarno <= list_length(query->rtable) &&
		   fkjn->referencedVarno > 0 &&
		   fkjn->referencedVarno <= list_length(query->rtable));

	referencing_rte = rt_fetch(fkjn->referencingVarno, query->rtable);
	Assert(referencing_rte != NULL);

	if (trunk_rte != referencing_rte)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("virtual foreign key constraint violation"),
				 errdetail("Referenced columns target a non-referencing table in derived table, violating uniqueness")));

	referencing_attnums = fkjn->referencingAttnums;
	foreach(lc, referencing_attnums)
	{
		int			attnum = lfirst_int(lc);
		char	   *colname = get_rte_attribute_name(referencing_rte, attnum);

		colaliases = lappend(colaliases, makeString(colname));
	}
	base_relid = drill_down_to_base_rel(pstate, referencing_rte,
										&base_colnames, colaliases, false,
										-1);

	foreach(lc, base_colnames)
	{
		char	   *colname = strVal(lfirst(lc));
		HeapTuple	tuple;
		Form_pg_attribute attr;
		AttrNumber	attnum;

		attnum = get_attnum(base_relid, colname);
		if (attnum == InvalidAttrNumber)
			elog(ERROR, "cache lookup failed for column \"%s\" of relation %u",
				 colname, base_relid);

		tuple = SearchSysCache2(ATTNUM,
								ObjectIdGetDatum(base_relid),
								Int16GetDatum(attnum));

		if (!HeapTupleIsValid(tuple))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 attnum, base_relid);

		attr = (Form_pg_attribute) GETSTRUCT(tuple);

		if (!attr->attnotnull)
		{
			ReleaseSysCache(tuple);
			ereport(ERROR,
					(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
					 errmsg("virtual foreign key constraint violation"),
					 errdetail("Nullable columns in derived table's referencing relation violate referential integrity")));
		}

		ReleaseSysCache(tuple);
	}

	return;
}
