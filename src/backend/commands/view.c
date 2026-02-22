/*-------------------------------------------------------------------------
 *
 * view.c
 *	  use rewrite rules to construct views
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/view.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/pg_depend.h"
#include "catalog/namespace.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_rewrite.h"
#include "commands/tablecmds.h"
#include "commands/view.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/analyze.h"
#include "parser/parser.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteHandler.h"
#include "rewrite/rewriteSupport.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static void checkViewColumns(TupleDesc newdesc, TupleDesc olddesc);
static void UpdateViewFKInfo(Oid viewOid, Query *viewParse);
static void revalidateDependentViews(Oid viewOid);

/*---------------------------------------------------------------------
 * DefineVirtualRelation
 *
 * Create a view relation and use the rules system to store the query
 * for the view.
 *
 * EventTriggerAlterTableStart must have been called already.
 *---------------------------------------------------------------------
 */
static ObjectAddress
DefineVirtualRelation(RangeVar *relation, List *tlist, bool replace,
					  List *options, Query *viewParse)
{
	Oid			viewOid;
	LOCKMODE	lockmode;
	List	   *attrList;
	ListCell   *t;

	/*
	 * create a list of ColumnDef nodes based on the names and types of the
	 * (non-junk) targetlist items from the view's SELECT list.
	 */
	attrList = NIL;
	foreach(t, tlist)
	{
		TargetEntry *tle = (TargetEntry *) lfirst(t);

		if (!tle->resjunk)
		{
			ColumnDef  *def = makeColumnDef(tle->resname,
											exprType((Node *) tle->expr),
											exprTypmod((Node *) tle->expr),
											exprCollation((Node *) tle->expr));

			/*
			 * It's possible that the column is of a collatable type but the
			 * collation could not be resolved, so double-check.
			 */
			if (type_is_collatable(exprType((Node *) tle->expr)))
			{
				if (!OidIsValid(def->collOid))
					ereport(ERROR,
							(errcode(ERRCODE_INDETERMINATE_COLLATION),
							 errmsg("could not determine which collation to use for view column \"%s\"",
									def->colname),
							 errhint("Use the COLLATE clause to set the collation explicitly.")));
			}
			else
				Assert(!OidIsValid(def->collOid));

			attrList = lappend(attrList, def);
		}
	}

	/*
	 * Look up, check permissions on, and lock the creation namespace; also
	 * check for a preexisting view with the same name.  This will also set
	 * relation->relpersistence to RELPERSISTENCE_TEMP if the selected
	 * namespace is temporary.
	 */
	lockmode = replace ? AccessExclusiveLock : NoLock;
	(void) RangeVarGetAndCheckCreationNamespace(relation, lockmode, &viewOid);

	if (OidIsValid(viewOid) && replace)
	{
		Relation	rel;
		TupleDesc	descriptor;
		List	   *atcmds = NIL;
		AlterTableCmd *atcmd;
		ObjectAddress address;

		/* Relation is already locked, but we must build a relcache entry. */
		rel = relation_open(viewOid, NoLock);

		/* Make sure it *is* a view. */
		if (rel->rd_rel->relkind != RELKIND_VIEW)
			ereport(ERROR,
					(errcode(ERRCODE_WRONG_OBJECT_TYPE),
					 errmsg("\"%s\" is not a view",
							RelationGetRelationName(rel))));

		/* Also check it's not in use already */
		CheckTableNotInUse(rel, "CREATE OR REPLACE VIEW");

		/*
		 * Due to the namespace visibility rules for temporary objects, we
		 * should only end up replacing a temporary view with another
		 * temporary view, and similarly for permanent views.
		 */
		Assert(relation->relpersistence == rel->rd_rel->relpersistence);

		/*
		 * Create a tuple descriptor to compare against the existing view, and
		 * verify that the old column list is an initial prefix of the new
		 * column list.
		 */
		descriptor = BuildDescForRelation(attrList);
		checkViewColumns(descriptor, rel->rd_att);

		/*
		 * If new attributes have been added, we must add pg_attribute entries
		 * for them.  It is convenient (although overkill) to use the ALTER
		 * TABLE ADD COLUMN infrastructure for this.
		 *
		 * Note that we must do this before updating the query for the view,
		 * since the rules system requires that the correct view columns be in
		 * place when defining the new rules.
		 *
		 * Also note that ALTER TABLE doesn't run parse transformation on
		 * AT_AddColumnToView commands.  The ColumnDef we supply must be ready
		 * to execute as-is.
		 */
		if (list_length(attrList) > rel->rd_att->natts)
		{
			ListCell   *c;
			int			skip = rel->rd_att->natts;

			foreach(c, attrList)
			{
				if (skip > 0)
				{
					skip--;
					continue;
				}
				atcmd = makeNode(AlterTableCmd);
				atcmd->subtype = AT_AddColumnToView;
				atcmd->def = (Node *) lfirst(c);
				atcmds = lappend(atcmds, atcmd);
			}

			/* EventTriggerAlterTableStart called by ProcessUtilitySlow */
			AlterTableInternal(viewOid, atcmds, true);

			/* Make the new view columns visible */
			CommandCounterIncrement();
		}

		/*
		 * Update the query for the view.
		 *
		 * Note that we must do this before updating the view options, because
		 * the new options may not be compatible with the old view query (for
		 * example if we attempt to add the WITH CHECK OPTION, we require that
		 * the new view be automatically updatable, but the old view may not
		 * have been).
		 */
		StoreViewQuery(viewOid, viewParse, replace);

		/* Make the new view query visible before updating FK info */
		CommandCounterIncrement();

		/* Store FK preservation info in pg_class/pg_attribute */
		UpdateViewFKInfo(viewOid, viewParse);

		/* Make FK info visible */
		CommandCounterIncrement();

		/*
		 * Revalidate dependent views that may contain FK joins referencing
		 * this view.  This must happen after UpdateViewFKInfo so that
		 * dependent views see the updated FK metadata.
		 */
		revalidateDependentViews(viewOid);

		/*
		 * Update the view's options.
		 *
		 * The new options list replaces the existing options list, even if
		 * it's empty.
		 */
		atcmd = makeNode(AlterTableCmd);
		atcmd->subtype = AT_ReplaceRelOptions;
		atcmd->def = (Node *) options;
		atcmds = list_make1(atcmd);

		/* EventTriggerAlterTableStart called by ProcessUtilitySlow */
		AlterTableInternal(viewOid, atcmds, true);

		/*
		 * There is very little to do here to update the view's dependencies.
		 * Most view-level dependency relationships, such as those on the
		 * owner, schema, and associated composite type, aren't changing.
		 * Because we don't allow changing type or collation of an existing
		 * view column, those dependencies of the existing columns don't
		 * change either, while the AT_AddColumnToView machinery took care of
		 * adding such dependencies for new view columns.  The dependencies of
		 * the view's query could have changed arbitrarily, but that was dealt
		 * with inside StoreViewQuery.  What remains is only to check that
		 * view replacement is allowed when we're creating an extension.
		 */
		ObjectAddressSet(address, RelationRelationId, viewOid);

		recordDependencyOnCurrentExtension(&address, true);

		/*
		 * Seems okay, so return the OID of the pre-existing view.
		 */
		relation_close(rel, NoLock);	/* keep the lock! */

		return address;
	}
	else
	{
		CreateStmt *createStmt = makeNode(CreateStmt);
		ObjectAddress address;

		/*
		 * Set the parameters for keys/inheritance etc. All of these are
		 * uninteresting for views...
		 */
		createStmt->relation = relation;
		createStmt->tableElts = attrList;
		createStmt->inhRelations = NIL;
		createStmt->constraints = NIL;
		createStmt->options = options;
		createStmt->oncommit = ONCOMMIT_NOOP;
		createStmt->tablespacename = NULL;
		createStmt->if_not_exists = false;

		/*
		 * Create the relation (this will error out if there's an existing
		 * view, so we don't need more code to complain if "replace" is
		 * false).
		 */
		address = DefineRelation(createStmt, RELKIND_VIEW, InvalidOid, NULL,
								 NULL);
		Assert(address.objectId != InvalidOid);

		/* Make the new view relation visible */
		CommandCounterIncrement();

		/* Store the query for the view */
		StoreViewQuery(address.objectId, viewParse, replace);

		/* Make view query visible before updating FK info */
		CommandCounterIncrement();

		/* Store FK preservation info in pg_class/pg_attribute */
		UpdateViewFKInfo(address.objectId, viewParse);

		/* Make FK info visible for subsequent queries in the same transaction */
		CommandCounterIncrement();

		return address;
	}
}

