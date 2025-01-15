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

/* ----------
 * Uncomment the following to enable compilation of dump_A_list()
 * and dump_U_list() and to get a dump of A and U for each join.
 * ----------
#define FKJOINS_DEBUG
 */

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
									   RangeTblEntry *anchor_rte, int location);
static void validate_join_node(ParseState *pstate, Query *query, Node *node,
							   List **A, List **U, int location);
static bool check_columns_not_nullable(ParseState *pstate, RangeTblEntry *rte,
									   List *colaliases, int location);
static bool check_columns_unique(ParseState *pstate, RangeTblEntry *rte,
								 List *colaliases, int location);
static List *map_union(List *oldA, List *A_inner);
static const char *rte_aliasname(RangeTblEntry *rte);

#ifdef FKJOINS_DEBUG
static void dump_A_list(List *A, RangeTblEntry *new_rte);
static void dump_U_list(List *U, RangeTblEntry *new_rte);
#else
#define dump_A_list(A, new_rte)
#define dump_U_list(U, new_rte)
#endif

/*
 * transformAndValidateForeignKeyJoin
 *    Entry point for transforming a foreign key join
 */
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
 *    Constructs the ON clause for the foreign key join
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
 *    Searches the system catalogs to locate the foreign key constraint
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
			{
				if (ref_attnums[j] == conkey[i] && refd_attnums[j] == confkey[i])
					match = true;
			}

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
 *    Converts a list of column names to a comma-separated string
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
 *    Resolves the base relation from a potentially derived relation
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

				switch (rel->rd_rel->relkind)
				{
					case RELKIND_VIEW:
						query = get_view_query(rel);
						break;

					case RELKIND_RELATION:
						if (is_referenced && rel->rd_rel->relrowsecurity)
							ereport(ERROR,
									(errcode(ERRCODE_SYNTAX_ERROR),
									 errmsg("cannot use table \"%s\" with row level security enabled as referenced table in foreign key join",
											get_rel_name(rel->rd_id)),
									 errdetail("Using a table with row level security as the referenced table would violate referential integrity."),
									 parser_errposition(pstate, location)));
						*colnames_out = colnames;
						base_relid = rte->relid;
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
					bool		found = false;
					ListCell   *lc_alias;
					Var		   *aliasvar;
					RangeTblEntry *aliasrte;

					foreach(lc_alias, rte->eref->colnames)
					{
						char	   *aliasname = strVal(lfirst(lc_alias));

						if (strcmp(aliasname, colname) == 0)
						{
							found = true;
							break;
						}
						colpos++;
					}

					if (!found)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_COLUMN),
								 errmsg("column reference \"%s\" not found", colname),
								 parser_errposition(pstate, location)));

					aliasnode = list_nth(rte->joinaliasvars, colpos);
					if (!IsA(aliasnode, Var))
						ereport(ERROR,
								(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								 errmsg("foreign key joins require direct column references, found expression"),
								 parser_errposition(pstate, location)));

					aliasvar = castNode(Var, aliasnode);

					aliasrte = rt_fetch(aliasvar->varno, pstate->p_rtable);

					if (childrte == NULL)
						childrte = aliasrte;
					else if (childrte != aliasrte)
						ereport(ERROR,
								(errcode(ERRCODE_UNDEFINED_TABLE),
								 errmsg("key columns must all come from the same table"),
								 parser_errposition(pstate, location)));

					child_colnames = lappend(child_colnames,
											 makeString(get_rte_attribute_name(childrte,
																			   aliasvar->varattno)));
				}

				base_relid = drill_down_to_base_rel(pstate,
													childrte,
													colnames_out,
													child_colnames,
													is_referenced,
													location);

				return base_relid;
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
 *    Ensures that derived tables uphold virtual foreign key integrity
 */
static Oid
validate_and_resolve_derived_rel(ParseState *pstate, Query *query, RangeTblEntry *rte,
								 List *colnames, List **colnames_out,
								 bool is_referenced, int location)
{
	RangeTblEntry *anchor_rte = NULL;
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
			anchor_rte = rt_fetch(first_varno, query->rtable);
		}
		else if (first_varno != var->varno)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("key columns must all come from the same table"),
					 parser_errposition(pstate,
										exprLocation((Node *) matching_tle->expr))));

		base_colname = get_rte_attribute_name(anchor_rte, var->varattno);
		base_colnames = lappend(base_colnames, makeString(base_colname));
	}

	Assert(anchor_rte != NULL);

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
			validate_derived_rel_joins(pstate, query, anchor_rte, location);
		}
	}

	return drill_down_to_base_rel(pstate, anchor_rte, colnames_out,
								  base_colnames, is_referenced, location);
}


