/*-------------------------------------------------------------------------
 *
 * parse_fkjoin.c
 *	  Handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
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
#include "parser/analyze.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "parser/parse_fkjoin.h"
#include "rewrite/rewriteHandler.h"

static Node *transformForeignKeyJoin(ParseState *pstate, List *referencedVars, List *referencingVars);
static Oid	get_base_relid_from_rte(ParseState *pstate, RangeTblEntry *rte, List **colnames_out, List *colaliases, bool is_referenced);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid, List *referencing_cols, List *referenced_cols);
static char *ColumnListToString(const List *columns);
static Var *get_var_for_column(ParseState *pstate, RangeTblEntry *rte, int varno, char *colname, int location);
static List *find_target_columns(List *colaliases, List *targetList);
static RangeTblEntry *find_rte_with_target_columns(ParseState *pstate, Query *query, List *target_colnames,
												   List *colaliases, List **out_colnames);
static List *map_columns_to_base(List *colaliases, List *targetList, RangeTblEntry *sub_rte);
static Oid	process_query_rte(ParseState *pstate, Query *query, List *colaliases, List **colnames_out, bool is_referenced);

/*
 * transformForeignKeyJoinNode
 *	  Transform a foreign key join node into a join expression with appropriate
 *	  join conditions based on the foreign key relationship between tables.
 *
 * The function takes a JoinExpr node with fkJoin field set, identifies the
 * referencing and referenced relations, validates the foreign key relationship,
 * and constructs appropriate join conditions based on the matching columns.
 *
 * Note: this function overwrites both j->quals with the constructed join conditions
 * and j->fkJoin with a new ForeignKeyJoinNode containing the validated foreign key
 * information.
 */
extern Node *
transformForeignKeyJoinNode(ParseState *pstate, JoinExpr *j, ParseNamespaceItem *r_nsitem, List *l_namespace)
{
	ForeignKeyClause *fkjn = castNode(ForeignKeyClause, j->fkJoin);
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

	/* Find the referenced relation by alias */
	foreach(lc, l_namespace)
	{
		ParseNamespaceItem *nsi = (ParseNamespaceItem *) lfirst(lc);

		/* Ignore columns-only items */
		if (!nsi->p_rel_visible)
			continue;

		if (strcmp(nsi->p_names->aliasname, fkjn->refAlias) == 0)
		{
			if (other_rel)
				ereport(ERROR,
						(errcode(ERRCODE_AMBIGUOUS_ALIAS),
						 errmsg("table reference \"%s\" is ambiguous",
								fkjn->refAlias),
						 parser_errposition(pstate, fkjn->location)));
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
		/* The foreign key is FROM the right table to the left table */
		referencing_rel = other_rel;	/* Left table is the referencing table */
		referenced_rel = r_nsitem;	/* Right table is the referenced table */
		referencing_cols = fkjn->refCols;	/* Columns from referencing table */
		referenced_cols = fkjn->localCols;	/* Columns from referenced table */
	}
	else
	{
		/* The foreign key is TO the right table from the left table */
		referenced_rel = other_rel;
		referencing_rel = r_nsitem;
		referenced_cols = fkjn->refCols;	/* Columns from referenced table */
		referencing_cols = fkjn->localCols; /* Columns from referencing table */
	}

	/* Get RangeTblEntries for FK and ref tables */
	referencing_rte = rt_fetch(referencing_rel->p_rtindex, pstate->p_rtable);
	referenced_rte = rt_fetch(referenced_rel->p_rtindex, pstate->p_rtable);

	/* Adjust for subqueries */
	referencing_relid = get_base_relid_from_rte(pstate, referencing_rte, &referencing_base_cols, referencing_cols, false);
	referenced_relid = get_base_relid_from_rte(pstate, referenced_rte, &referenced_base_cols, referenced_cols, true);

	if (referencing_relid == InvalidOid || referenced_relid == InvalidOid)
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving complex subqueries are not supported"),
				 parser_errposition(pstate, fkjn->location)));
	}

	/*
	 * Proceed with finding the foreign key constraint using base column names
	 */
	fkoid = find_foreign_key(referencing_relid, referenced_relid,
							 referencing_base_cols, referenced_base_cols);

	/* Check if foreign key constraint exists */
	if (fkoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
						referencing_rte->eref->aliasname, ColumnListToString(referencing_cols),
						referenced_rte->eref->aliasname, ColumnListToString(referenced_cols)),
				 parser_errposition(pstate, fkjn->location)));

	forboth(lc, referencing_cols, rc, referenced_cols)
	{
		char	   *referencing_col = strVal(lfirst(lc));
		char	   *referenced_col = strVal(lfirst(rc));
		Var		   *referencing_var;
		Var		   *referenced_var;

		referencing_var = get_var_for_column(pstate, referencing_rte, referencing_rel->p_rtindex, referencing_col, fkjn->location);
		referenced_var = get_var_for_column(pstate, referenced_rte, referenced_rel->p_rtindex, referenced_col, fkjn->location);

		/* Mark vars for SELECT privilege */
		markVarForSelectPriv(pstate, referencing_var);
		markVarForSelectPriv(pstate, referenced_var);

		/* Add to lists */
		referencingVars = lappend(referencingVars, referencing_var);
		referencedVars = lappend(referencedVars, referenced_var);

		/* Collect attribute numbers */
		referencing_attnums = lappend_int(referencing_attnums, referencing_var->varattno);
		referenced_attnums = lappend_int(referenced_attnums, referenced_var->varattno);
	}

	/* Generate the join qualifications */
	j->quals = transformForeignKeyJoin(pstate, referencingVars, referencedVars);

	/* Create the ForeignKeyJoinNode */
	fkjn_node = makeNode(ForeignKeyJoinNode);

	fkjn_node->fkdir = fkjn->fkdir;
	fkjn_node->referencingVarno = referencing_rel->p_rtindex;
	fkjn_node->referencingAttnums = referencing_attnums;
	fkjn_node->referencedVarno = referenced_rel->p_rtindex;
	fkjn_node->referencedAttnums = referenced_attnums;
	fkjn_node->constraint = fkoid;

	/* Overwrite j->fkJoin with the new ForeignKeyJoinNode */
	j->fkJoin = (Node *) fkjn_node;
}

