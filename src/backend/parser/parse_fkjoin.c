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
#include "catalog/pg_constraint.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_fkjoin.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
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
static RTEId *resolve_fk_base_table(ParseState *pstate,
									ParseNamespaceItem *nsitem,
									List *attnums,
									List **base_attnums,
									int location);
static bool check_fk_cols_not_null(Oid relid, List *attnums,
								   List **notNullOids);
static bool check_fk_cols_unique(Oid relid, List *attnums);
void
transformAndValidateForeignKeyJoin(ParseState *pstate, JoinExpr *join,
								   ParseNamespaceItem *r_nsitem,
								   List *l_namespace)
{
	ForeignKeyClause *fkjn = castNode(ForeignKeyClause, join->fkJoin);
	ListCell   *lc;
	RangeTblEntry *referencing_rte,
			   *referenced_rte;
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
	RTEId	   *referencing_rteid;
	RTEId	   *referenced_rteid;

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

	referencing_rteid = resolve_fk_base_table(pstate, referencing_rel,
											  referencing_attnums,
											  &referencing_base_attnums,
											  fkjn->location);
	referenced_rteid = resolve_fk_base_table(pstate, referenced_rel,
											 referenced_attnums,
											 &referenced_base_attnums,
											 fkjn->location);

	Assert(referencing_rteid != NULL && referenced_rteid != NULL);

	/*
	 * Check that the referenced side preserves the referenced base table
	 * instance.  Compare by baserelindex to distinguish different instances
	 * of the same base table (e.g., self-joins).
	 */
	if (referenced_rte->fkPreservedRteid == NULL ||
		referenced_rte->fkPreservedRteid->baserelindex != referenced_rteid->baserelindex)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("referenced relation does not preserve the referenced base table"),
				 errdetail("The referenced side of a foreign key join must preserve the referenced base table's rows without loss or duplication."),
				 parser_errposition(pstate, fkjn->location)));

	fkoid = find_foreign_key(referencing_rteid->relid, referenced_rteid->relid,
							 referencing_base_attnums, referenced_base_attnums);

	if (fkoid == InvalidOid)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
						referencing_rte->alias ? referencing_rte->alias->aliasname :
						get_rel_name(referencing_rteid->relid),
						column_list_to_string(referencing_cols),
						referenced_rte->alias ? referenced_rte->alias->aliasname :
						get_rel_name(referenced_rteid->relid),
						column_list_to_string(referenced_cols)),
				 parser_errposition(pstate, fkjn->location)));

	join->quals = build_fk_join_on_clause(pstate, referencing_rel->p_nscolumns, referencing_attnums, referenced_rel->p_nscolumns, referenced_attnums);

	fkjn_node = makeNode(ForeignKeyJoinNode);
	fkjn_node->fkdir = fkjn->fkdir;
	fkjn_node->referencingVarno = referencing_rel->p_rtindex;
	fkjn_node->referencingAttnums = referencing_attnums;
	fkjn_node->referencedVarno = referenced_rel->p_rtindex;
	fkjn_node->referencedAttnums = referenced_attnums;
	fkjn_node->constraint = fkoid;
	fkjn_node->referencingRteid = referencing_rteid;
	fkjn_node->referencedRteid = referenced_rteid;

	/*
	 * Check NOT NULL and uniqueness of FK columns on the referencing side.
	 *
	 * Collect NOT NULL constraint OIDs when the referencing table is on the
	 * "inner" side of the join (the side whose rows can be filtered out).
	 * This creates a dependency so that DROP NOT NULL is blocked when needed:
	 *   - INNER JOIN: always track (unmatched rows are lost on both sides)
	 *   - LEFT JOIN + FKDIR_TO (referencing on right): track (right is inner)
	 *   - RIGHT JOIN + FKDIR_FROM (referencing on left): track (left is inner)
	 *   - Otherwise: don't track (referencing side is preserved by outer join)
	 */
	{
		bool		track_notnull;
		List	   *notnull_oids = NIL;

		track_notnull = (join->jointype == JOIN_INNER ||
						 (join->jointype == JOIN_LEFT && fkjn->fkdir == FKDIR_TO) ||
						 (join->jointype == JOIN_RIGHT && fkjn->fkdir == FKDIR_FROM));

		fkjn_node->fkColsNotNull = check_fk_cols_not_null(referencing_rteid->relid,
														   referencing_base_attnums,
														   track_notnull ? &notnull_oids : NULL);
		fkjn_node->notNullConstraints = notnull_oids;
	}

	fkjn_node->fkColsUnique = check_fk_cols_unique(referencing_rteid->relid,
													referencing_base_attnums);

	join->fkJoin = (Node *) fkjn_node;
}

/*
 * resolve_fk_base_table
 *		Use the per-column base table mapping (fkColBaseRteids/fkColBaseAttnums)
 *		to resolve the underlying base table instance for the given columns.
 *
 * All columns must resolve to the same base table instance (identified by
 * RTEId's baserelindex).  Works for all RTE types: base tables have identity
 * colmaps, joins compute colmaps from joinaliasvars, and derived relations
 * (subqueries, views, CTEs) propagate colmaps from their inner queries.
 *
 * Returns the RTEId of the base table instance and fills *base_attnums with
 * the resolved attribute numbers.
 */