/*
 * Verify that the columns associated with proposed new view definition match
 * the columns of the old view.  This is similar to equalRowTypes(), with code
 * added to generate specific complaints.  Also, we allow the new view to have
 * more columns than the old.
 */
static void
checkViewColumns(TupleDesc newdesc, TupleDesc olddesc)
{
	int			i;

	if (newdesc->natts < olddesc->natts)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
				 errmsg("cannot drop columns from view")));

	for (i = 0; i < olddesc->natts; i++)
	{
		Form_pg_attribute newattr = TupleDescAttr(newdesc, i);
		Form_pg_attribute oldattr = TupleDescAttr(olddesc, i);

		/* XXX msg not right, but we don't support DROP COL on view anyway */
		if (newattr->attisdropped != oldattr->attisdropped)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot drop columns from view")));

		if (strcmp(NameStr(newattr->attname), NameStr(oldattr->attname)) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change name of view column \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							NameStr(newattr->attname)),
					 errhint("Use ALTER VIEW ... RENAME COLUMN ... to change name of view column instead.")));

		/*
		 * We cannot allow type, typmod, or collation to change, since these
		 * properties may be embedded in Vars of other views/rules referencing
		 * this one.  Other column attributes can be ignored.
		 */
		if (newattr->atttypid != oldattr->atttypid ||
			newattr->atttypmod != oldattr->atttypmod)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change data type of view column \"%s\" from %s to %s",
							NameStr(oldattr->attname),
							format_type_with_typemod(oldattr->atttypid,
													 oldattr->atttypmod),
							format_type_with_typemod(newattr->atttypid,
													 newattr->atttypmod))));

		/*
		 * At this point, attcollations should be both valid or both invalid,
		 * so applying get_collation_name unconditionally should be fine.
		 */
		if (newattr->attcollation != oldattr->attcollation)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_TABLE_DEFINITION),
					 errmsg("cannot change collation of view column \"%s\" from \"%s\" to \"%s\"",
							NameStr(oldattr->attname),
							get_collation_name(oldattr->attcollation),
							get_collation_name(newattr->attcollation))));
	}

	/*
	 * We ignore the constraint fields.  The new view desc can't have any
	 * constraints, and the only ones that could be on the old view are
	 * defaults, which we are happy to leave in place.
	 */
}

