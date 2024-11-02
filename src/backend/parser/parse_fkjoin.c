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
#include "nodes/primnodes.h"
#include "parser/parse_coerce.h"
#include "parser/parse_expr.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "parser/parse_fkjoin.h"

static Node *transformForeignKeyJoin(ParseState *pstate, List *referencingVars, List *referencedVars);
static Oid	find_foreign_key(Oid referencing_relid, Oid referenced_relid, List *referencing_cols, List *referenced_cols);
static char *ColumnListToString(const List *columns);

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
extern void
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
	Oid			fkoid;
	ForeignKeyJoinNode *fkjn_node;
	List	   *referencing_attnums = NIL;
	List	   *referenced_attnums = NIL;

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

	fkoid = find_foreign_key(referencing_rte->relid, referenced_rte->relid,
							 referencing_cols, referenced_cols);

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
		char	   *referencing_col,
				   *referenced_col;
		AttrNumber	referencing_attnum,
					referenced_attnum;
		Oid			referencing_vartype,
					referenced_vartype;
		int32		referencing_vartypmod,
					referenced_vartypmod;
		Oid			referencing_varcollid,
					referenced_varcollid;
		Var		   *referencing_var,
				   *referenced_var;

		referencing_col = strVal(lfirst(lc));
		referenced_col = strVal(lfirst(rc));

		/* Get attribute numbers for the columns */
		referencing_attnum = get_attnum(referencing_rte->relid, referencing_col);
		referenced_attnum = get_attnum(referenced_rte->relid, referenced_col);

		if (referencing_attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" not found in table \"%s\"",
							referencing_col, referencing_rte->eref->aliasname),
					 parser_errposition(pstate, fkjn->location)));

		if (referenced_attnum == InvalidAttrNumber)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" not found in table \"%s\"",
							referenced_col, referenced_rte->eref->aliasname),
					 parser_errposition(pstate, fkjn->location)));

		/* Get type information for referencing column */
		get_atttypetypmodcoll(referencing_rte->relid, referencing_attnum,
							  &referencing_vartype, &referencing_vartypmod, &referencing_varcollid);

		/* Get type information for referenced column */
		get_atttypetypmodcoll(referenced_rte->relid, referenced_attnum,
							  &referenced_vartype, &referenced_vartypmod, &referenced_varcollid);

		/* Build Vars */
		referencing_var = makeVar(referencing_rel->p_rtindex,
								  referencing_attnum,
								  referencing_vartype,
								  referencing_vartypmod,
								  referencing_varcollid,
								  0);
		referenced_var = makeVar(referenced_rel->p_rtindex,
								 referenced_attnum,
								 referenced_vartype,
								 referenced_vartypmod,
								 referenced_varcollid,
								 0);

		/* Mark vars for SELECT privilege */
		markVarForSelectPriv(pstate, referencing_var);
		markVarForSelectPriv(pstate, referenced_var);

		/* Add to lists */
		referencingVars = lappend(referencingVars, referencing_var);
		referencedVars = lappend(referencedVars, referenced_var);

		/* Collect attribute numbers */
		referencing_attnums = lappend_int(referencing_attnums, referencing_attnum);
		referenced_attnums = lappend_int(referenced_attnums, referenced_attnum);
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