/*
 * validate_derived_rel_joins
 *     Ensures that all joins uphold virtual foreign key integrity
 */
static void
validate_derived_rel_joins(ParseState *pstate, Query *query,
						   RangeTblEntry *anchor_rte, int location)
{
	List	   *A = NIL;		/* Set of relations that preserve all rows */
	List	   *U = NIL;		/* Set of relations that preserve uniqueness */
	List	   *fromlist;
	Node	   *jtnode;
	RangeTblEntry *first_rte;
	bool		anchor_self_preserving;
	ListCell   *lc;

	/* Initialize A and U with the first RTE */
	Assert(query->rtable != NIL);

	fromlist = castNode(List, query->jointree->fromlist);
	if (list_length(fromlist) == 0)
		return;

	jtnode = (Node *) linitial(fromlist);

	if (IsA(jtnode, RangeTblRef))
	{
		first_rte = rt_fetch(((RangeTblRef *) jtnode)->rtindex,
							 query->rtable);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		Node	   *larg = ((JoinExpr *) jtnode)->larg;

		while (IsA(larg, JoinExpr))
		{
			larg = ((JoinExpr *) larg)->larg;
		}

		if (!IsA(larg, RangeTblRef))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("unsupported join structure in referenced table"),
					 parser_errposition(pstate, location)));

		first_rte = rt_fetch(((RangeTblRef *) larg)->rtindex,
							 query->rtable);
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported join structure in referenced table"),
				 parser_errposition(pstate, location)));
	}

	/*
	 * Initialize A as a map where first_rte maps to {first_rte}. i.e. A = [
	 * [first_rte, [first_rte]] ] in list form
	 */
	A = list_make1(list_make2(first_rte, list_make1(first_rte)));
	U = list_make1(first_rte);

	validate_join_node(pstate, query, jtnode, &A, &U, location);

	anchor_self_preserving = false;
	foreach(lc, A)
	{
		List	   *entry = (List *) lfirst(lc);
		RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
		List	   *preserved_set = (List *) lsecond(entry);

		if (key_rte == anchor_rte && list_member_ptr(preserved_set, anchor_rte))
		{
			anchor_self_preserving = true;
			break;
		}
	}

	if (!anchor_self_preserving)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("virtual foreign key constraint violation"),
				 errdetail("The derived table does not preserve all rows from the referenced relation."),
				 parser_errposition(pstate, location)));

	if (!list_member_ptr(U, anchor_rte))
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("virtual foreign key constraint violation"),
				 errdetail("The derived table does not preserve uniqueness of the referenced relation's key."),
				 parser_errposition(pstate, location)));
}


/*
 * validate_join_node
 *     Recursively process join nodes and update the A and U sets
 */