static RTEId *
resolve_fk_base_table(ParseState *pstate,
					  ParseNamespaceItem *nsitem,
					  List *attnums,
					  List **base_attnums,
					  int location)
{
	RangeTblEntry *rte = rt_fetch(nsitem->p_rtindex, pstate->p_rtable);
	RTEId	   *base_rteid = NULL;
	List	   *result_attnums = NIL;
	ListCell   *lc;

	if (rte->fkColBaseRteids == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins require direct column references"),
				 parser_errposition(pstate, location)));

	foreach(lc, attnums)
	{
		int			attnum = lfirst_int(lc);
		RTEId	   *col_rteid;
		int			col_attnum;

		if (attnum < 1 || attnum > list_length(rte->fkColBaseRteids))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins require direct column references"),
					 parser_errposition(pstate, location)));

		col_rteid = (RTEId *) list_nth(rte->fkColBaseRteids, attnum - 1);
		col_attnum = list_nth_int(rte->fkColBaseAttnums, attnum - 1);

		if (col_rteid == NULL || !OidIsValid(col_rteid->relid))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins require direct column references"),
					 errdetail("Column \"%s\" is not a direct reference to a base table column.",
							   strVal(list_nth(rte->eref->colnames, attnum - 1))),
					 parser_errposition(pstate, location)));

		if (base_rteid == NULL)
			base_rteid = col_rteid;
		else if (base_rteid->baserelindex != col_rteid->baserelindex)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("all key columns must belong to the same table"),
					 parser_errposition(pstate, location)));

		result_attnums = lappend_int(result_attnums, col_attnum);
	}

	if (base_rteid == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("no valid columns for foreign key join"),
				 parser_errposition(pstate, location)));

	*base_attnums = result_attnums;
	return base_rteid;
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
 * check_fk_cols_not_null
 *		Check whether all the given columns of a base table have NOT NULL
 *		constraints.
 *
 * attnums is a list of int16 attribute numbers (base table attnums).
 *
 * If notNullOids is not NULL and all columns are NOT NULL, the function
 * collects the NOT NULL constraint OIDs into *notNullOids.  This is used
 * for dependency tracking so that DROP NOT NULL is blocked when needed.
 */
static bool
check_fk_cols_not_null(Oid relid, List *attnums, List **notNullOids)
{
	Relation	rel;
	TupleDesc	tupdesc;
	ListCell   *lc;
	bool		all_not_null = true;
	List	   *constraints = NIL;

	rel = table_open(relid, AccessShareLock);
	tupdesc = RelationGetDescr(rel);

	foreach(lc, attnums)
	{
		int			attnum = lfirst_int(lc);
		Form_pg_attribute attr;

		if (attnum < 1 || attnum > tupdesc->natts)
		{
			all_not_null = false;
			break;
		}

		attr = TupleDescAttr(tupdesc, attnum - 1);
		if (!attr->attnotnull)
		{
			all_not_null = false;
			break;
		}

		/* Collect NOT NULL constraint OIDs if requested */
		if (notNullOids != NULL)
		{
			HeapTuple	conTup;

			conTup = findNotNullConstraintAttnum(relid, attnum);
			if (HeapTupleIsValid(conTup))
			{
				Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(conTup);

				constraints = lappend_oid(constraints, con->oid);
				heap_freetuple(conTup);
			}
		}
	}

	table_close(rel, AccessShareLock);

	if (notNullOids != NULL)
	{
		if (all_not_null)
			*notNullOids = constraints;
		else
		{
			list_free(constraints);
			*notNullOids = NIL;
		}
	}

	return all_not_null;
}

/*
 * check_fk_cols_unique
 *		Check whether the given columns of a base table form a unique key
 *		(UNIQUE or PRIMARY KEY constraint).
 *
 * attnums is a list of int16 attribute numbers (base table attnums).
 * Returns true if there exists a UNIQUE or PRIMARY KEY constraint whose
 * columns are exactly the given set.
 */
static bool
check_fk_cols_unique(Oid relid, List *attnums)
{
	Relation	conrel;
	SysScanDesc scan;
	ScanKeyData skey[1];
	HeapTuple	tup;
	bool		found = false;
	int			nkeys = list_length(attnums);

	conrel = table_open(ConstraintRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_constraint_conrelid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(relid));
	scan = systable_beginscan(conrel, ConstraintRelidTypidNameIndexId,
							  true, NULL, 1, skey);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_constraint con = (Form_pg_constraint) GETSTRUCT(tup);
		Datum		conkey_datum;
		bool		conkey_isnull;
		ArrayType  *conkey_arr;
		int16	   *conkey;
		int			con_nkeys;
		bool		match;

		if (con->contype != CONSTRAINT_UNIQUE &&
			con->contype != CONSTRAINT_PRIMARY)
			continue;

		conkey_datum = SysCacheGetAttr(CONSTROID, tup,
									   Anum_pg_constraint_conkey,
									   &conkey_isnull);
		if (conkey_isnull)
			continue;

		conkey_arr = DatumGetArrayTypeP(conkey_datum);
		con_nkeys = ArrayGetNItems(ARR_NDIM(conkey_arr), ARR_DIMS(conkey_arr));

		if (con_nkeys != nkeys)
			continue;

		conkey = (int16 *) ARR_DATA_PTR(conkey_arr);

		/*
		 * Check that every column in the constraint appears in our attnum
		 * list, and vice versa (since lengths are equal, checking one
		 * direction suffices).
		 */
		match = true;
		for (int i = 0; i < con_nkeys && match; i++)
		{
			bool		col_found = false;
			ListCell   *lc;

			foreach(lc, attnums)
			{
				if (lfirst_int(lc) == conkey[i])
				{
					col_found = true;
					break;
				}
			}
			if (!col_found)
				match = false;
		}

		if (match)
		{
			found = true;
			break;
		}
	}

	systable_endscan(scan);
	table_close(conrel, AccessShareLock);

	return found;
}

