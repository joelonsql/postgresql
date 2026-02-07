/*-------------------------------------------------------------------------
 *
 * parse_fk_join.c
 *	  handle foreign key joins in parser
 *
 * This file implements the FOR KEY join syntax, which allows joins to
 * be expressed using declared foreign key relationships.  The system
 * validates at query analysis time that the specified column pairs
 * correspond to an actual FK constraint and that the referenced side
 * satisfies the invariants required for the FK guarantee to hold.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_fk_join.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_class.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_rewrite.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/primnodes.h"
#include "optimizer/optimizer.h"
#include "parser/parse_clause.h"
#include "parser/parse_expr.h"
#include "parser/parse_fk_join.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

/*
 * Information about a resolved FK join column, tracing it back to
 * a base table and attribute number.
 */
typedef struct FkColumnInfo
{
	Oid			relid;			/* base table OID */
	AttrNumber	attnum;			/* attribute number in base table */
	char	   *colname;		/* column name as written */
	int			leaf_varno;		/* varno of the RTE_RELATION in the leaf query */
	int			col_location;	/* source location of the column expression */
} FkColumnInfo;

/*
 * Module-level list of all visible CTE definitions, collected from the
 * ParseState hierarchy at the start of transformFkJoinClause.  This avoids
 * threading the list through every internal function.
 */
static List *fkjoin_visible_ctes = NIL;

/* Forward declarations */
static bool traceFkColumn(Query *subquery, const char *colname,
						  Oid *relid, AttrNumber *attnum,
						  int *leaf_varno, int location);
static bool traceFkColumnByIndex(Query *subquery, int col_index,
								 Oid *relid, AttrNumber *attnum,
								 int *leaf_varno);
static bool traceTargetEntryToBase(Query *query, TargetEntry *tle,
								   Oid *relid, AttrNumber *attnum,
								   int *leaf_varno);
static CommonTableExpr *findCTEByName(Query *query, const char *ctename);
static Oid lookupFkConstraint(Oid fk_relid, Oid pk_relid,
							  int nkeys,
							  AttrNumber *fk_attnums,
							  AttrNumber *pk_attnums);
static void validateReferencedSide(ParseState *pstate,
								   RangeTblEntry *rte,
								   FkColumnInfo *pk_colinfos,
								   int nkeys,
								   int location);
static bool checkRowPreservation(Query *query, Oid base_relid);
static bool checkRowPreservationInNode(Query *query, Node *jtnode,
									   Oid base_relid);
static bool nodeContainsBaseRelid(Query *query, Node *jtnode,
								  Oid base_relid);
static bool checkUniquenessPreservation(Query *query, Oid base_relid,
										AttrNumber *pk_attnums, int nkeys);
static bool checkUniquenessInNode(Query *query, Node *jtnode,
								  Oid base_relid);
static bool checkFkColumnsNotNull(Oid conoid);
static bool checkGroupByRestoresUniqueness(Query *query, Oid base_relid,
										   AttrNumber *pk_attnums, int nkeys,
										   int location);
static Node *buildFkJoinQuals(ParseState *pstate,
							  ParseNamespaceColumn *fk_nscolumns,
							  int *fk_indexes,
							  ParseNamespaceColumn *pk_nscolumns,
							  int *pk_indexes,
							  int nkeys);
static RangeTblEntry *findRteByName(ParseState *pstate,
									const char *refname,
									List *my_namespace,
									ParseNamespaceItem **nsitem_out,
									int location);
static bool revalidateFkJoinInQuery(Query *query);
static bool revalidateFkJoinInNode(Query *query, Node *jtnode);

/*
 * transformFkJoinClause
 *
 * Process a FOR KEY join clause.  Validates the FK constraint exists,
 * validates the referenced side preserves rows and uniqueness, and
 * generates the equi-join ON condition.
 *
 * Returns the generated quals (a BoolExpr or OpExpr).
 * Populates the JoinExpr's FK-specific fields as a side effect.
 */