static void
validate_join_node(ParseState *pstate, Query *query, Node *node,
				   List **A, List **U, int location)
{
	JoinExpr   *join;
	ForeignKeyJoinNode *fkjn;
	RangeTblEntry *referencing_rte;
	RangeTblEntry *referenced_rte;
	List	   *referencing_colaliases = NIL;
	List	   *referenced_colaliases = NIL;
	ListCell   *lc;
	RangeTblEntry *existing_rte;
	RangeTblEntry *new_rte;
	bool		fk_cols_not_null;
	bool		fk_cols_unique;
	List	   *A_inner = NIL;
	bool		preserves_rows = false;
	bool		self_preserving = false;

	if (node == NULL || IsA(node, RangeTblRef))
		return;

	if (!IsA(node, JoinExpr))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("unsupported join structure in referenced table"),
				 parser_errposition(pstate, location)));

	join = (JoinExpr *) node;

	validate_join_node(pstate, query, join->larg, A, U, location);
	validate_join_node(pstate, query, join->rarg, A, U, location);

	if (join->fkJoin == NULL)
	{
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("virtual foreign key constraint violation"),
				 errdetail("The derived table contains a join that is not a foreign key join"),
				 parser_errposition(pstate, location)));
	}

	fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);

	referencing_rte = rt_fetch(fkjn->referencingVarno, query->rtable);
	referenced_rte = rt_fetch(fkjn->referencedVarno, query->rtable);

	foreach(lc, fkjn->referencingAttnums)
	{
		int			attnum = lfirst_int(lc);
		char	   *colname = get_rte_attribute_name(referencing_rte, attnum);

		referencing_colaliases = lappend(referencing_colaliases, makeString(colname));
	}

	foreach(lc, fkjn->referencedAttnums)
	{
		int			attnum = lfirst_int(lc);
		char	   *colname = get_rte_attribute_name(referenced_rte, attnum);

		referenced_colaliases = lappend(referenced_colaliases, makeString(colname));
	}

	fk_cols_not_null = check_columns_not_nullable(pstate, referencing_rte,
												  referencing_colaliases, location);
	fk_cols_unique = check_columns_unique(pstate, referencing_rte,
										  referencing_colaliases, location);

	if (fkjn->fkdir == FKDIR_FROM)
	{
		existing_rte = referencing_rte;
		new_rte = referenced_rte;
	}
	else
	{
		existing_rte = referenced_rte;
		new_rte = referencing_rte;
	}

	/*
	 * See if existing_rte is present in A at all, and if it is
	 * self-preserving.
	 */
	foreach(lc, *A)
	{
		List	   *entry = (List *) lfirst(lc);
		RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
		List	   *preserved_set = (List *) lsecond(entry);

		if (key_rte == existing_rte)
		{
			preserves_rows = true;
			if (list_member_ptr(preserved_set, existing_rte))
				self_preserving = true;
			break;
		}
	}

	/* Build A_inner only if fk_cols_not_null is true */
	if (fk_cols_not_null)
	{
		if (fkjn->fkdir == FKDIR_TO && self_preserving)
		{
			/*
			 * For each key in A, if existing_rte is in that key's
			 * preserved_set, we add new_rte to that new set.
			 */
			foreach(lc, *A)
			{
				List	   *entry = (List *) lfirst(lc);
				RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
				List	   *preserved_set = (List *) lsecond(entry);

				if (list_member_ptr(preserved_set, existing_rte))
				{
					A_inner = lappend(A_inner,
									  list_make2(key_rte, list_make1(new_rte)));
				}
			}
			/* Also add new_rte => [new_rte] */
			A_inner = lappend(A_inner,
							  list_make2(new_rte, list_make1(new_rte)));
		}
		else if (fkjn->fkdir == FKDIR_FROM && preserves_rows)
		{
			/*
			 * For each key in A, new preserved = old preserved ∩
			 * existing_rte's preserved_set
			 */
			List	   *existing_preserved = NIL;

			/* Gather existing_rte's preserved set. */
			foreach(lc, *A)
			{
				List	   *entry = (List *) lfirst(lc);
				RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);

				if (key_rte == existing_rte)
				{
					existing_preserved = (List *) lsecond(entry);
					break;
				}
			}

			foreach(lc, *A)
			{
				List	   *entry = (List *) lfirst(lc);
				RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
				List	   *preserved_set = (List *) lsecond(entry);
				List	   *new_preserved = NIL;
				ListCell   *lc2;

				foreach(lc2, preserved_set)
				{
					RangeTblEntry *preserved_rte = lfirst(lc2);

					if (list_member_ptr(existing_preserved, preserved_rte))
						new_preserved = lappend(new_preserved, preserved_rte);
				}

				A_inner = lappend(A_inner,
								  list_make2(key_rte, new_preserved));
			}

			/* new_rte => copy of existing_preserved */
			A_inner = lappend(A_inner,
							  list_make2(new_rte, list_copy(existing_preserved)));
		}
	}

	/*
	 * Now we do the final merges into *A* based on join type.
	 */
	if (join->jointype == JOIN_INNER)
	{
		*A = A_inner;
	}
	else if (join->jointype == JOIN_LEFT)
	{
		*A = map_union(*A, A_inner);
	}
	else if (join->jointype == JOIN_RIGHT)
	{
		List	   *extra = list_make1(list_make2(new_rte, list_make1(new_rte)));
		List	   *temp = map_union(A_inner, extra);

		*A = temp;
	}
	else if (join->jointype == JOIN_FULL)
	{
		/*
		 * We'll do *A = map_union(*A, A_inner), then *A = map_union(*A, {
		 * new_rte: [new_rte] })
		 */
		List	   *temp = map_union(*A, A_inner);
		List	   *extra = list_make1(list_make2(new_rte, list_make1(new_rte)));

		*A = map_union(temp, extra);

	}

	/*
	 * Now update *U* based on direction & uniqueness, matching the Python
	 * logic.
	 */
	if (fkjn->fkdir == FKDIR_FROM)
	{
		if (list_member_ptr(*U, existing_rte) && fk_cols_unique)
		{
			*U = lappend(*U, new_rte);
		}
	}
	else
	{
		if (list_member_ptr(*U, existing_rte))
		{
			if (fk_cols_unique)
				*U = lappend(*U, new_rte);
			else
				*U = list_make1(new_rte);
		}
		else if (!fk_cols_unique)
		{
			*U = NIL;
		}
	}

	dump_A_list(*A, new_rte);
	dump_U_list(*U, new_rte);
}