/* transformForeignKeyJoin()
 *	  Build a complete ON clause from a foreign key join specification.
 *	  We are given lists of nodes representing referencing and referenced columns.
 *	  Result is a transformed qualification expression.
 */
static Node *
transformForeignKeyJoin(ParseState *pstate,
						List *referencingVars, List *referencedVars)
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

		/* Now create the referencing_var = referenced_var join condition */
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
 * get_base_relid_from_rte
 *	  Given an RTE, recursively find the base relation OID and column names
 *	  that are referenced by the RTE's target list.
 *
 * pstate - current parse state
 * rte - range table entry to process
 * colnames_out - receives list of base relation column names
 * colaliases - list of column aliases from outer query
 *
 * Returns the OID of the base relation, or InvalidOid if not found or not supported.
 * The base relation column names are returned in colnames_out.
 *
 * This handles base relations, views, subqueries and CTEs by recursively traversing
 * their definitions to find the underlying base relation and columns.
 */
static Oid
get_base_relid_from_rte(ParseState *pstate, RangeTblEntry *rte, List **colnames_out, List *colaliases, bool is_referenced)
{
	if (rte->rtekind == RTE_RELATION)
	{
		/* Base relation or view */
		Relation	rel = table_open(rte->relid, AccessShareLock);
		bool		is_view = rel->rd_rel->relkind == RELKIND_VIEW;

		table_close(rel, AccessShareLock);

		if (!is_view)
		{
			/* Base relation */
			*colnames_out = colaliases;
			return rte->relid;
		}
		else
		{
			/* View - get base relation from view definition */
			Relation	viewrel = table_open(rte->relid, AccessShareLock);
			Query	   *viewQuery = get_view_query(viewrel);
			Oid			base_relid;

			table_close(viewrel, AccessShareLock);

			/* Check for filtering on referenced side */
			if (is_referenced && viewQuery->jointree->quals != NULL)
				ereport(ERROR,
						(errcode(ERRCODE_SYNTAX_ERROR),
						 errmsg("cannot use filtered view \"%s\" as referenced table in foreign key join",
								get_rel_name(rte->relid)),
						 errdetail("Using a filtered view as the referenced table would violate referential integrity.")));

			base_relid = process_query_rte(pstate, viewQuery, colaliases, colnames_out, is_referenced);
			return base_relid;
		}
	}
	else if (rte->rtekind == RTE_SUBQUERY)
	{
		return process_query_rte(pstate, rte->subquery, colaliases, colnames_out, is_referenced);
	}
	else if (rte->rtekind == RTE_CTE)
	{
		CommonTableExpr *cte = NULL;
		ListCell   *lc;

		/* Find the CTE definition */
		foreach(lc, pstate->p_ctenamespace)
		{
			cte = (CommonTableExpr *) lfirst(lc);
			if (strcmp(cte->ctename, rte->ctename) == 0)
				break;
			cte = NULL;
		}

		if (!cte)
			return InvalidOid;	/* CTE not found */

		return process_query_rte(pstate, castNode(Query, cte->ctequery),
								 colaliases, colnames_out, is_referenced);
	}
	else
	{
		/* Unsupported RTE kind for FK join */
		return InvalidOid;
	}
}

