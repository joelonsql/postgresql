/*-------------------------------------------------------------------------
 *
 * keyjoincmds.c
 *	  Commands for manipulating stored key-join proofs.
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/keyjoincmds.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/htup_details.h"
#include "access/relation.h"
#include "access/skey.h"
#include "access/table.h"
#include "catalog/indexing.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_rewrite.h"
#include "commands/keyjoin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "parser/parse_key_join.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	get_rule_event_relation(Oid ruleOid);
static List *find_dependent_key_join_objects(Oid refclassid, Oid refobjid);
static void revalidate_dependent_key_join_objects_recurse(Oid refclassid,
														  Oid refobjid,
														  List *ancestors);
static bool object_address_list_member(List *objects, Oid classId,
									   Oid objectId);
static ObjectAddress *make_object_address(Oid classId, Oid objectId);
static void revalidate_dependent_key_join_relation(Oid relationOid);
static void revalidate_dependent_key_join_function(Oid procOid);
static void revalidate_dependent_key_join_policy(Oid policy_id);
static void revalidate_stored_key_join_node(Node *stored, bool stored_is_query);
static Oid	get_policy_relid(Oid policy_id);
static Node *policy_string_to_node(HeapTuple policy_tuple,
								   TupleDesc policy_desc, AttrNumber attnum);

static Oid
get_rule_event_relation(Oid ruleOid)
{
	Relation	rewriteRel;
	ScanKeyData key[1];
	SysScanDesc scan;
	HeapTuple	tup;
	Oid			result = InvalidOid;

	rewriteRel = table_open(RewriteRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_rewrite_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(ruleOid));
	scan = systable_beginscan(rewriteRel, RewriteOidIndexId, true,
							  NULL, 1, key);
	tup = systable_getnext(scan);
#ifdef USE_ASSERT_CHECKING
	Assert(HeapTupleIsValid(tup));
#else
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for rule %u", ruleOid);
#endif
	result = ((Form_pg_rewrite) GETSTRUCT(tup))->ev_class;
	Assert(OidIsValid(result));
	systable_endscan(scan);
	table_close(rewriteRel, AccessShareLock);

	return result;
}

static List *
find_dependent_key_join_objects(Oid refclassid, Oid refobjid)
{
	Relation	depRel;
	ScanKeyData key[2];
	SysScanDesc scan;
	HeapTuple	tup;
	List	   *result = NIL;

	depRel = table_open(DependRelationId, AccessShareLock);
	ScanKeyInit(&key[0],
				Anum_pg_depend_refclassid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refclassid));
	ScanKeyInit(&key[1],
				Anum_pg_depend_refobjid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(refobjid));
	scan = systable_beginscan(depRel, DependReferenceIndexId, true,
							  NULL, 2, key);

	while (HeapTupleIsValid((tup = systable_getnext(scan))))
	{
		Form_pg_depend dep = (Form_pg_depend) GETSTRUCT(tup);
		Oid			classid = InvalidOid;
		Oid			objectid = InvalidOid;

		if (dep->classid == RewriteRelationId)
		{
			objectid = get_rule_event_relation(dep->objid);
			classid = RelationRelationId;
		}
		else if (dep->classid == ProcedureRelationId)
		{
			classid = ProcedureRelationId;
			objectid = dep->objid;
		}
		else if (dep->classid == PolicyRelationId)
		{
			classid = PolicyRelationId;
			objectid = dep->objid;
		}
		else
			continue;

		if (classid == refclassid && objectid == refobjid)
			continue;
		if (!object_address_list_member(result, classid, objectid))
			result = lappend(result, make_object_address(classid, objectid));
	}

	systable_endscan(scan);
	table_close(depRel, AccessShareLock);

	return result;
}

static void
revalidate_dependent_key_join_objects_recurse(Oid refclassid, Oid refobjid,
											  List *ancestors)
{
	List	   *dependents;
	List	   *path = list_copy(ancestors);
	ListCell   *lc;

	Assert(!object_address_list_member(ancestors, refclassid, refobjid));
	path = lappend(path, make_object_address(refclassid, refobjid));

	dependents = find_dependent_key_join_objects(refclassid, refobjid);
	foreach(lc, dependents)
	{
		ObjectAddress *depobj = (ObjectAddress *) lfirst(lc);

		if (object_address_list_member(path, depobj->classId,
									   depobj->objectId))
			continue;

		switch (depobj->classId)
		{
			case RelationRelationId:
				revalidate_dependent_key_join_relation(depobj->objectId);
				revalidate_dependent_key_join_objects_recurse(RelationRelationId,
															  depobj->objectId,
															  path);
				break;
			case ProcedureRelationId:
				revalidate_dependent_key_join_function(depobj->objectId);
				revalidate_dependent_key_join_objects_recurse(ProcedureRelationId,
															  depobj->objectId,
															  path);
				break;
			default:
				Assert(depobj->classId == PolicyRelationId);
				revalidate_dependent_key_join_policy(depobj->objectId);
				revalidate_dependent_key_join_objects_recurse(PolicyRelationId,
															  depobj->objectId,
															  path);
				break;
		}
	}
}

static void
revalidate_dependent_key_join_relation(Oid relationOid)
{
	Relation	rel;

	rel = relation_open(relationOid, AccessShareLock);

	/*
	 * Walk every rule attached to the dependent relation.  A stored key-join
	 * proof can live in any rule action: a view's or matview's _RETURN rule,
	 * an INSTEAD-OF rule on a view, a DO ALSO/INSTEAD rule on a plain table,
	 * etc.  Each key-join-bearing action must be revalidated so DDL that
	 * would make the proof unprovable is rejected with the existing "key join
	 * cannot be proven from available constraints" error.
	 *
	 * Revalidation may change copied KeyJoinNode dependency lists.
	 * Dependency shrinkage is safe to leave stale in pg_depend, but new
	 * dependencies or other semantic changes would make the stored proof
	 * unsafe without rewriting its owning object.
	 */
	Assert(rel->rd_rules != NULL);
	for (int i = 0; i < rel->rd_rules->numLocks; i++)
	{
		RewriteRule *rule = rel->rd_rules->rules[i];
		ListCell   *lc;

		foreach(lc, rule->actions)
		{
			Node	   *action = (Node *) lfirst(lc);

			if (IsA(action, Query))
				revalidate_stored_key_join_node(action, true);
		}

		revalidate_stored_key_join_node(rule->qual, false);
	}

	relation_close(rel, AccessShareLock);
}