/*
 * check_columns_not_nullable
 *    Check that all specified columns in the relation are NOT NULL
 */
static bool
check_columns_not_nullable(ParseState *pstate, RangeTblEntry *rte,
						   List *colaliases, int location)
{
	List	   *base_colnames = NIL;
	Oid			base_relid;
	ListCell   *lc;

	base_relid = drill_down_to_base_rel(pstate, rte,
										&base_colnames, colaliases, false,
										location);

	foreach(lc, base_colnames)
	{
		char	   *colname = strVal(lfirst(lc));
		AttrNumber	attnum;
		HeapTuple	tuple;
		Form_pg_attribute attr;

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
			return false;
		}

		ReleaseSysCache(tuple);
	}

	return true;
}


/*
 * check_columns_unique
 *    Check if there is a UNIQUE constraint on the specified columns
 */
static bool
check_columns_unique(ParseState *pstate, RangeTblEntry *rte,
					 List *colaliases, int location)
{
	List	   *base_colnames = NIL;
	Oid			base_relid;
	Relation	rel;
	SysScanDesc scan;
	ScanKeyData skey[1];
	HeapTuple	htup;
	bool		found = false;

	base_relid = drill_down_to_base_rel(pstate, rte,
										&base_colnames, colaliases, false,
										location);

	rel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(base_relid));

	scan = systable_beginscan(rel, ConstraintRelidTypidNameIndexId, true,
							  NULL, 1, skey);

	while (HeapTupleIsValid(htup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(htup);
		Datum		conkey_datum;
		bool		conkey_isnull;
		ArrayType  *conkey_arr;
		int16	   *conkey;
		int			nkeys;
		List	   *unique_colnames = NIL;
		ListCell   *lc;
		bool		all_cols_match = true;
		int			i;

		if (con->contype != CONSTRAINT_PRIMARY &&
			con->contype != CONSTRAINT_UNIQUE)
			continue;

		conkey_datum = SysCacheGetAttr(CONSTROID, htup,
									   Anum_pg_constraint_conkey,
									   &conkey_isnull);
		if (conkey_isnull)
			continue;

		conkey_arr = DatumGetArrayTypeP(conkey_datum);
		conkey = (int16 *) ARR_DATA_PTR(conkey_arr);
		nkeys = ArrayGetNItems(ARR_NDIM(conkey_arr), ARR_DIMS(conkey_arr));

		if (nkeys != list_length(base_colnames))
			continue;

		for (i = 0; i < nkeys; i++)
		{
			char	   *colname = get_attname(base_relid, conkey[i], false);

			unique_colnames = lappend(unique_colnames, makeString(colname));
		}

		foreach(lc, base_colnames)
		{
			char	   *colname = strVal(lfirst(lc));
			ListCell   *lc2;
			bool		col_found = false;

			foreach(lc2, unique_colnames)
			{
				if (strcmp(colname, strVal(lfirst(lc2))) == 0)
				{
					col_found = true;
					break;
				}
			}

			if (!col_found)
			{
				all_cols_match = false;
				break;
			}
		}

		if (all_cols_match)
		{
			found = true;
			list_free_deep(unique_colnames);
			break;
		}

		list_free_deep(unique_colnames);
	}

	systable_endscan(scan);
	table_close(rel, AccessShareLock);

	return found;
}


/* ------------------------------------------------------------------------
 *  Dictionary-union helper routines for merges of A
 * ------------------------------------------------------------------------
 */

/*
 * map_union
 *
 * Perform a "dictionary union" of oldA and A_inner, but for each key that
 * appears in both, we do a *set union* of the second element (the preserved
 * list), instead of overwriting. This ensures that if oldA had t3 : { t1, t3 }
 * and A_inner has t3 : { t1 }, final t3 remains { t1, t3 } ∪ { t1 } = { t1, t3 }.
 */