/*
 * ColumnListToString
 *	Utility routine to convert a list of column names into a comma-separated
 *	string.
 *
 * This is used primarily to form error messages, and so we do not quote
 * the list elements, for the sake of legibility.
 *
 * This function assumes all list elements are String values representing
 * column names.
 */
static char *
ColumnListToString(const List *columns)
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

		if (IsA(name, String))
			appendStringInfoString(&string, strVal(name));
		else
			elog(ERROR, "unexpected node type in column list: %d",
				 (int) nodeTag(name));

		first = false;
	}

	return string.data;
}

/*
 * find_foreign_key
 *	  Find a foreign key constraint between two relations that matches the
 *	  specified column lists.
 *
 * referencing_relid - OID of the referencing relation
 * referenced_relid - OID of the referenced relation
 * referencing_cols - list of column names in the referencing relation
 * referenced_cols - list of column names in the referenced relation
 *
 * Returns the OID of the matching foreign key constraint, or InvalidOid if
 * no match is found.
 *
 * This scans the system catalogs to find a foreign key constraint where:
 * - The referencing relation matches referencing_relid
 * - The referenced relation matches referenced_relid
 * - The referencing columns match referencing_cols in order
 * - The referenced columns match referenced_cols in order
 */
static Oid
find_foreign_key(Oid referencing_relid, Oid referenced_relid, List *referencing_cols, List *referenced_cols)
{
	HeapTuple	tuple;
	ScanKeyData skey[1];
	SysScanDesc scan;
	Relation	relation;
	Oid			fkoid = InvalidOid;

	relation = table_open(ConstraintRelationId, AccessShareLock);

	/* Scan for constraints on the referencing table */
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

		if (con->contype != CONSTRAINT_FOREIGN || con->confrelid != referenced_relid)
			continue;

		conkey_datum = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_conkey, &conkey_isnull);
		confkey_datum = SysCacheGetAttr(CONSTROID, tuple, Anum_pg_constraint_confkey, &confkey_isnull);

		if (conkey_isnull || confkey_isnull)
			continue;

		/* Convert Datum to ArrayType */
		conkey_array = DatumGetArrayTypeP(conkey_datum);
		confkey_array = DatumGetArrayTypeP(confkey_datum);

		/* Get the number of keys */
		nkeys = ArrayGetNItems(ARR_NDIM(conkey_array), ARR_DIMS(conkey_array));

		if (nkeys != ArrayGetNItems(ARR_NDIM(confkey_array), ARR_DIMS(confkey_array)))
			continue;

		if (nkeys != list_length(referencing_cols) || nkeys != list_length(referenced_cols))
			continue;

		/* Get the array data */
		conkey = (int16 *) ARR_DATA_PTR(conkey_array);
		confkey = (int16 *) ARR_DATA_PTR(confkey_array);

		for (int i = 0; i < nkeys; i++)
		{
			char	   *ref_col = strVal(list_nth(referencing_cols, i));
			char	   *refd_col = strVal(list_nth(referenced_cols, i));
			AttrNumber	ref_attnum = get_attnum(referencing_relid, ref_col);
			AttrNumber	refd_attnum = get_attnum(referenced_relid, refd_col);

			if (conkey[i] != ref_attnum || confkey[i] != refd_attnum)
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
 * get_var_for_column
 *	  Given a range table entry, variable number, and column name, construct
 *	  a Var node referencing that column.
 *
 * pstate - current parse state
 * rte - range table entry to reference
 * varno - RTE index in range table
 * colname - name of column to reference
 * location - parse location for error reporting
 *
 * Returns a Var node for the specified column, or throws an error if the
 * column cannot be found or referenced.
 *
 * This handles base relations and views by recursively traversing view
 * definitions to find the underlying base relation column. For views,
 * only simple column references are supported - expressions in the view
 * target list will result in an error.
 */
static Var *
get_var_for_column(ParseState *pstate, RangeTblEntry *rte, int varno, char *colname, int location)
{
	Var		   *var = NULL;
	int			attnum;
	int32		vartypmod;
	Oid			vartype;
	Oid			varcollation;

	if (rte->rtekind == RTE_RELATION)
	{
		/* Base relation or view */
		Relation	rel = table_open(rte->relid, AccessShareLock);
		bool		is_view = rel->rd_rel->relkind == RELKIND_VIEW;

		table_close(rel, AccessShareLock);

		if (!is_view)
		{
			/* Base relation */
			attnum = get_attnum(rte->relid, colname);
			if (attnum == InvalidAttrNumber)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist", colname),
						 parser_errposition(pstate, location)));
			/* Get type information */
			get_atttypetypmodcoll(rte->relid, attnum, &vartype, &vartypmod, &varcollation);
			var = makeVar(varno, attnum, vartype, vartypmod, varcollation, 0);
		}
		else
		{
			/* View - get base relation from view definition */
			Relation	viewrel = table_open(rte->relid, AccessShareLock);
			Query	   *viewQuery = get_view_query(viewrel);
			ListCell   *lc;

			table_close(viewrel, AccessShareLock);

			/* Find matching column in view's target list */
			foreach(lc, viewQuery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(lc);

				if (strcmp(tle->resname, colname) == 0)
				{
					if (!IsA(tle->expr, Var))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins involving expressions in views are not supported"),
								 parser_errposition(pstate, location)));

					/* Build a Var node referencing the view's output column */
					var = makeVar(varno,
								  tle->resno,
								  exprType((Node *) tle->expr),
								  exprTypmod((Node *) tle->expr),
								  exprCollation((Node *) tle->expr),
								  0);
					break;
				}
			}

			if (var == NULL)
				ereport(ERROR,
						(errcode(ERRCODE_UNDEFINED_COLUMN),
						 errmsg("column \"%s\" does not exist in view", colname),
						 parser_errposition(pstate, location)));
		}
	}
	else if (rte->rtekind == RTE_SUBQUERY)
	{
		/* Subquery */
		TargetEntry *tle = NULL;
		ListCell   *lc;

		foreach(lc, rte->subquery->targetList)
		{
			TargetEntry *sub_tle = (TargetEntry *) lfirst(lc);

			if (strcmp(sub_tle->resname, colname) == 0)
			{
				if (!IsA(sub_tle->expr, Var))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving expressions in subqueries are not supported"),
							 parser_errposition(pstate, location)));
				tle = sub_tle;
				break;
			}
		}
		if (!tle)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in subquery", colname),
					 parser_errposition(pstate, location)));
		/* Build a Var node referencing the subquery's output column */
		var = makeVar(varno,
					  tle->resno,
					  exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr),
					  0);
	}
	else if (rte->rtekind == RTE_CTE)
	{
		/* CTE */
		TargetEntry *tle = NULL;
		ListCell   *lc;
		CommonTableExpr *cte = NULL;
		ListCell   *lc_cte;

		/* Find the referenced CTE */
		foreach(lc_cte, pstate->p_ctenamespace)
		{
			cte = (CommonTableExpr *) lfirst(lc_cte);
			if (strcmp(cte->ctename, rte->ctename) == 0)
				break;
		}

		if (!cte)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("CTE \"%s\" does not exist", rte->ctename),
					 parser_errposition(pstate, location)));

		/* Find the matching column in the CTE's target list */
		foreach(lc, GetCTETargetList(cte))
		{
			TargetEntry *sub_tle = (TargetEntry *) lfirst(lc);

			if (strcmp(sub_tle->resname, colname) == 0)
			{
				if (!IsA(sub_tle->expr, Var))
					ereport(ERROR,
							(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							 errmsg("foreign key joins involving expressions in CTEs are not supported"),
							 parser_errposition(pstate, location)));
				tle = sub_tle;
				break;
			}
		}

		if (!tle)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in CTE \"%s\"", colname, rte->ctename),
					 parser_errposition(pstate, location)));

		/* Build a Var node referencing the CTE's output column */
		var = makeVar(varno,
					  tle->resno,
					  exprType((Node *) tle->expr),
					  exprTypmod((Node *) tle->expr),
					  exprCollation((Node *) tle->expr),
					  0);
	}
	else
	{
		/* Other RTE kinds are not supported for FK joins */
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving this type of relation are not supported"),
				 parser_errposition(pstate, location)));
	}
	return var;
}