Node *
transformFkJoinClause(ParseState *pstate,
					  JoinExpr *j,
					  ParseNamespaceItem *l_nsitem,
					  ParseNamespaceItem *r_nsitem,
					  List *l_colnames,
					  List *r_colnames,
					  ParseNamespaceColumn *l_nscolumns,
					  ParseNamespaceColumn *r_nscolumns,
					  List *my_namespace)
{
	RangeVar   *ref_table = j->fk_ref_table;
	FkJoinArrowDir arrow_dir = j->fk_arrow_dir;
	List	   *fk_col_names = j->fk_join_cols;
	List	   *pk_col_names = j->pk_join_cols;
	int			nkeys;
	int			location;

	/* Determine which sides of the join are FK and PK */
	List	   *fk_side_colnames;
	List	   *pk_side_colnames;
	ParseNamespaceColumn *fk_side_nscolumns;
	ParseNamespaceColumn *pk_side_nscolumns;
	RangeTblEntry *fk_rte;
	RangeTblEntry *pk_rte;

	/* Additional variables for column resolution */
	FkColumnInfo *fk_colinfos;
	FkColumnInfo *pk_colinfos;
	int		   *fk_indexes;
	int		   *pk_indexes;
	AttrNumber	fk_attnums[INDEX_MAX_KEYS];
	AttrNumber	pk_attnums[INDEX_MAX_KEYS];
	Oid			fk_base_relid = InvalidOid;
	Oid			pk_base_relid = InvalidOid;
	Oid			conoid;
	ListCell   *lc;
	int			i;
	Node	   *result;

	/* Get the location from the JoinExpr for error messages */
	location = j->fk_location;

	/*
	 * Collect all visible CTE definitions from the parse state hierarchy.
	 * This allows internal functions to resolve CTE references without
	 * needing the ParseState.
	 */
	{
		ParseState *ps;
		List	   *saved_ctes = fkjoin_visible_ctes;

		fkjoin_visible_ctes = NIL;
		for (ps = pstate; ps != NULL; ps = ps->parentParseState)
		{
			ListCell   *clc;

			foreach(clc, ps->p_ctenamespace)
				fkjoin_visible_ctes = lappend(fkjoin_visible_ctes,
											  lfirst(clc));
		}

		/* Ensure cleanup on error via PG_TRY */
		PG_TRY();
		{
			result = NULL;		/* will be set below */
		}
		PG_CATCH();
		{
			fkjoin_visible_ctes = saved_ctes;
			PG_RE_THROW();
		}
		PG_END_TRY();
		/* saved_ctes restored at the end of the function */
	}

	/* Validate column count match */
	if (list_length(fk_col_names) != list_length(pk_col_names))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("number of referencing and referenced columns for foriegn key disagree"),
				 parser_errposition(pstate, location)));

	nkeys = list_length(fk_col_names);

	/*
	 * Find the arrow-target table in the namespace.  The referenced table
	 * must be the left side of this join (part of what's already been
	 * processed).
	 */
	{
		ParseNamespaceItem *ref_nsitem = NULL;
		RangeTblEntry *ref_rte;
		const char *refname = ref_table->relname;

		/* Search in left side namespace for the referenced name */
		ref_rte = findRteByName(pstate, refname, my_namespace,
								&ref_nsitem, location);

		if (ref_rte == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_TABLE),
					 errmsg("table reference \"%s\" not found", refname),
					 parser_errposition(pstate, location)));

		/*
		 * Determine which is FK side and which is PK side based on the
		 * arrow direction and which side the ref_table resolves to.
		 */
		if (arrow_dir == FK_JOIN_FORWARD)
		{
			/*
			 * -> syntax: rarg (right) has FK cols, arrow target (left) has
			 * PK cols.  The first column list belongs to the joined table
			 * (right side), the second to the arrow target (ref_table).
			 */
			fk_side_colnames = r_colnames;
			fk_side_nscolumns = r_nscolumns;
			fk_rte = r_nsitem->p_rte;

			pk_side_colnames = ref_nsitem->p_names->colnames;
			pk_side_nscolumns = ref_nsitem->p_nscolumns;
			pk_rte = ref_rte;
		}
		else
		{
			/*
			 * <- syntax: rarg (right) has PK cols, arrow target (left) has
			 * FK cols.  The first column list belongs to the joined table
			 * (right side), the second to the arrow target (ref_table).
			 */
			pk_side_colnames = r_colnames;
			pk_side_nscolumns = r_nscolumns;
			pk_rte = r_nsitem->p_rte;

			fk_side_colnames = ref_nsitem->p_names->colnames;
			fk_side_nscolumns = ref_nsitem->p_nscolumns;
			fk_rte = ref_rte;
		}

		/* Store the ref_table's RT index for deparse */
		j->fk_ref_rtindex = ref_nsitem->p_rtindex;
	}

	/*
	 * Reject materialized views on either side early.
	 */
	if (fk_rte->rtekind == RTE_RELATION &&
		get_rel_relkind(fk_rte->relid) == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving this type of relation are not supported"),
				 errdetail("This operation is not supported for materialized views."),
				 parser_errposition(pstate, location)));

	if (pk_rte->rtekind == RTE_RELATION &&
		get_rel_relkind(pk_rte->relid) == RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving this type of relation are not supported"),
				 errdetail("This operation is not supported for materialized views."),
				 parser_errposition(pstate, location)));

	/*
	 * Reject set operations on either side - cannot trace FK through UNION,
	 * INTERSECT, or EXCEPT.
	 */
	if (fk_rte->rtekind == RTE_SUBQUERY &&
		fk_rte->subquery != NULL &&
		fk_rte->subquery->setOperations != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving set operations are not supported"),
				 parser_errposition(pstate, location)));

	/*
	 * Allocate arrays for column info.
	 */
	fk_colinfos = (FkColumnInfo *) palloc0(nkeys * sizeof(FkColumnInfo));
	pk_colinfos = (FkColumnInfo *) palloc0(nkeys * sizeof(FkColumnInfo));
	fk_indexes = (int *) palloc(nkeys * sizeof(int));
	pk_indexes = (int *) palloc(nkeys * sizeof(int));

	/*
	 * Resolve FK-side columns.
	 */
	i = 0;
	foreach(lc, fk_col_names)
	{
		char	   *colname = strVal(lfirst(lc));
		int			ndx;
		int			col_index = -1;
		ListCell   *col_lc;

		ndx = 0;
		foreach(col_lc, fk_side_colnames)
		{
			char	   *cn = strVal(lfirst(col_lc));

			if (strcmp(cn, colname) == 0)
			{
				if (col_index >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("common column name \"%s\" appears more than once in referencing table",
									colname),
							 parser_errposition(pstate, location)));
				col_index = ndx;
			}
			ndx++;
		}

		if (col_index < 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in referencing table",
							colname),
					 parser_errposition(pstate, location)));

		fk_indexes[i] = col_index;
		fk_colinfos[i].colname = colname;
		fk_colinfos[i].relid = InvalidOid;
		fk_colinfos[i].attnum = InvalidAttrNumber;

		if (fk_rte->rtekind == RTE_RELATION)
		{
			char		fk_relkind = get_rel_relkind(fk_rte->relid);

			if (fk_relkind == RELKIND_VIEW)
			{
				Relation	fk_view_rel;
				Query	   *fk_viewquery;

				fk_view_rel = table_open(fk_rte->relid, AccessShareLock);
				fk_viewquery = get_view_query(fk_view_rel);
				traceFkColumn(fk_viewquery, colname,
							  &fk_colinfos[i].relid,
							  &fk_colinfos[i].attnum,
							  &fk_colinfos[i].leaf_varno,
							  location);
				table_close(fk_view_rel, AccessShareLock);
			}
			else
			{
				fk_colinfos[i].relid = fk_rte->relid;
				fk_colinfos[i].attnum = col_index + 1;
				fk_colinfos[i].leaf_varno = 0;
			}
		}
		else if (fk_rte->rtekind == RTE_SUBQUERY)
		{
			traceFkColumnByIndex(fk_rte->subquery, col_index,
								 &fk_colinfos[i].relid,
								 &fk_colinfos[i].attnum,
								 &fk_colinfos[i].leaf_varno);
		}
		else if (fk_rte->rtekind == RTE_CTE)
		{
			CommonTableExpr *cte = GetCTEForRTE(pstate, fk_rte, 0);

			if (cte != NULL && IsA(cte->ctequery, Query))
			{
				traceFkColumnByIndex((Query *) cte->ctequery, col_index,
									 &fk_colinfos[i].relid,
									 &fk_colinfos[i].attnum,
									 &fk_colinfos[i].leaf_varno);
			}
		}
		else if (fk_rte->rtekind == RTE_JOIN)
		{
			ParseNamespaceColumn *nscol = &fk_side_nscolumns[col_index];

			if (nscol->p_varno > 0)
			{
				RangeTblEntry *col_rte = rt_fetch(nscol->p_varno,
												  pstate->p_rtable);

				if (col_rte->rtekind == RTE_RELATION)
				{
					fk_colinfos[i].relid = col_rte->relid;
					fk_colinfos[i].attnum = nscol->p_varattno;
					fk_colinfos[i].leaf_varno = nscol->p_varno;
				}
				else if (col_rte->rtekind == RTE_SUBQUERY &&
						 col_rte->subquery != NULL)
				{
					traceFkColumnByIndex(col_rte->subquery,
										 nscol->p_varattno - 1,
										 &fk_colinfos[i].relid,
										 &fk_colinfos[i].attnum,
										 &fk_colinfos[i].leaf_varno);
				}
				else if (col_rte->rtekind == RTE_CTE)
				{
					CommonTableExpr *cte = GetCTEForRTE(pstate, col_rte, 0);

					if (cte != NULL && IsA(cte->ctequery, Query))
						traceFkColumnByIndex((Query *) cte->ctequery,
											 nscol->p_varattno - 1,
											 &fk_colinfos[i].relid,
											 &fk_colinfos[i].attnum,
											 &fk_colinfos[i].leaf_varno);
				}
			}
		}

		/*
		 * Capture the column's source location from the subquery's
		 * target entry for better error positioning.
		 */
		fk_colinfos[i].col_location = -1;
		if (fk_rte->rtekind == RTE_SUBQUERY && fk_rte->subquery != NULL)
		{
			ListCell   *tlc;
			int			tidx = 0;

			foreach(tlc, fk_rte->subquery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlc);

				if (tle->resjunk)
					continue;
				if (tidx == col_index && tle->expr != NULL)
				{
					fk_colinfos[i].col_location = exprLocation((Node *) tle->expr);
					break;
				}
				tidx++;
			}
		}

		i++;
	}

	/*
	 * Resolve PK-side columns.
	 */
	i = 0;
	foreach(lc, pk_col_names)
	{
		char	   *colname = strVal(lfirst(lc));
		int			ndx;
		int			col_index = -1;
		ListCell   *col_lc;

		ndx = 0;
		foreach(col_lc, pk_side_colnames)
		{
			char	   *cn = strVal(lfirst(col_lc));

			if (strcmp(cn, colname) == 0)
			{
				if (col_index >= 0)
					ereport(ERROR,
							(errcode(ERRCODE_AMBIGUOUS_COLUMN),
							 errmsg("common column name \"%s\" appears more than once in referenced table",
									colname),
							 parser_errposition(pstate, location)));
				col_index = ndx;
			}
			ndx++;
		}

		if (col_index < 0)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_COLUMN),
					 errmsg("column \"%s\" does not exist in referenced table",
							colname),
					 parser_errposition(pstate, location)));

		pk_indexes[i] = col_index;
		pk_colinfos[i].colname = colname;
		pk_colinfos[i].relid = InvalidOid;
		pk_colinfos[i].attnum = InvalidAttrNumber;

		if (pk_rte->rtekind == RTE_RELATION)
		{
			char		pk_relkind = get_rel_relkind(pk_rte->relid);

			if (pk_relkind == RELKIND_VIEW)
			{
				Relation	pk_view_rel;
				Query	   *pk_viewquery;

				pk_view_rel = table_open(pk_rte->relid, AccessShareLock);
				pk_viewquery = get_view_query(pk_view_rel);
				traceFkColumn(pk_viewquery, colname,
							  &pk_colinfos[i].relid,
							  &pk_colinfos[i].attnum,
							  &pk_colinfos[i].leaf_varno,
							  location);
				table_close(pk_view_rel, AccessShareLock);
			}
			else
			{
				pk_colinfos[i].relid = pk_rte->relid;
				pk_colinfos[i].attnum = col_index + 1;
				pk_colinfos[i].leaf_varno = 0;
			}
		}
		else if (pk_rte->rtekind == RTE_SUBQUERY)
		{
			traceFkColumnByIndex(pk_rte->subquery, col_index,
								 &pk_colinfos[i].relid,
								 &pk_colinfos[i].attnum,
								 &pk_colinfos[i].leaf_varno);
		}
		else if (pk_rte->rtekind == RTE_CTE)
		{
			CommonTableExpr *cte = GetCTEForRTE(pstate, pk_rte, 0);

			if (cte != NULL && IsA(cte->ctequery, Query))
			{
				traceFkColumnByIndex((Query *) cte->ctequery, col_index,
									 &pk_colinfos[i].relid,
									 &pk_colinfos[i].attnum,
									 &pk_colinfos[i].leaf_varno);
			}
		}
		else if (pk_rte->rtekind == RTE_JOIN)
		{
			ParseNamespaceColumn *nscol = &pk_side_nscolumns[col_index];

			if (nscol->p_varno > 0)
			{
				RangeTblEntry *col_rte = rt_fetch(nscol->p_varno,
												  pstate->p_rtable);

				if (col_rte->rtekind == RTE_RELATION)
				{
					pk_colinfos[i].relid = col_rte->relid;
					pk_colinfos[i].attnum = nscol->p_varattno;
					pk_colinfos[i].leaf_varno = nscol->p_varno;
				}
				else if (col_rte->rtekind == RTE_SUBQUERY &&
						 col_rte->subquery != NULL)
				{
					traceFkColumnByIndex(col_rte->subquery,
										 nscol->p_varattno - 1,
										 &pk_colinfos[i].relid,
										 &pk_colinfos[i].attnum,
										 &pk_colinfos[i].leaf_varno);
				}
				else if (col_rte->rtekind == RTE_CTE)
				{
					CommonTableExpr *cte = GetCTEForRTE(pstate, col_rte, 0);

					if (cte != NULL && IsA(cte->ctequery, Query))
						traceFkColumnByIndex((Query *) cte->ctequery,
											 nscol->p_varattno - 1,
											 &pk_colinfos[i].relid,
											 &pk_colinfos[i].attnum,
											 &pk_colinfos[i].leaf_varno);
				}
			}
		}

		/*
		 * Capture the column's source location from the subquery's
		 * target entry for better error positioning.
		 */
		pk_colinfos[i].col_location = -1;
		if (pk_rte->rtekind == RTE_SUBQUERY && pk_rte->subquery != NULL)
		{
			ListCell   *tlc;
			int			tidx = 0;

			foreach(tlc, pk_rte->subquery->targetList)
			{
				TargetEntry *tle = (TargetEntry *) lfirst(tlc);

				if (tle->resjunk)
					continue;
				if (tidx == col_index && tle->expr != NULL)
				{
					pk_colinfos[i].col_location = exprLocation((Node *) tle->expr);
					break;
				}
				tidx++;
			}
		}

		i++;
	}

	/*
	 * Check that all FK columns belong to the same base table instance.
	 */
	for (i = 0; i < nkeys; i++)
	{
		if (!OidIsValid(fk_colinfos[i].relid))
		{
			/*
			 * Check for expression target entries and give specific errors.
			 */
			if (fk_rte->rtekind == RTE_RELATION &&
				get_rel_relkind(fk_rte->relid) == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("target entry \"%s\" is an expression, not a direct column reference",
								fk_colinfos[i].colname),
						 parser_errposition(pstate, location)));

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));
		}

		if (i == 0)
			fk_base_relid = fk_colinfos[i].relid;
		else if (fk_colinfos[i].relid != fk_base_relid ||
				 fk_colinfos[i].leaf_varno != fk_colinfos[0].leaf_varno)
		{
			int			errloc = fk_colinfos[i].col_location >= 0 ?
				fk_colinfos[i].col_location : location;

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("all key columns must belong to the same table"),
					 parser_errposition(pstate, errloc)));
		}
	}

	/*
	 * Check that all PK columns belong to the same base table instance.
	 */
	for (i = 0; i < nkeys; i++)
	{
		if (!OidIsValid(pk_colinfos[i].relid))
		{
			/*
			 * Give specific errors for expression target entries.
			 */
			if (pk_rte->rtekind == RTE_RELATION &&
				get_rel_relkind(pk_rte->relid) == RELKIND_VIEW)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("target entry \"%s\" is an expression, not a direct column reference",
								pk_colinfos[i].colname),
						 parser_errposition(pstate, location)));

			if (pk_rte->rtekind == RTE_SUBQUERY &&
				pk_rte->subquery != NULL &&
				pk_rte->subquery->groupClause != NIL)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("GROUP BY column %d is not a simple column reference",
								i + 1),
						 parser_errposition(pstate, location)));

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));
		}

		if (i == 0)
			pk_base_relid = pk_colinfos[i].relid;
		else if (pk_colinfos[i].relid != pk_base_relid ||
				 pk_colinfos[i].leaf_varno != pk_colinfos[0].leaf_varno)
		{
			int			errloc = pk_colinfos[i].col_location >= 0 ?
				pk_colinfos[i].col_location : location;

			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("all key columns must belong to the same table"),
					 parser_errposition(pstate, errloc)));
		}
	}

	/* Build attribute number arrays for FK constraint lookup */
	for (i = 0; i < nkeys; i++)
	{
		fk_attnums[i] = fk_colinfos[i].attnum;
		pk_attnums[i] = pk_colinfos[i].attnum;
	}

	/*
	 * Look up the FK constraint.
	 */
	conoid = lookupFkConstraint(fk_base_relid, pk_base_relid,
								nkeys, fk_attnums, pk_attnums);

	if (!OidIsValid(conoid))
	{
		/*
		 * Build error message with table names.
		 */
		StringInfoData fk_buf,
					pk_buf;
		const char *fk_relname;
		const char *pk_relname;

		/* Determine display names */
		if (arrow_dir == FK_JOIN_FORWARD)
		{
			fk_relname = r_nsitem->p_names->aliasname;
			pk_relname = ref_table->relname;
		}
		else
		{
			fk_relname = ref_table->relname;
			pk_relname = r_nsitem->p_names->aliasname;
		}

		/*
		 * Use a friendly name for unnamed derived tables.
		 */
		if (fk_relname && strcmp(fk_relname, "unnamed_join") == 0)
			fk_relname = "<unnamed derived table>";
		if (pk_relname && strcmp(pk_relname, "unnamed_join") == 0)
			pk_relname = "<unnamed derived table>";

		initStringInfo(&fk_buf);
		i = 0;
		foreach(lc, fk_col_names)
		{
			if (i > 0)
				appendStringInfoString(&fk_buf, ", ");
			appendStringInfoString(&fk_buf, strVal(lfirst(lc)));
			i++;
		}

		initStringInfo(&pk_buf);
		i = 0;
		foreach(lc, pk_col_names)
		{
			if (i > 0)
				appendStringInfoString(&pk_buf, ", ");
			appendStringInfoString(&pk_buf, strVal(lfirst(lc)));
			i++;
		}

		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("there is no foreign key constraint on table \"%s\" (%s) referencing table \"%s\" (%s)",
						fk_relname, fk_buf.data,
						pk_relname, pk_buf.data),
				 parser_errposition(pstate, location)));
	}

	/*
	 * Validate the referenced (PK) side.
	 */
	validateReferencedSide(pstate, pk_rte, pk_colinfos, nkeys, location);

	/*
	 * Store the FK constraint OID in the JoinExpr for dependency tracking.
	 */
	j->fk_constraint_oid = conoid;

	/*
	 * Record the FK constraint as a dependency so that views using FK joins
	 * get proper pg_depend entries.  Store it in the top-level ParseState's
	 * p_fk_constraint_deps; the caller will copy these to Query->constraintDeps.
	 */
	{
		ParseState *ps = pstate;

		while (ps->parentParseState != NULL)
			ps = ps->parentParseState;

		ps->p_fk_constraint_deps =
			list_append_unique_oid(ps->p_fk_constraint_deps, conoid);

		/*
		 * For INNER FK joins, row preservation depends on FK columns being
		 * NOT NULL.  Record those NOT NULL constraints as dependencies too,
		 * so that DROP NOT NULL properly cascades to views.
		 */
		if (j->jointype == JOIN_INNER)
		{
			HeapTuple	contup;
			Form_pg_constraint conForm;
			Datum		adatum;
			bool		isNull;
			ArrayType  *arr;
			int			numfkkeys;
			int16	   *fkattnums;
			int			k;
			Oid			fkrelid;

			contup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
			if (HeapTupleIsValid(contup))
			{
				conForm = (Form_pg_constraint) GETSTRUCT(contup);
				fkrelid = conForm->conrelid;

				adatum = SysCacheGetAttr(CONSTROID, contup,
										 Anum_pg_constraint_conkey, &isNull);
				if (!isNull)
				{
					arr = DatumGetArrayTypeP(adatum);
					numfkkeys = ARR_DIMS(arr)[0];
					fkattnums = (int16 *) ARR_DATA_PTR(arr);

					for (k = 0; k < numfkkeys; k++)
					{
						HeapTuple	nntup;

						nntup = findNotNullConstraintAttnum(fkrelid,
															fkattnums[k]);
						if (nntup != NULL)
						{
							ps->p_fk_constraint_deps =
								list_append_unique_oid(ps->p_fk_constraint_deps,
													   ((Form_pg_constraint) GETSTRUCT(nntup))->oid);
							heap_freetuple(nntup);
						}
					}
				}
				ReleaseSysCache(contup);
			}
		}
	}

	/*
	 * Build the equi-join condition.
	 */
	{
		ParseNamespaceColumn *left_nscolumns;
		ParseNamespaceColumn *right_nscolumns;
		int		   *left_indexes;
		int		   *right_indexes;

		if (arrow_dir == FK_JOIN_FORWARD)
		{
			/* FK cols are on the right (rarg), PK cols on the left */
			left_indexes = pk_indexes;
			left_nscolumns = pk_side_nscolumns;
			right_indexes = fk_indexes;
			right_nscolumns = fk_side_nscolumns;
		}
		else
		{
			/* PK cols are on the right (rarg), FK cols on the left */
			left_indexes = fk_indexes;
			left_nscolumns = fk_side_nscolumns;
			right_indexes = pk_indexes;
			right_nscolumns = pk_side_nscolumns;
		}

		result = buildFkJoinQuals(pstate,
								  left_nscolumns, left_indexes,
								  right_nscolumns, right_indexes,
								  nkeys);
	}

	/* Clean up module-level state */
	fkjoin_visible_ctes = NIL;

	return result;
}