static List *
map_union(List *oldA, List *A_inner)
{
	List	   *result = NIL;
	ListCell   *lc;

	/* 1) Copy all entries from oldA into result (shallow copy of lists). */
	foreach(lc, oldA)
	{
		List	   *entry = (List *) lfirst(lc);
		RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
		List	   *old_vals = (List *) lsecond(entry);

		/* Copy old_vals so we can modify it if needed. */
		result = lappend(result,
						 list_make2(key_rte, list_copy(old_vals)));
	}

	/*
	 * 2) For each (key, new_vals) in A_inner, union it into result if key
	 * exists.
	 */
	foreach(lc, A_inner)
	{
		List	   *entry = (List *) lfirst(lc);
		RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
		List	   *new_vals = (List *) lsecond(entry);
		bool		found_key = false;

		for (ListCell *lc2 = list_head(result); lc2; lc2 = lnext(result, lc2))
		{
			List	   *res_entry = (List *) lfirst(lc2);
			RangeTblEntry *res_key = (RangeTblEntry *) linitial(res_entry);
			List	   *res_vals = (List *) lsecond(res_entry);

			if (res_key == key_rte)
			{
				/*
				 * Merge sets: res_vals = res_vals ∪ new_vals by appending
				 * only those new_vals not in res_vals.
				 */
				ListCell   *lcv;

				foreach(lcv, new_vals)
				{
					RangeTblEntry *nv = (RangeTblEntry *) lfirst(lcv);

					if (!list_member_ptr(res_vals, nv))
						res_vals = lappend(res_vals, nv);
				}
				lsecond(res_entry) = res_vals;
				found_key = true;
				break;
			}
		}

		if (!found_key)
		{
			/*
			 * key_rte not in result, so just append [key_rte, copy(new_vals)]
			 */
			result = lappend(result,
							 list_make2(key_rte, list_copy(new_vals)));
		}
	}

	return result;
}

/* ------------------------------------------------------------------------
 *  Helper routines to show A and U using relation aliases
 * ------------------------------------------------------------------------
 */

/*
 * rte_aliasname
 *    Returns the best textual name for an RTE: alias if present, else
 *    the base rel name, else "<unknown>".
 */
static const char *
rte_aliasname(RangeTblEntry *rte)
{
	if (rte->eref && rte->eref->aliasname)
		return rte->eref->aliasname;
	else if (rte->alias && rte->alias->aliasname)
		return rte->alias->aliasname;
	else if (rte->relid != InvalidOid)
	{
		const char *relname = get_rel_name(rte->relid);

		if (relname)
			return relname;
	}
	return "<unknown>";
}

#ifdef FKJOINS_DEBUG
/*
 * dump_A_list
 *    Print A in dictionary style, e.g.:
 *    { t1: { t2 }, t2: { t2 }, t3: { t3, t2 } }
 *
 *    Each entry in A is [ key_rte, preserved_list ].
 */
static void
dump_A_list(List *A, RangeTblEntry *new_rte)
{
	StringInfoData buf;
	char	   *result;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '{');

	for (int i = 0; i < list_length(A); i++)
	{
		List	   *entry = (List *) list_nth(A, i);
		RangeTblEntry *key_rte = (RangeTblEntry *) linitial(entry);
		List	   *preservedSet = (List *) lsecond(entry);

		if (i > 0)
			appendStringInfoString(&buf, ", ");

		appendStringInfo(&buf, "%s: {", rte_aliasname(key_rte));

		for (int j = 0; j < list_length(preservedSet); j++)
		{
			RangeTblEntry *elt = list_nth(preservedSet, j);

			if (j > 0)
				appendStringInfoString(&buf, ", ");
			appendStringInfoString(&buf, rte_aliasname(elt));
		}

		appendStringInfoChar(&buf, '}');
	}

	appendStringInfoChar(&buf, '}');
	result = pstrdup(buf.data);
	pfree(buf.data);

	elog(NOTICE, "%s A => %s", rte_aliasname(new_rte), result);
	pfree(result);
}

/*
 * dump_U_list
 *    Print U as a set, e.g.:
 *    { t1, t2, t3 }
 */
static void
dump_U_list(List *U, RangeTblEntry *new_rte)
{
	StringInfoData buf;
	char	   *result;

	initStringInfo(&buf);

	appendStringInfoChar(&buf, '{');

	for (int i = 0; i < list_length(U); i++)
	{
		RangeTblEntry *elt = (RangeTblEntry *) list_nth(U, i);

		if (i > 0)
			appendStringInfoString(&buf, ", ");
		appendStringInfoString(&buf, rte_aliasname(elt));
	}

	appendStringInfoChar(&buf, '}');
	result = pstrdup(buf.data);
	pfree(buf.data);

	elog(NOTICE, "%s U => %s", rte_aliasname(new_rte), result);
	pfree(result);
}
#endif