/*
 * find_target_columns
 *	  Given a list of column aliases and a target list, find matching target
 *	  columns and return their names.
 *
 * Returns a list of column names from the target list that match the aliases.
 * Only non-junk columns are considered. The returned names are copies of the
 * target column names.
 */
static List *
find_target_columns(List *colaliases, List *targetList)
{
	List	   *target_colnames = NIL;
	ListCell   *lc_alias;

	foreach(lc_alias, colaliases)
	{
		char	   *alias_name = strVal(lfirst(lc_alias));
		ListCell   *lc_tle;

		foreach(lc_tle, targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc_tle);

			if (!tle->resjunk && strcmp(tle->resname, alias_name) == 0)
			{
				target_colnames = lappend(target_colnames, makeString(alias_name));
				break;
			}
		}
	}
	return target_colnames;
}

/*
 * find_rte_with_target_columns
 *	  Find a range table entry in a query that contains the specified target columns.
 *
 * This function searches through a query's range table entries (RTEs) to find one
 * that contains all the specified target columns. For each RTE, it checks if the
 * target list contains Var nodes referencing that RTE's columns that match the
 * target column names.
 *
 * pstate - current parse state
 * query - query to search in
 * target_colnames - list of column names to look for
 * colaliases - list of column aliases from outer query
 * out_colnames - receives list of base relation column names
 *
 * Returns the matching RTE if found, or NULL if no matching RTE exists.
 * The base relation column names are returned in out_colnames.
 */