/*
 * traceFkColumn
 *
 * Given a subquery and a column name, trace through the subquery's target
 * list to find the underlying base table and attribute number.
 *
 * Returns true if successfully traced, false if the column could not
 * be resolved to a base table column.
 */
static bool
traceFkColumn(Query *subquery, const char *colname,
			  Oid *relid, AttrNumber *attnum, int *leaf_varno,
			  int location)
{
	ListCell   *lc;
	TargetEntry *matching_tle = NULL;

	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;

		if (tle->resname && strcmp(tle->resname, colname) == 0)
		{
			matching_tle = tle;
			break;
		}
	}

	if (matching_tle == NULL)
		return false;

	return traceTargetEntryToBase(subquery, matching_tle, relid, attnum,
								  leaf_varno);
}

/*
 * traceFkColumnByIndex
 *
 * Like traceFkColumn but uses a positional index (0-based) rather than
 * a column name.  This is needed when the column names in the RTE
 * (possibly aliased) differ from the underlying query's target list names.
 */
static bool
traceFkColumnByIndex(Query *subquery, int col_index,
					 Oid *relid, AttrNumber *attnum,
					 int *leaf_varno)
{
	ListCell   *lc;
	int			idx = 0;

	foreach(lc, subquery->targetList)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(lc);

		if (tle->resjunk)
			continue;

		if (idx == col_index)
			return traceTargetEntryToBase(subquery, tle, relid, attnum,
										  leaf_varno);
		idx++;
	}

	return false;
}