static void
revalidate_dependent_key_join_function(Oid procOid)
{
	HeapTuple	tup;
	Datum		datum;
	bool		isnull;
	Node	   *body;

	tup = SearchSysCache1(PROCOID, ObjectIdGetDatum(procOid));
#ifdef USE_ASSERT_CHECKING
	Assert(HeapTupleIsValid(tup));
#else
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for function %u", procOid);
#endif

	datum = SysCacheGetAttr(PROCOID, tup, Anum_pg_proc_prosqlbody, &isnull);
	if (isnull)
	{
		ReleaseSysCache(tup);
		return;
	}

	body = stringToNode(TextDatumGetCString(datum));
	ReleaseSysCache(tup);

	revalidate_stored_key_join_node(body, false);
}

static void
revalidate_dependent_key_join_policy(Oid policy_id)
{
	Relation	pg_policy_rel;
	Oid			table_id;
	ScanKeyData skey[1];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	TupleDesc	policy_desc;
	Node	   *qual;
	Node	   *with_check_qual;

	table_id = get_policy_relid(policy_id);
	LockRelationOid(table_id, AccessExclusiveLock);

	pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true, NULL,
							   1, skey);
	policy_tuple = systable_getnext(sscan);

#ifdef USE_ASSERT_CHECKING
	Assert(HeapTupleIsValid(policy_tuple));
#else
	if (!HeapTupleIsValid(policy_tuple))
		elog(ERROR, "cache lookup failed for policy %u", policy_id);