static RangeTblEntry *
find_rte_with_target_columns(ParseState *pstate, Query *query, List *target_colnames,
							 List *colaliases, List **out_colnames)
{
	ListCell   *lc_rte;
	Index		rtindex = 1;	/* RTEs are 1-based */

	foreach(lc_rte, query->rtable)
	{
		RangeTblEntry *candidate_rte = (RangeTblEntry *) lfirst(lc_rte);
		List	   *candidate_cols = NIL;
		bool		has_target_cols = false;

		/* Check if this RTE contains our target columns */
		ListCell   *lc_tle;

		foreach(lc_tle, query->targetList)
		{
			TargetEntry *tle = (TargetEntry *) lfirst(lc_tle);
			ListCell   *lc_target;

			if (tle->resjunk)
				continue;

			foreach(lc_target, target_colnames)
			{
				if (strcmp(tle->resname, strVal(lfirst(lc_target))) == 0)
				{
					if (IsA(tle->expr, Var))
					{
						Var		   *var = (Var *) tle->expr;

						if (var->varno == rtindex)
						{
							has_target_cols = true;
							break;
						}
					}
				}
			}
			if (has_target_cols)
				break;
		}

		if (has_target_cols)
		{
			Oid			candidate_relid = get_base_relid_from_rte(pstate, candidate_rte,
																  &candidate_cols, colaliases, false);

			if (candidate_relid != InvalidOid)
			{
				*out_colnames = candidate_cols;
				return candidate_rte;
			}
		}
		rtindex++;
	}
	return NULL;
}