/*
 * traceTargetEntryToBase
 *
 * Given a TargetEntry from a query, trace the expression back to find
 * the underlying base table OID and attribute number.
 *
 * Uses fkjoin_visible_ctes for resolving CTE references.
 */
static bool
traceTargetEntryToBase(Query *query, TargetEntry *tle,
					   Oid *relid, AttrNumber *attnum,
					   int *leaf_varno)
{
	Expr	   *expr = tle->expr;
	Var		   *var;
	RangeTblEntry *rte;

	if (expr == NULL)
		return false;

	/* Strip any RelabelType nodes */
	while (IsA(expr, RelabelType))
		expr = ((RelabelType *) expr)->arg;

	if (!IsA(expr, Var))
		return false;

	var = (Var *) expr;

	/*
	 * Follow the Var through indirection (RTE_JOIN, RTE_GROUP) until we
	 * reach a base table, subquery, or CTE that requires recursion.
	 */
	for (;;)
	{
		if (var->varno <= 0 || var->varno > list_length(query->rtable))
			return false;

		rte = rt_fetch(var->varno, query->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			char		relkind = get_rel_relkind(rte->relid);

			/*
			 * If the relation is a view, trace through its definition.
			 */
			if (relkind == RELKIND_VIEW)
			{
				Relation	view_rel;
				Query	   *viewquery;
				bool		found;

				view_rel = table_open(rte->relid, AccessShareLock);
				viewquery = get_view_query(view_rel);
				found = traceFkColumnByIndex(viewquery,
											 var->varattno - 1,
											 relid, attnum, leaf_varno);
				table_close(view_rel, AccessShareLock);
				return found;
			}

			*relid = rte->relid;
			*attnum = var->varattno;
			*leaf_varno = var->varno;
			return true;
		}
		else if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
		{
			TargetEntry *sub_tle;
			ListCell   *sub_lc;
			int			sub_idx = 0;

			foreach(sub_lc, rte->subquery->targetList)
			{
				sub_tle = (TargetEntry *) lfirst(sub_lc);
				if (sub_tle->resjunk)
					continue;
				sub_idx++;
				if (sub_idx == var->varattno)
				{
					/*
					 * Make CTEs from the current query and the subquery
					 * visible for deeper tracing.  The current query's
					 * CTEs are needed because the inner subquery may
					 * reference a CTE defined at this level (with
					 * ctelevelsup > 0).
					 */
					List	   *saved_ctes = fkjoin_visible_ctes;
					List	   *new_ctes = NIL;
					bool		result;

					if (query->cteList != NIL)
						new_ctes = list_concat_copy(query->cteList, new_ctes);
					if (rte->subquery->cteList != NIL)
						new_ctes = list_concat_copy(rte->subquery->cteList,
													new_ctes);
					if (new_ctes != NIL)
						fkjoin_visible_ctes =
							list_concat(new_ctes, fkjoin_visible_ctes);

					result = traceTargetEntryToBase(rte->subquery, sub_tle,
													relid, attnum,
													leaf_varno);

					if (new_ctes != NIL)
						list_free(fkjoin_visible_ctes);
					fkjoin_visible_ctes = saved_ctes;

					return result;
				}
			}
			return false;
		}
		else if (rte->rtekind == RTE_CTE)
		{
			/*
			 * Find the CTE by name.  First search the current query's
			 * cteList, then the module-level visible CTE list.
			 */
			CommonTableExpr *cte;

			cte = findCTEByName(query, rte->ctename);

			if (cte == NULL)
				return false;

			{
				Query	   *ctequery = (Query *) cte->ctequery;
				TargetEntry *cte_tle;
				ListCell   *cte_tle_lc;
				int			cte_idx = 0;

				foreach(cte_tle_lc, ctequery->targetList)
				{
					cte_tle = (TargetEntry *) lfirst(cte_tle_lc);
					if (cte_tle->resjunk)
						continue;
					cte_idx++;
					if (cte_idx == var->varattno)
						return traceTargetEntryToBase(ctequery, cte_tle,
													  relid, attnum,
													  leaf_varno);
				}
				return false;
			}
		}
		else if (rte->rtekind == RTE_GROUP)
		{
			/*
			 * Follow groupexprs to find the original column.  The
			 * groupexprs list has the pre-aggregation expressions.
			 */
			Node	   *groupexpr;

			if (rte->groupexprs == NIL ||
				var->varattno <= 0 ||
				var->varattno > list_length(rte->groupexprs))
				return false;

			groupexpr = (Node *) list_nth(rte->groupexprs,
										  var->varattno - 1);
			if (groupexpr == NULL)
				return false;

			while (IsA(groupexpr, RelabelType))
				groupexpr = (Node *) ((RelabelType *) groupexpr)->arg;

			if (!IsA(groupexpr, Var))
				return false;

			/* Loop with the resolved Var */
			var = (Var *) groupexpr;
			continue;
		}
		else if (rte->rtekind == RTE_JOIN)
		{
			/*
			 * Follow joinaliasvars to find the underlying column.
			 */
			Node	   *aliasvar;

			if (var->varattno <= 0 ||
				var->varattno > list_length(rte->joinaliasvars))
				return false;

			aliasvar = list_nth(rte->joinaliasvars,
								var->varattno - 1);

			while (IsA(aliasvar, RelabelType))
				aliasvar = (Node *) ((RelabelType *) aliasvar)->arg;

			if (!IsA(aliasvar, Var))
				return false;

			/* Loop with the resolved Var */
			var = (Var *) aliasvar;
			continue;
		}
		else
		{
			return false;
		}
	}
}

/*
 * findCTEByName
 *
 * Search for a CTE by name, first in the query's own cteList, then
 * in the module-level fkjoin_visible_ctes list.
 */