/*
 * UpdateViewFKInfo
 *		Store FK preservation info from a view's parsed query into the
 *		system catalogs (pg_class.relfkpreserved and pg_attribute columns).
 *
 * This makes FK join metadata available via the relcache so the parser
 * doesn't need to inspect view rewrite rules.
 */
static void
UpdateViewFKInfo(Oid viewOid, Query *viewParse)
{
	HeapTuple	classtup;
	Form_pg_class classform;
	Relation	attrel;
	int			natts;
	ListCell   *lc_rteid;
	ListCell   *lc_attnum;

	/* Update pg_class.relfkpreserved (stores baserelindex, not OID) */
	classtup = SearchSysCacheCopy1(RELOID, ObjectIdGetDatum(viewOid));
	if (!HeapTupleIsValid(classtup))
		elog(ERROR, "cache lookup failed for relation %u", viewOid);
	classform = (Form_pg_class) GETSTRUCT(classtup);
	classform->relfkpreserved = (viewParse->fkPreservedR != NIL) ?
		(int32) ((RTEId *) linitial(viewParse->fkPreservedR))->baserelindex : 0;
	{
		Relation	pg_class_rel = table_open(RelationRelationId, RowExclusiveLock);

		CatalogTupleUpdate(pg_class_rel, &classtup->t_self, classtup);
		table_close(pg_class_rel, RowExclusiveLock);
	}
	heap_freetuple(classtup);

	/* Update pg_attribute FK column mapping */
	natts = list_length(viewParse->fkColBaseRteids);
	if (natts == 0)
		return;

	attrel = table_open(AttributeRelationId, RowExclusiveLock);

	lc_rteid = list_head(viewParse->fkColBaseRteids);
	lc_attnum = list_head(viewParse->fkColBaseAttnums);

	for (int i = 1; i <= natts; i++)
	{
		HeapTuple		atttup;
		Form_pg_attribute attform;
		RTEId		   *col_rteid;

		atttup = SearchSysCacheCopy2(ATTNUM,
									 ObjectIdGetDatum(viewOid),
									 Int16GetDatum(i));
		if (!HeapTupleIsValid(atttup))
			elog(ERROR, "cache lookup failed for attribute %d of relation %u",
				 i, viewOid);
		attform = (Form_pg_attribute) GETSTRUCT(atttup);

		col_rteid = (RTEId *) lfirst(lc_rteid);
		attform->attfkbaserelid = col_rteid ? col_rteid->relid : InvalidOid;
		attform->attfkbaserelindex = col_rteid ? (int32) col_rteid->baserelindex : 0;
		attform->attfkbaseattnum = lfirst_int(lc_attnum);

		CatalogTupleUpdate(attrel, &atttup->t_self, atttup);
		heap_freetuple(atttup);

		lc_rteid = lnext(viewParse->fkColBaseRteids, lc_rteid);
		lc_attnum = lnext(viewParse->fkColBaseAttnums, lc_attnum);
	}

	table_close(attrel, RowExclusiveLock);

	/*
	 * Explicitly invalidate the relcache entry for the view so that the
	 * TupleDesc is rebuilt with the updated FK column mapping attributes.
	 * pg_attribute syscache invalidation alone doesn't trigger a relcache
	 * rebuild for the owning relation.
	 */
	CacheInvalidateRelcacheByRelid(viewOid);
}