/*
 * map_columns_to_base
 *	  Maps column aliases from an outer query to the corresponding base column names
 *	  in a subquery or base relation.
 *
 * colaliases - list of column aliases from outer query
 * targetList - target list entries from inner query/relation
 * sub_rte - range table entry for inner query/relation
 *
 * Returns list of mapped base column names, or NIL if mapping fails. The mapping
 * will fail if:
 * - The alias names don't match the target list resnames
 * - Any target entry is an expression rather than a direct column reference
 */
static List *
map_columns_to_base(List *colaliases, List *targetList, RangeTblEntry *sub_rte)
{
	List	   *sub_colaliases = NIL;
	ListCell   *lc_alias,
			   *lc_tle;

	forboth(lc_alias, colaliases, lc_tle, targetList)
	{
		char	   *alias_name = strVal(lfirst(lc_alias));
		TargetEntry *tle = (TargetEntry *) lfirst(lc_tle);

		if (tle->resjunk)
			continue;

		if (strcmp(tle->resname, alias_name) != 0)
			return NIL;			/* Alias names do not match target list */

		if (IsA(tle->expr, Var))
		{
			/* Direct reference, collect var information */
			Var		   *var = (Var *) tle->expr;
			char	   *base_colname = get_rte_attribute_name(sub_rte, var->varattno);

			/* Collect the corresponding alias for recursive call */
			sub_colaliases = lappend(sub_colaliases, makeString(base_colname));
		}
		else
		{
			/* Expression, cannot process */
			return NIL;
		}
	}
	return sub_colaliases;
}

/*
 * process_query_rte
 *	  Process a Query RTE to find the base relation and column names referenced
 *	  by its target list.
 *
 * pstate - current parse state
 * query - the Query to process
 * colaliases - list of column aliases from outer query
 * colnames_out - receives list of base relation column names
 *
 * Returns the OID of the base relation, or InvalidOid if not found or not supported.
 * The base relation column names are returned in colnames_out.
 *
 * This handles subqueries and CTEs by recursively traversing their definitions
 * to find the underlying base relation and columns. Set operations are not supported.
 */
static Oid
process_query_rte(ParseState *pstate, Query *query, List *colaliases, List **colnames_out, bool is_referenced)
{
	RangeTblEntry *sub_rte = NULL;
	List	   *target_colnames;
	List	   *sub_colaliases;
	Oid			base_relid;

	/* Check if query contains set operations */
	if (query->setOperations != NULL)
		return InvalidOid;

	/* Only check for filtering if this is the referenced side */
	if (is_referenced && query->jointree->quals != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("cannot use filtered query as referenced table in foreign key join"),
				 errdetail("Using a filtered query as the referenced table would violate referential integrity.")));

	/* Find which columns we're looking for in the target list */
	target_colnames = find_target_columns(colaliases, query->targetList);

	/* Handle both single table and JOIN cases */
	if (list_length(query->rtable) == 1)
		sub_rte = (RangeTblEntry *) linitial(query->rtable);
	else						/* Must be one or more JOINs */
	{
		sub_rte = find_rte_with_target_columns(pstate, query, target_colnames,
											   colaliases, colnames_out);
		if (!sub_rte)
			return InvalidOid;
	}

	/* Map columns from outer query to inner query/base table */
	sub_colaliases = map_columns_to_base(colaliases, query->targetList, sub_rte);
	if (sub_colaliases == NIL)
		return InvalidOid;

	/* Recursively get base relid and column names */
	base_relid = get_base_relid_from_rte(pstate, sub_rte, colnames_out,
										 sub_colaliases, is_referenced);

	return base_relid;
}