static CommonTableExpr *
findCTEByName(Query *query, const char *ctename)
{
	ListCell   *lc;

	/* First search the query's own cteList */
	foreach(lc, query->cteList)
	{
		CommonTableExpr *c = (CommonTableExpr *) lfirst(lc);

		if (strcmp(c->ctename, ctename) == 0 &&
			IsA(c->ctequery, Query))
			return c;
	}

	/* Fall back to the module-level visible CTEs */
	foreach(lc, fkjoin_visible_ctes)
	{
		CommonTableExpr *c = (CommonTableExpr *) lfirst(lc);

		if (strcmp(c->ctename, ctename) == 0 &&
			IsA(c->ctequery, Query))
			return c;
	}

	return NULL;
}

/*
 * lookupFkConstraint
 *
 * Look up a foreign key constraint matching the given tables and columns.
 * Returns the constraint OID, or InvalidOid if not found.
 */
static Oid
lookupFkConstraint(Oid fk_relid, Oid pk_relid,
				   int nkeys,
				   AttrNumber *fk_attnums,
				   AttrNumber *pk_attnums)
{
	Relation	fk_rel;
	List	   *fkeylist;
	ListCell   *lc;
	Oid			result = InvalidOid;

	fk_rel = table_open(fk_relid, AccessShareLock);
	fkeylist = RelationGetFKeyList(fk_rel);

	foreach(lc, fkeylist)
	{
		ForeignKeyCacheInfo *fk = (ForeignKeyCacheInfo *) lfirst(lc);

		if (fk->confrelid != pk_relid)
			continue;
		if (fk->nkeys != nkeys)
			continue;
		if (!fk->conenforced)
			continue;

		/*
		 * Check column pairs match as a set (order-independent).
		 * Each user-specified pair (fk_attnums[i], pk_attnums[i]) must
		 * match some constraint pair (conkey[j], confkey[j]).
		 */
		{
			bool		match = true;
			bool		used[INDEX_MAX_KEYS] = {false};
			int			i;

			for (i = 0; i < nkeys; i++)
			{
				bool		found = false;
				int			j;

				for (j = 0; j < nkeys; j++)
				{
					if (!used[j] &&
						fk->conkey[j] == fk_attnums[i] &&
						fk->confkey[j] == pk_attnums[i])
					{
						used[j] = true;
						found = true;
						break;
					}
				}
				if (!found)
				{
					match = false;
					break;
				}
			}

			if (match)
			{
				result = fk->conoid;
				break;
			}
		}
	}

	table_close(fk_rel, AccessShareLock);

	return result;
}

/*
 * validateReferencedSide
 *
 * Validate that the referenced (PK) side of a FK join preserves all rows
 * and preserves uniqueness of keys.  This checks:
 * - No WHERE, HAVING, LIMIT, OFFSET
 * - No filtering joins
 * - No one-to-many joins that destroy uniqueness
 */
static void
validateReferencedSide(ParseState *pstate,
					   RangeTblEntry *rte,
					   FkColumnInfo *pk_colinfos,
					   int nkeys,
					   int location)
{
	/*
	 * For base tables, no validation of the relation itself is needed -
	 * base tables inherently preserve all rows and uniqueness.
	 */
	if (rte->rtekind == RTE_RELATION)
	{
		char		relkind = get_rel_relkind(rte->relid);

		/* Materialized views are not supported */
		if (relkind == RELKIND_MATVIEW)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 errdetail("This operation is not supported for materialized views."),
					 parser_errposition(pstate, location)));

		/* For views, validate the underlying query */
		if (relkind == RELKIND_VIEW)
		{
			Relation	view_rel;
			Query	   *viewquery;
			RangeTblEntry fake_rte;

			view_rel = table_open(rte->relid, AccessShareLock);
			viewquery = get_view_query(view_rel);

			memset(&fake_rte, 0, sizeof(RangeTblEntry));
			fake_rte.type = T_RangeTblEntry;
			fake_rte.rtekind = RTE_SUBQUERY;
			fake_rte.subquery = viewquery;

			table_close(view_rel, AccessShareLock);

			validateReferencedSide(pstate, &fake_rte, pk_colinfos, nkeys,
								  location);
			return;
		}

		/* Plain base table - inherently preserves rows and uniqueness */
		return;
	}

	if (rte->rtekind == RTE_FUNCTION)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving this type of relation are not supported"),
				 parser_errposition(pstate, location)));

	if (rte->rtekind == RTE_SUBQUERY)
	{
		Query	   *subquery = rte->subquery;

		if (subquery == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));

		/* Check for set operations */
		if (subquery->setOperations != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving set operations are not supported"),
					 parser_errposition(pstate, location)));

		/* Check for WHERE clause (row filtering) */
		if (subquery->jointree && subquery->jointree->quals != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("foreign key join violation"),
					 errdetail("referenced relation does not preserve all rows"),
					 parser_errposition(pstate, location)));

		/* Check for HAVING clause */
		if (subquery->havingQual != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("foreign key join violation"),
					 errdetail("referenced relation does not preserve all rows"),
					 parser_errposition(pstate, location)));

		/* Check for LIMIT */
		if (subquery->limitCount != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("foreign key join violation"),
					 errdetail("referenced relation does not preserve all rows"),
					 parser_errposition(pstate, location)));

		/* Check for OFFSET */
		if (subquery->limitOffset != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FOREIGN_KEY),
					 errmsg("foreign key join violation"),
					 errdetail("referenced relation does not preserve all rows"),
					 parser_errposition(pstate, location)));

		/*
		 * Check joins within the subquery for uniqueness and row
		 * preservation.
		 */
		if (subquery->jointree)
		{
			Oid			base_relid = pk_colinfos[0].relid;
			AttrNumber	sub_pk_attnums[INDEX_MAX_KEYS];
			int			i;

			for (i = 0; i < nkeys; i++)
				sub_pk_attnums[i] = pk_colinfos[i].attnum;

			/*
			 * If there's a GROUP BY, check if it can restore uniqueness.
			 */
			if (subquery->groupClause != NIL)
			{
				if (!checkGroupByRestoresUniqueness(subquery, base_relid,
													sub_pk_attnums, nkeys,
													location))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FOREIGN_KEY),
							 errmsg("foreign key join violation"),
							 errdetail("referenced relation does not preserve all rows"),
							 parser_errposition(pstate, location)));
			}
			else
			{
				/*
				 * No GROUP BY: check uniqueness first, then row
				 * preservation.  Uniqueness is checked first because
				 * one-to-many joins are a more specific diagnostic.
				 */
				if (!checkUniquenessPreservation(subquery, base_relid,
												 sub_pk_attnums, nkeys))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FOREIGN_KEY),
							 errmsg("foreign key join violation"),
							 errdetail("referenced relation does not preserve uniqueness of keys"),
							 parser_errposition(pstate, location)));

				if (!checkRowPreservation(subquery, base_relid))
					ereport(ERROR,
							(errcode(ERRCODE_INVALID_FOREIGN_KEY),
							 errmsg("foreign key join violation"),
							 errdetail("referenced relation does not preserve all rows"),
							 parser_errposition(pstate, location)));
			}
		}

		return;
	}

	if (rte->rtekind == RTE_CTE)
	{
		CommonTableExpr *cte = GetCTEForRTE(pstate, rte, 0);

		if (cte == NULL || !IsA(cte->ctequery, Query))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));

		/* Recursive CTEs are not supported */
		if (cte->cterecursive)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key joins involving this type of relation are not supported"),
					 parser_errposition(pstate, location)));

		/*
		 * For non-recursive CTEs, validate the CTE's query as if it were
		 * a subquery.
		 */
		{
			Query	   *ctequery = (Query *) cte->ctequery;
			RangeTblEntry fake_rte;

			memset(&fake_rte, 0, sizeof(RangeTblEntry));
			fake_rte.type = T_RangeTblEntry;
			fake_rte.rtekind = RTE_SUBQUERY;
			fake_rte.subquery = ctequery;

			validateReferencedSide(pstate, &fake_rte, pk_colinfos, nkeys,
								  location);
		}

		return;
	}

	/* Other RTE types not supported */
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("foreign key joins involving this type of relation are not supported"),
			 parser_errposition(pstate, location)));
}

/*
 * checkRowPreservation
 *
 * Check that the joins in a query preserve all rows of the specified
 * base table.  Recursively walks the join tree.
 *
 * Join types and row preservation:
 * - LEFT JOIN preserves left side's rows
 * - RIGHT JOIN preserves right side's rows
 * - FULL JOIN preserves both sides' rows
 * - INNER FK JOIN preserves the FK side's rows if FK columns are NOT NULL
 * - Cross joins (multiple FROM items) preserve all rows
 */