#endif

	policy_desc = RelationGetDescr(pg_policy_rel);
	qual = policy_string_to_node(policy_tuple, policy_desc,
								 Anum_pg_policy_polqual);
	with_check_qual = policy_string_to_node(policy_tuple, policy_desc,
											Anum_pg_policy_polwithcheck);

	revalidate_stored_key_join_node(qual, false);
	revalidate_stored_key_join_node(with_check_qual, false);

	systable_endscan(sscan);
	table_close(pg_policy_rel, AccessShareLock);
}

static void
revalidate_stored_key_join_node(Node *stored, bool stored_is_query)
{
	Node	   *copy;

	if (stored == NULL || !storedNodeContainsKeyJoin(stored))
		return;

	copy = copyObject(stored);
	if (stored_is_query)
		revalidateStoredKeyJoinProofsInQuery(castNode(Query, copy));
	else
		revalidateStoredKeyJoinProofsInNode(copy);

	if (!revalidatedStoredKeyJoinProofsAreSafe(stored, copy))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FOREIGN_KEY),
				 errmsg("stored key join proof would require new dependencies")));
}

void
RevalidateDependentKeyJoinObjectsOnConstraint(Oid constraintOid)
{
	Assert(OidIsValid(constraintOid));

	revalidate_dependent_key_join_objects_recurse(ConstraintRelationId,
												  constraintOid, NIL);
}

void
RevalidateDependentKeyJoinObjectsOnRelation(Oid relationOid)
{
	Assert(OidIsValid(relationOid));

	revalidate_dependent_key_join_objects_recurse(RelationRelationId,
												  relationOid, NIL);
}

void
RevalidateDependentKeyJoinObjectsOnProcedure(Oid procOid)
{
	Assert(OidIsValid(procOid));

	revalidate_dependent_key_join_objects_recurse(ProcedureRelationId,
												  procOid, NIL);
}

static bool
object_address_list_member(List *objects, Oid classId, Oid objectId)
{
	foreach_ptr(ObjectAddress, object, objects)
	{
		Assert(object->objectSubId == 0);

		if (object->classId == classId &&
			object->objectId == objectId)
			return true;
	}
	return false;
}

static ObjectAddress *
make_object_address(Oid classId, Oid objectId)
{
	ObjectAddress *object = palloc(sizeof(ObjectAddress));

	ObjectAddressSet(*object, classId, objectId);
	return object;
}

static Oid
get_policy_relid(Oid policy_id)
{
	Relation	pg_policy_rel;
	ScanKeyData skey[1];
	SysScanDesc sscan;
	HeapTuple	policy_tuple;
	Oid			table_id;

	pg_policy_rel = table_open(PolicyRelationId, AccessShareLock);

	ScanKeyInit(&skey[0],
				Anum_pg_policy_oid,
				BTEqualStrategyNumber, F_OIDEQ,
				ObjectIdGetDatum(policy_id));

	sscan = systable_beginscan(pg_policy_rel, PolicyOidIndexId, true, NULL,
							   1, skey);
	policy_tuple = systable_getnext(sscan);

#ifdef USE_ASSERT_CHECKING
	Assert(HeapTupleIsValid(policy_tuple));
#else
	if (!HeapTupleIsValid(policy_tuple))
		elog(ERROR, "cache lookup failed for policy %u", policy_id);
#endif

	table_id = ((Form_pg_policy) GETSTRUCT(policy_tuple))->polrelid;

	systable_endscan(sscan);
	table_close(pg_policy_rel, AccessShareLock);

	return table_id;
}

static Node *
policy_string_to_node(HeapTuple policy_tuple, TupleDesc policy_desc,
					  AttrNumber attnum)
{
	Datum		expr_datum;
	bool		expr_isnull;

	expr_datum = heap_getattr(policy_tuple, attnum, policy_desc,
							  &expr_isnull);
	if (expr_isnull)
		return NULL;

	return stringToNode(TextDatumGetCString(expr_datum));
}