static void
DefineViewRules(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * Set up the ON SELECT rule.  Since the query has already been through
	 * parse analysis, we use DefineQueryRewrite() directly.
	 */
	DefineQueryRewrite(pstrdup(ViewSelectRuleName),
					   viewOid,
					   NULL,
					   CMD_SELECT,
					   true,
					   replace,
					   list_make1(viewParse));

	/*
	 * Someday: automatic ON INSERT, etc
	 */
}

/*
 * DefineView
 *		Execute a CREATE VIEW command.
 */
ObjectAddress
DefineView(ViewStmt *stmt, const char *queryString,
		   int stmt_location, int stmt_len)
{
	RawStmt    *rawstmt;
	Query	   *viewParse;
	RangeVar   *view;
	ListCell   *cell;
	bool		check_option;
	ObjectAddress address;
	ObjectAddress temp_object;

	/*
	 * Run parse analysis to convert the raw parse tree to a Query.  Note this
	 * also acquires sufficient locks on the source table(s).
	 */
	rawstmt = makeNode(RawStmt);
	rawstmt->stmt = stmt->query;
	rawstmt->stmt_location = stmt_location;
	rawstmt->stmt_len = stmt_len;

	viewParse = parse_analyze_fixedparams(rawstmt, queryString, NULL, 0, NULL);

	/*
	 * The grammar should ensure that the result is a single SELECT Query.
	 * However, it doesn't forbid SELECT INTO, so we have to check for that.
	 */
	if (!IsA(viewParse, Query))
		elog(ERROR, "unexpected parse analysis result");
	if (viewParse->utilityStmt != NULL &&
		IsA(viewParse->utilityStmt, CreateTableAsStmt))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain SELECT INTO")));
	if (viewParse->commandType != CMD_SELECT)
		elog(ERROR, "unexpected parse analysis result");

	/*
	 * Check for unsupported cases.  These tests are redundant with ones in
	 * DefineQueryRewrite(), but that function will complain about a bogus ON
	 * SELECT rule, and we'd rather the message complain about a view.
	 */
	if (viewParse->hasModifyingCTE)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("views must not contain data-modifying statements in WITH")));

	/*
	 * If the user specified the WITH CHECK OPTION, add it to the list of
	 * reloptions.
	 */
	if (stmt->withCheckOption == LOCAL_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("local"), -1));
	else if (stmt->withCheckOption == CASCADED_CHECK_OPTION)
		stmt->options = lappend(stmt->options,
								makeDefElem("check_option",
											(Node *) makeString("cascaded"), -1));

	/*
	 * Check that the view is auto-updatable if WITH CHECK OPTION was
	 * specified.
	 */
	check_option = false;

	foreach(cell, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(cell);

		if (strcmp(defel->defname, "check_option") == 0)
			check_option = true;
	}

	/*
	 * If the check option is specified, look to see if the view is actually
	 * auto-updatable or not.
	 */
	if (check_option)
	{
		const char *view_updatable_error =
			view_query_is_auto_updatable(viewParse, true);

		if (view_updatable_error)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("WITH CHECK OPTION is supported only on automatically updatable views"),
					 errhint("%s", _(view_updatable_error))));
	}

	/*
	 * If a list of column names was given, run through and insert these into
	 * the actual query tree. - thomas 2000-03-08
	 */
	if (stmt->aliases != NIL)
	{
		ListCell   *alist_item = list_head(stmt->aliases);
		ListCell   *targetList;

		foreach(targetList, viewParse->targetList)
		{
			TargetEntry *te = lfirst_node(TargetEntry, targetList);

			/* junk columns don't get aliases */
			if (te->resjunk)
				continue;
			te->resname = pstrdup(strVal(lfirst(alist_item)));
			alist_item = lnext(stmt->aliases, alist_item);
			if (alist_item == NULL)
				break;			/* done assigning aliases */
		}

		if (alist_item != NULL)
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("CREATE VIEW specifies more column "
							"names than columns")));
	}

	/* Unlogged views are not sensible. */
	if (stmt->view->relpersistence == RELPERSISTENCE_UNLOGGED)
		ereport(ERROR,
				(errcode(ERRCODE_SYNTAX_ERROR),
				 errmsg("views cannot be unlogged because they do not have storage")));

	/*
	 * If the user didn't explicitly ask for a temporary view, check whether
	 * we need one implicitly.  We allow TEMP to be inserted automatically as
	 * long as the CREATE command is consistent with that --- no explicit
	 * schema name.
	 */
	view = copyObject(stmt->view);	/* don't corrupt original command */
	if (view->relpersistence == RELPERSISTENCE_PERMANENT
		&& query_uses_temp_object(viewParse, &temp_object))
	{
		view->relpersistence = RELPERSISTENCE_TEMP;
		ereport(NOTICE,
				(errmsg("view \"%s\" will be a temporary view",
						view->relname),
				 errdetail("It depends on temporary %s.",
						   getObjectDescription(&temp_object, false))));
	}

	/*
	 * Create the view relation
	 *
	 * NOTE: if it already exists and replace is false, the xact will be
	 * aborted.
	 */
	address = DefineVirtualRelation(view, viewParse->targetList,
									stmt->replace, stmt->options, viewParse);

	return address;
}