static bool
checkRowPreservation(Query *query, Oid base_relid)
{
	if (query->jointree == NULL)
		return true;

	/* WHERE and HAVING clauses filter rows */
	if (query->jointree->quals != NULL)
		return false;
	if (query->havingQual != NULL)
		return false;

	/* LIMIT and OFFSET can reduce rows */
	if (query->limitCount != NULL || query->limitOffset != NULL)
		return false;

	return checkRowPreservationInNode(query, (Node *) query->jointree,
									  base_relid);
}

/*
 * checkRowPreservationInNode
 *
 * Recursive helper for checkRowPreservation.  Walks a single node
 * of the join tree.
 */
static bool
checkRowPreservationInNode(Query *query, Node *jtnode, Oid base_relid)
{
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		/*
		 * In a FROM list, items are implicitly cross-joined, which
		 * preserves all rows from each side.  Find the item containing
		 * the base table and recurse into it.
		 */
		foreach(lc, f->fromlist)
		{
			Node	   *item = (Node *) lfirst(lc);

			if (nodeContainsBaseRelid(query, item, base_relid))
				return checkRowPreservationInNode(query, item, base_relid);
		}
		return false;			/* base table not found */
	}

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);

		/*
		 * For views, check that the view's internal join tree preserves
		 * all rows of the base table.
		 */
		if (rte->rtekind == RTE_RELATION &&
			get_rel_relkind(rte->relid) == RELKIND_VIEW)
		{
			Relation	view_rel;
			Query	   *viewquery;
			bool		result;

			view_rel = table_open(rte->relid, AccessShareLock);
			viewquery = get_view_query(view_rel);
			result = checkRowPreservation(viewquery, base_relid);
			table_close(view_rel, AccessShareLock);
			return result;
		}

		/*
		 * For subqueries, check that the subquery preserves all rows
		 * of the base table (including checking for WHERE/HAVING/LIMIT).
		 */
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
			return checkRowPreservation(rte->subquery, base_relid);

		/*
		 * For CTEs, check that the CTE query preserves all rows.
		 */
		if (rte->rtekind == RTE_CTE)
		{
			CommonTableExpr *cte;

			cte = findCTEByName(query, rte->ctename);
			if (cte != NULL && IsA(cte->ctequery, Query))
				return checkRowPreservation((Query *) cte->ctequery,
										   base_relid);
			return false;
		}

		/* Base table reference - trivially preserved */
		return true;
	}

	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		in_left = nodeContainsBaseRelid(query, j->larg,
												   base_relid);
		bool		in_right = nodeContainsBaseRelid(query, j->rarg,
													base_relid);

		if (!in_left && !in_right)
			return false;

		if (in_left)
		{
			bool		preserved;

			switch (j->jointype)
			{
				case JOIN_LEFT:
				case JOIN_FULL:
					preserved = true;
					break;
				case JOIN_INNER:

					/*
					 * An INNER FK join preserves the FK side's rows only
					 * if the FK columns are NOT NULL.  With nullable FK
					 * columns, NULL values won't match in the equi-join.
					 *
					 * FK_JOIN_REVERSE: FK is on the left (arrow target),
					 * PK is on the right (rarg).  Left rows preserved
					 * if FK columns are NOT NULL.
					 */
					if (j->fk_arrow_dir == FK_JOIN_REVERSE &&
						OidIsValid(j->fk_constraint_oid) &&
						checkFkColumnsNotNull(j->fk_constraint_oid))
						preserved = true;
					else
						preserved = false;
					break;
				default:
					preserved = false;
					break;
			}

			if (!preserved)
				return false;

			return checkRowPreservationInNode(query, j->larg, base_relid);
		}
		else
		{
			/* in_right */
			bool		preserved;

			switch (j->jointype)
			{
				case JOIN_RIGHT:
				case JOIN_FULL:
					preserved = true;
					break;
				case JOIN_INNER:

					/*
					 * FK_JOIN_FORWARD: FK is on the right (rarg), PK is
					 * on the left (arrow target).  Right rows preserved
					 * if FK columns are NOT NULL.
					 */
					if (j->fk_arrow_dir == FK_JOIN_FORWARD &&
						OidIsValid(j->fk_constraint_oid) &&
						checkFkColumnsNotNull(j->fk_constraint_oid))
						preserved = true;
					else
						preserved = false;
					break;
				default:
					preserved = false;
					break;
			}

			if (!preserved)
				return false;

			return checkRowPreservationInNode(query, j->rarg, base_relid);
		}
	}

	return false;
}

/*
 * nodeContainsBaseRelid
 *
 * Check whether a join tree node contains a reference to the specified
 * base relation OID.  Searches through subqueries and CTEs.
 */
static bool
nodeContainsBaseRelid(Query *query, Node *jtnode, Oid base_relid)
{
	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);

		if (rte->rtekind == RTE_RELATION)
		{
			if (rte->relid == base_relid)
				return true;

			/*
			 * If it's a view, check whether the view's underlying query
			 * references the base table (recursively handles nested views).
			 */
			if (get_rel_relkind(rte->relid) == RELKIND_VIEW)
			{
				Relation	view_rel;
				Query	   *viewquery;
				bool		found = false;

				view_rel = table_open(rte->relid, AccessShareLock);
				viewquery = get_view_query(view_rel);
				if (viewquery->jointree != NULL)
					found = nodeContainsBaseRelid(viewquery,
												  (Node *) viewquery->jointree,
												  base_relid);
				table_close(view_rel, AccessShareLock);
				return found;
			}
			return false;
		}

		/*
		 * For subqueries, recursively check the subquery's join tree.
		 */
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL)
		{
			if (rte->subquery->jointree != NULL)
				return nodeContainsBaseRelid(rte->subquery,
											(Node *) rte->subquery->jointree,
											base_relid);
			return false;
		}
		else if (rte->rtekind == RTE_CTE)
		{
			/*
			 * Look up the CTE's query and check if it references the
			 * base table.
			 */
			CommonTableExpr *cte;

			cte = findCTEByName(query, rte->ctename);
			if (cte != NULL)
			{
				Query	   *ctequery = (Query *) cte->ctequery;

				if (ctequery->jointree != NULL)
					return nodeContainsBaseRelid(ctequery,
												(Node *) ctequery->jointree,
												base_relid);
			}
		}

		return false;
	}
	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		return nodeContainsBaseRelid(query, j->larg, base_relid) ||
			nodeContainsBaseRelid(query, j->rarg, base_relid);
	}
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (nodeContainsBaseRelid(query, (Node *) lfirst(lc),
									  base_relid))
				return true;
		}
	}
	return false;
}

/*
 * checkUniquenessPreservation
 *
 * Check that joins in a query preserve the uniqueness of the specified
 * key columns on the base table.  Recursively walks the join tree.
 *
 * Joins that preserve uniqueness:
 * - Many-to-one FK lookups (base table is FK side)
 * - LEFT/RIGHT/FULL joins (null-padding doesn't duplicate)
 *
 * Joins that destroy uniqueness:
 * - One-to-many joins where a child table references the base table's PK
 */
static bool
checkUniquenessPreservation(Query *query, Oid base_relid,
							AttrNumber *pk_attnums, int nkeys)
{
	if (query->jointree == NULL)
		return true;

	return checkUniquenessInNode(query, (Node *) query->jointree,
								 base_relid);
}

/*
 * checkUniquenessInNode
 *
 * Recursive helper: check if any join in this node destroys the
 * uniqueness of the base table's key columns.
 */
static bool
checkUniquenessInNode(Query *query, Node *jtnode, Oid base_relid)
{
	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			Node	   *item = (Node *) lfirst(lc);

			if (nodeContainsBaseRelid(query, item, base_relid))
				return checkUniquenessInNode(query, item, base_relid);
		}
		return true;
	}

	if (IsA(jtnode, RangeTblRef))
	{
		int			varno = ((RangeTblRef *) jtnode)->rtindex;
		RangeTblEntry *rte = rt_fetch(varno, query->rtable);

		/*
		 * For views, check that the view's internal joins preserve
		 * uniqueness of the base table.
		 */
		if (rte->rtekind == RTE_RELATION &&
			get_rel_relkind(rte->relid) == RELKIND_VIEW)
		{
			Relation	view_rel;
			Query	   *viewquery;
			bool		result;

			view_rel = table_open(rte->relid, AccessShareLock);
			viewquery = get_view_query(view_rel);
			result = checkUniquenessInNode(viewquery,
										   (Node *) viewquery->jointree,
										   base_relid);
			table_close(view_rel, AccessShareLock);
			return result;
		}

		/* For subqueries, check within the subquery */
		if (rte->rtekind == RTE_SUBQUERY && rte->subquery != NULL &&
			rte->subquery->jointree != NULL)
			return checkUniquenessInNode(rte->subquery,
										 (Node *) rte->subquery->jointree,
										 base_relid);

		return true;
	}

	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;
		bool		in_left = nodeContainsBaseRelid(query, j->larg,
												   base_relid);
		bool		in_right = nodeContainsBaseRelid(query, j->rarg,
													base_relid);

		if (!in_left && !in_right)
			return true;		/* base table not in this join subtree */

		/*
		 * For FK joins, check if this join introduces duplicates.
		 * A join where base_relid is the PK side is one-to-many
		 * (each base row can match multiple FK rows), destroying
		 * uniqueness.
		 */
		if (j->fk_arrow_dir != FK_JOIN_NONE &&
			OidIsValid(j->fk_constraint_oid))
		{
			Oid			inner_pk_relid = InvalidOid;
			HeapTuple	tup;

			tup = SearchSysCache1(CONSTROID,
								  ObjectIdGetDatum(j->fk_constraint_oid));
			if (HeapTupleIsValid(tup))
			{
				Form_pg_constraint con;

				con = (Form_pg_constraint) GETSTRUCT(tup);
				inner_pk_relid = con->confrelid;
				ReleaseSysCache(tup);
			}

			/*
			 * If base_relid is the PK side of this FK join, this is
			 * a one-to-many join that destroys uniqueness.
			 */
			if (inner_pk_relid == base_relid)
				return false;
		}
		else if (j->fk_arrow_dir == FK_JOIN_NONE)
		{
			/*
			 * Non-FK INNER join with a table that isn't known to be
			 * many-to-one.  This could destroy uniqueness.
			 */
			if (j->jointype == JOIN_INNER)
				return false;
		}

		/* Recurse into the subtree containing the base table */
		if (in_left)
			return checkUniquenessInNode(query, j->larg, base_relid);
		else
			return checkUniquenessInNode(query, j->rarg, base_relid);
	}

	return true;
}

/*
 * checkFkColumnsNotNull
 *
 * Check that all FK (referencing) columns of a constraint have NOT NULL
 * constraints.  Returns true if all FK columns are NOT NULL.
 */
static bool
checkFkColumnsNotNull(Oid conoid)
{
	HeapTuple	tup;
	Form_pg_constraint con;
	Oid			fk_relid;
	Datum		adatum;
	bool		isNull;
	ArrayType  *arr;
	int			numkeys;
	int16	   *attnums;
	int			i;
	bool		allNotNull = true;

	tup = SearchSysCache1(CONSTROID, ObjectIdGetDatum(conoid));
	if (!HeapTupleIsValid(tup))
		return false;

	con = (Form_pg_constraint) GETSTRUCT(tup);
	fk_relid = con->conrelid;

	/* Get the FK column attribute numbers */
	adatum = SysCacheGetAttr(CONSTROID, tup,
							 Anum_pg_constraint_conkey, &isNull);
	if (isNull)
	{
		ReleaseSysCache(tup);
		return false;
	}

	arr = DatumGetArrayTypeP(adatum);
	numkeys = ARR_DIMS(arr)[0];
	attnums = (int16 *) ARR_DATA_PTR(arr);

	/* Check each FK column for NOT NULL */
	for (i = 0; i < numkeys; i++)
	{
		HeapTuple	atttup;

		atttup = SearchSysCache2(ATTNUM,
								 ObjectIdGetDatum(fk_relid),
								 Int16GetDatum(attnums[i]));
		if (HeapTupleIsValid(atttup))
		{
			Form_pg_attribute att = (Form_pg_attribute) GETSTRUCT(atttup);

			if (!att->attnotnull)
				allNotNull = false;
			ReleaseSysCache(atttup);
		}
		else
		{
			allNotNull = false;
		}

		if (!allNotNull)
			break;
	}

	ReleaseSysCache(tup);
	return allNotNull;
}

/*
 * checkGroupByRestoresUniqueness
 *
 * When a subquery has GROUP BY, check if the grouping columns correspond
 * to a PK or UNIQUE constraint on the base table, and if so, whether
 * the joins preserve all rows of the base table.
 */