/*
 * Use the rules system to store the query for the view.
 */
void
StoreViewQuery(Oid viewOid, Query *viewParse, bool replace)
{
	/*
	 * Now create the rules associated with the view.
	 */
	DefineViewRules(viewOid, viewParse, replace);
}

/*
 * revalidateDependentViews
 *		Re-parse and re-analyze views that depend on the given view,
 *		to verify that FK join constraints are still valid after the
 *		view has been replaced.
 *
 * This scans pg_depend for views whose rewrite rules depend on the
 * replaced view.  For each such view, we re-parse its definition
 * using pg_get_viewdef() and re-analyze it.  If the re-analysis
 * raises an error (e.g., an FK join constraint is now violated),
 * we catch it and report a more helpful error message.
 */
static void
revalidateDependentViews(Oid viewOid)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	HTAB	   *seen_views;
	HASHCTL		hash_ctl;

	hash_ctl.keysize = sizeof(Oid);
	hash_ctl.entrysize = sizeof(Oid);
	hash_ctl.hcxt = CurrentMemoryContext;
	seen_views = hash_create("Dependent view tracking", 32,
							 &hash_ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);

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
		Form_pg_depend depform = (Form_pg_depend) GETSTRUCT(tup);
		Oid			dependentViewOid;
		bool		found;
		Relation	rewriteRel;
		ScanKeyData rewritescankey[1];
		SysScanDesc rewritescan;
		HeapTuple	rewriteTuple;
		Form_pg_rewrite ruleform;

		if (depform->deptype != DEPENDENCY_NORMAL)
			continue;

		if (depform->classid != RewriteRelationId)
			continue;

		/* Look up the rewrite rule to find which view it belongs to */
		rewriteRel = table_open(RewriteRelationId, AccessShareLock);

		ScanKeyInit(&rewritescankey[0],
					Anum_pg_rewrite_oid,
					BTEqualStrategyNumber, F_OIDEQ,
					ObjectIdGetDatum(depform->objid));

		rewritescan = systable_beginscan(rewriteRel, RewriteOidIndexId, true,
										 NULL, 1, rewritescankey);

		rewriteTuple = systable_getnext(rewritescan);
		if (!HeapTupleIsValid(rewriteTuple))
		{
			systable_endscan(rewritescan);
			table_close(rewriteRel, AccessShareLock);
			continue;
		}

		ruleform = (Form_pg_rewrite) GETSTRUCT(rewriteTuple);
		dependentViewOid = ruleform->ev_class;

		systable_endscan(rewritescan);
		table_close(rewriteRel, AccessShareLock);

		/* Skip if we've already checked this view */
		(void) hash_search(seen_views, &dependentViewOid, HASH_ENTER, &found);
		if (found)
			continue;

		/*
		 * Re-parse and re-analyze the dependent view's definition.
		 * Make sure the current transaction's changes are visible first.
		 */
		{
			Datum		viewdef;
			char	   *viewdef_str;
			List	   *parsetree_list;
			RawStmt    *parsetree;
			ParseState *pstate;

			CommandCounterIncrement();

			viewdef = DirectFunctionCall1(pg_get_viewdef,
										  ObjectIdGetDatum(dependentViewOid));
			if (DatumGetPointer(viewdef) == NULL)
				continue;

			viewdef_str = text_to_cstring(DatumGetTextPP(viewdef));

			parsetree_list = raw_parser(viewdef_str, RAW_PARSE_DEFAULT);
			if (list_length(parsetree_list) != 1)
				continue;

			parsetree = linitial_node(RawStmt, parsetree_list);

			pstate = make_parsestate(NULL);
			pstate->p_sourcetext = viewdef_str;

			PG_TRY();
			{
				(void) parse_sub_analyze((Node *) parsetree->stmt,
										 pstate, NULL, false, 0);
			}
			PG_CATCH();
			{
				FlushErrorState();
				ereport(ERROR,
						(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
						 errmsg("virtual foreign key constraint violation while re-validating view \"%s.%s\"",
								get_namespace_name(get_rel_namespace(dependentViewOid)),
								get_rel_name(dependentViewOid))));
			}
			PG_END_TRY();

			free_parsestate(pstate);
			pfree(viewdef_str);
		}
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);
	hash_destroy(seen_views);
}