static bool
checkGroupByRestoresUniqueness(Query *query, Oid base_relid,
							   AttrNumber *pk_attnums, int nkeys,
							   int location)
{
	ListCell   *lc;
	int			nGroupCols = 0;
	AttrNumber *groupAttnums;
	Oid			groupRelid = InvalidOid;
	int			groupVarno = 0;
	bool		allSimpleVars = true;

	/*
	 * First, verify all GROUP BY columns are simple column references
	 * from the same base table instance.
	 */
	groupAttnums = (AttrNumber *) palloc(list_length(query->groupClause) *
										 sizeof(AttrNumber));

	foreach(lc, query->groupClause)
	{
		SortGroupClause *sgc = (SortGroupClause *) lfirst(lc);
		TargetEntry *tle;
		Expr	   *expr;
		Var		   *var;
		RangeTblEntry *rte;

		tle = get_sortgroupclause_tle(sgc, query->targetList);

		if (tle == NULL)
		{
			allSimpleVars = false;
			break;
		}

		expr = tle->expr;
		while (IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;

		if (!IsA(expr, Var))
		{
			allSimpleVars = false;
			break;
		}

		var = (Var *) expr;
		rte = rt_fetch(var->varno, query->rtable);

		/*
		 * If this is an RTE_GROUP reference, follow through the
		 * groupexprs to find the underlying base table column.
		 */
		if (rte->rtekind == RTE_GROUP)
		{
			Node	   *gexpr;

			if (rte->groupexprs == NIL ||
				var->varattno <= 0 ||
				var->varattno > list_length(rte->groupexprs))
			{
				allSimpleVars = false;
				break;
			}

			gexpr = (Node *) list_nth(rte->groupexprs,
									  var->varattno - 1);

			/* Strip type coercions */
			while (gexpr && IsA(gexpr, RelabelType))
				gexpr = (Node *) ((RelabelType *) gexpr)->arg;

			if (gexpr == NULL || !IsA(gexpr, Var))
			{
				allSimpleVars = false;
				break;
			}

			var = (Var *) gexpr;
			if (var->varno <= 0 || var->varno > list_length(query->rtable))
			{
				allSimpleVars = false;
				break;
			}
			rte = rt_fetch(var->varno, query->rtable);
		}

		if (rte->rtekind != RTE_RELATION)
		{
			allSimpleVars = false;
			break;
		}

		if (nGroupCols == 0)
		{
			groupRelid = rte->relid;
			groupVarno = var->varno;
		}
		else if (rte->relid != groupRelid || var->varno != groupVarno)
		{
			/* Columns from different tables or different instances */
			allSimpleVars = false;
			break;
		}

		groupAttnums[nGroupCols] = var->varattno;
		nGroupCols++;
	}

	if (!allSimpleVars)
	{
		pfree(groupAttnums);
		return false;
	}

	/*
	 * Also need to check row preservation: the joins must preserve all
	 * rows of the base table.  With GROUP BY, row preservation is checked
	 * through the join types (LEFT/RIGHT/FULL/FK INNER).
	 */
	if (!checkRowPreservation(query, base_relid))
	{
		pfree(groupAttnums);
		return false;
	}

	pfree(groupAttnums);
	return true;
}

/*
 * buildFkJoinQuals
 *
 * Build the equi-join condition from the resolved column references.
 * This is modeled after transformJoinUsingClause.
 */
static Node *
buildFkJoinQuals(ParseState *pstate,
				 ParseNamespaceColumn *left_nscolumns, int *left_indexes,
				 ParseNamespaceColumn *right_nscolumns, int *right_indexes,
				 int nkeys)
{
	List	   *l_usingvars = NIL;
	List	   *r_usingvars = NIL;
	Node	   *result;
	int			i;

	for (i = 0; i < nkeys; i++)
	{
		Var		   *lvar = buildVarFromNSColumn(pstate,
												left_nscolumns + left_indexes[i]);
		Var		   *rvar = buildVarFromNSColumn(pstate,
												right_nscolumns + right_indexes[i]);

		l_usingvars = lappend(l_usingvars, lvar);
		r_usingvars = lappend(r_usingvars, rvar);
	}

	/* Build equality conditions using transformJoinUsingClause */
	result = transformJoinUsingClause(pstate, l_usingvars, r_usingvars);

	return result;
}

/*
 * findRteByName
 *
 * Search the namespace for a range table entry with the given name.
 */
static RangeTblEntry *
findRteByName(ParseState *pstate, const char *refname,
			  List *my_namespace, ParseNamespaceItem **nsitem_out,
			  int location)
{
	ListCell   *lc;

	foreach(lc, my_namespace)
	{
		ParseNamespaceItem *nsitem = (ParseNamespaceItem *) lfirst(lc);

		if (nsitem->p_names &&
			nsitem->p_names->aliasname &&
			strcmp(nsitem->p_names->aliasname, refname) == 0)
		{
			if (nsitem_out)
				*nsitem_out = nsitem;
			return nsitem->p_rte;
		}
	}

	return NULL;
}

/*
 * revalidateFkJoinInQuery
 *
 * Walk a query's join tree looking for FK join JoinExprs.  For each one found,
 * re-check that the referenced side still preserves rows and uniqueness.
 * Returns true if all FK joins are still valid, false otherwise.
 */
static bool
revalidateFkJoinInQuery(Query *query)
{
	if (query == NULL || query->jointree == NULL)
		return true;

	return revalidateFkJoinInNode(query, (Node *) query->jointree);
}

static bool
revalidateFkJoinInNode(Query *query, Node *jtnode)
{
	if (jtnode == NULL)
		return true;

	if (IsA(jtnode, RangeTblRef))
	{
		/*
		 * We don't need to recurse into referenced views or subqueries
		 * here. FK joins within those views have their own pg_depend
		 * entries and will be validated independently when those views
		 * are replaced. We only check FK joins at the current query
		 * level's join tree.
		 */
		return true;
	}

	if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *j = (JoinExpr *) jtnode;

		/* Check child nodes first */
		if (!revalidateFkJoinInNode(query, j->larg))
			return false;
		if (!revalidateFkJoinInNode(query, j->rarg))
			return false;

		/* If this is an FK join, re-validate the referenced side */
		if (j->fk_arrow_dir != FK_JOIN_NONE &&
			OidIsValid(j->fk_constraint_oid))
		{
			RangeTblEntry *pk_rte;
			int			pk_rtindex;
			Oid			pk_base_relid;
			AttrNumber	pk_attnums[INDEX_MAX_KEYS];
			int			nkeys;
			HeapTuple	contup;
			Form_pg_constraint conForm;

			/*
			 * Determine which side is the PK (referenced) side.
			 * FORWARD: left (larg) is PK side.
			 * REVERSE: right (rarg) is PK side.
			 */
			if (j->fk_arrow_dir == FK_JOIN_FORWARD)
			{
				/* PK side is the arrow target (found via fk_ref_rtindex) */
				pk_rtindex = j->fk_ref_rtindex;
			}
			else
			{
				/* PK side is rarg */
				if (IsA(j->rarg, RangeTblRef))
					pk_rtindex = ((RangeTblRef *) j->rarg)->rtindex;
				else
					return true;  /* can't determine, skip */
			}

			if (pk_rtindex <= 0 || pk_rtindex > list_length(query->rtable))
				return true;

			pk_rte = rt_fetch(pk_rtindex, query->rtable);

			/*
			 * Look up the FK constraint to get PK base relid and attnums.
			 */
			contup = SearchSysCache1(CONSTROID,
									 ObjectIdGetDatum(j->fk_constraint_oid));
			if (!HeapTupleIsValid(contup))
				return false;

			conForm = (Form_pg_constraint) GETSTRUCT(contup);
			pk_base_relid = conForm->confrelid;

			/* Get PK column attnums from confkey */
			{
				Datum		adatum;
				bool		isNull;
				ArrayType  *arr;
				int16	   *attnums_raw;
				int			i;

				adatum = SysCacheGetAttr(CONSTROID, contup,
										 Anum_pg_constraint_confkey, &isNull);
				if (isNull)
				{
					ReleaseSysCache(contup);
					return false;
				}

				arr = DatumGetArrayTypeP(adatum);
				nkeys = ARR_DIMS(arr)[0];
				attnums_raw = (int16 *) ARR_DATA_PTR(arr);

				for (i = 0; i < nkeys && i < INDEX_MAX_KEYS; i++)
					pk_attnums[i] = attnums_raw[i];
			}

			ReleaseSysCache(contup);

			/*
			 * Check the PK side: for views and subqueries, we need to
			 * verify that the underlying query still preserves all rows
			 * and uniqueness.
			 */
			if (pk_rte->rtekind == RTE_RELATION &&
				get_rel_relkind(pk_rte->relid) == RELKIND_VIEW)
			{
				Relation	view_rel;
				Query	   *viewquery;

				view_rel = table_open(pk_rte->relid, AccessShareLock);
				viewquery = get_view_query(view_rel);

				/* Check row preservation */
				if (!checkRowPreservation(viewquery, pk_base_relid))
				{
					table_close(view_rel, AccessShareLock);
					return false;
				}

				/* Check uniqueness */
				if (!checkUniquenessPreservation(viewquery, pk_base_relid,
												 pk_attnums, nkeys))
				{
					table_close(view_rel, AccessShareLock);
					return false;
				}

				table_close(view_rel, AccessShareLock);
			}
			else if (pk_rte->rtekind == RTE_SUBQUERY && pk_rte->subquery != NULL)
			{
				if (!checkRowPreservation(pk_rte->subquery, pk_base_relid))
					return false;
				if (!checkUniquenessPreservation(pk_rte->subquery, pk_base_relid,
												 pk_attnums, nkeys))
					return false;
			}
		}

		return true;
	}

	if (IsA(jtnode, FromExpr))
	{
		FromExpr   *f = (FromExpr *) jtnode;
		ListCell   *lc;

		foreach(lc, f->fromlist)
		{
			if (!revalidateFkJoinInNode(query, (Node *) lfirst(lc)))
				return false;
		}
		return true;
	}

	return true;
}

/*
 * validateFkJoinView
 *
 * Revalidate FK joins in dependent views when a view is replaced.
 * Called from view.c during CREATE OR REPLACE VIEW.
 *
 * This scans pg_depend to find views that depend on the given view,
 * then re-validates any FK joins in those views.
 */
void
validateFkJoinView(Oid viewOid)
{
	Relation	depRel;
	SysScanDesc scan;
	ScanKeyData key[2];
	HeapTuple	tup;

	depRel = table_open(DependRelationId, AccessShareLock);

	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(RelationRelationId));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(viewOid));

	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid(tup = systable_getnext(scan)))
	{
		Form_pg_depend foundDep = (Form_pg_depend) GETSTRUCT(tup);
		Oid			depViewOid = InvalidOid;

		/*
		 * View dependencies in pg_depend can appear in two forms:
		 * 1. classid = RelationRelationId (direct relation dependency)
		 * 2. classid = RewriteRelationId (dependency via the view's
		 *    rewrite rule, which is the more common case)
		 *
		 * For rewrite rule dependencies, we resolve the rule OID to the
		 * owning view via pg_rewrite.ev_class.
		 */
		if (foundDep->classid == RelationRelationId)
		{
			depViewOid = foundDep->objid;
		}
		else if (foundDep->classid == RewriteRelationId)
		{
			/* Look up the rewrite rule to find the owning view */
			Relation	rwRel;
			SysScanDesc rwScan;
			ScanKeyData rwKey[1];
			HeapTuple	rwTup;

			rwRel = table_open(RewriteRelationId, AccessShareLock);

			ScanKeyInit(&rwKey[0],
						Anum_pg_rewrite_oid,
						BTEqualStrategyNumber, F_OIDEQ,
						ObjectIdGetDatum(foundDep->objid));

			rwScan = systable_beginscan(rwRel, RewriteOidIndexId, true,
										NULL, 1, rwKey);

			rwTup = systable_getnext(rwScan);
			if (HeapTupleIsValid(rwTup))
			{
				Form_pg_rewrite rwForm = (Form_pg_rewrite) GETSTRUCT(rwTup);

				depViewOid = rwForm->ev_class;
			}

			systable_endscan(rwScan);
			table_close(rwRel, AccessShareLock);
		}

		if (!OidIsValid(depViewOid))
			continue;

		/* Skip self-references */
		if (depViewOid == viewOid)
			continue;

		if (get_rel_relkind(depViewOid) == RELKIND_VIEW)
		{
			Relation	depView;
			Query	   *depQuery;

			depView = table_open(depViewOid, AccessShareLock);
			depQuery = get_view_query(depView);

			if (!revalidateFkJoinInQuery(depQuery))
			{
				char	   *viewname = get_rel_name(depViewOid);
				char	   *nspname = get_namespace_name(get_rel_namespace(depViewOid));

				table_close(depView, AccessShareLock);
				systable_endscan(scan);
				table_close(depRel, AccessShareLock);

				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FOREIGN_KEY),
						 errmsg("virtual foreign key constraint violation while re-validating view \"%s.%s\"",
								nspname, viewname)));
			}

			table_close(depView, AccessShareLock);
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
}
