/*-------------------------------------------------------------------------
 *
 * fkgraph.c
 *	  handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/parse_fkgraph.c
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
#include "parser/parse_fkgraph.h"
#include "parser/parse_relation.h"
#include "parser/parsetree.h"
#include "rewrite/rewriteHandler.h"
#include "utils/array.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "parser/parse_fkjoin.h"

/*
 * A node in the ForeignKeyGraph. Each RangeTblEntry (RTE) is represented
 * by one FKGraphNode, which may have multiple outgoing edges referencing
 * other nodes, or incoming edges from other nodes referencing this one.
 */
typedef struct FKGraphNode
{
	RangeTblEntry *rte;			/* The associated RangeTblEntry */
	Index		varno;			/* The varno index of this RTE in the query */
	List	   *outedges;		/* List of FKGraphEdge* (edges from this node) */
	List	   *inedges;		/* List of FKGraphEdge* (edges into this node) */
}			FKGraphNode;

/*
 * Represents a foreign key join edge from "from" node to "to" node.
 *
 * non_null_referencing = indicates whether the referencing side's columns
 *                        are guaranteed non-null (true) or nullable (false).
 * outer_join_safe      = indicates whether the underlying join is an outer join
 *                        that preserves rows even when referencing columns are
 *                        NULL or the parent node might be missing.
 */
typedef struct FKGraphEdge
{
	FKGraphNode *from;
	FKGraphNode *to;
	bool		non_null_referencing;
	bool		outer_join_safe;
}			FKGraphEdge;

/*
 * ForeignKeyGraph aggregates nodes and edges discovered in the query's jointree.
 * It also carries along pstate, query, and a parse location for error reporting.
 */
typedef struct ForeignKeyGraph
{
	ParseState *pstate;
	Query	   *query;
	int			location;
	List	   *nodes;			/* List of FKGraphNode* */
	List	   *edges;			/* List of FKGraphEdge* */
}			ForeignKeyGraph;

static FKGraphNode *fkgraph_find_arborescence_root(ForeignKeyGraph *graph);
static bool fkgraph_topological_check(ForeignKeyGraph *graph);
static FKGraphNode *fkgraph_get_or_add_node(ForeignKeyGraph *graph,
											 RangeTblEntry *rte,
											 Index varno);
static FKGraphEdge *fkgraph_add_edge(ForeignKeyGraph *graph,
									  FKGraphNode *from,
									  FKGraphNode *to);
static void fkgraph_build_from_node(ParseState *pstate,
									Query *query,
									ForeignKeyGraph *graph,
									Node *jtnode,
									int location);
static bool fkgraph_varno_in_jointree(List *rtable, Index varno, Node *jointree);
static void fkgraph_verify_dfs(ParseState *pstate, FKGraphNode *node, bool parentMightBeMissing, int location);
static ForeignKeyGraph *create_empty_fkgraph(ParseState *pstate,
											  Query *query,
											  int location);

/*
 * fkgraph_verify
 */
void
fkgraph_verify(ParseState *pstate, Query *query, RangeTblEntry *trunk_rte, int location)
{
	ForeignKeyGraph *graph = create_empty_fkgraph(pstate, query, location);
	FKGraphNode *root_node;
	ListCell   *lc;

	foreach(lc, query->jointree->fromlist)
	{
		Node	   *n = (Node *) lfirst(lc);

		fkgraph_build_from_node(pstate, query, graph, n, location);
	}

	/* Ensure we have exactly one root node with n_nodes - 1 edges, etc. */
	root_node = fkgraph_find_arborescence_root(graph);
	if (root_node == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
					errmsg("derived relation does not form a valid arborescence for foreign key join"),
					parser_errposition(pstate, location)));

	/* Ensure the root node corresponds to the trunk RTE */
	if (root_node->rte != trunk_rte)
		ereport(ERROR,
				(errcode(ERRCODE_INTEGRITY_CONSTRAINT_VIOLATION),
				 errmsg("trunk relation must be the root of the foreign key join arborescence"),
				 parser_errposition(pstate, location)));

	fkgraph_verify_dfs(pstate, root_node, false, location);
}


/*
 * create_empty_fkgraph
 *
 * Create and initialize an empty ForeignKeyGraph for a given parse state
 * and query. We'll fill in its nodes and edges as we walk the jointree.
 */
static ForeignKeyGraph *
create_empty_fkgraph(ParseState *pstate, Query *query, int location)
{
	ForeignKeyGraph *graph = palloc0(sizeof(ForeignKeyGraph));

	graph->pstate = pstate;
	graph->query = query;
	graph->location = location;
	graph->nodes = NIL;
	graph->edges = NIL;
	return graph;
}

/*
 * fkgraph_get_or_add_node
 *
 * Retrieve (or create) the graph node that corresponds to a particular
 * RangeTblEntry and varno. If it doesn't exist yet in graph->nodes,
 * create a new one.
 */
static FKGraphNode *
fkgraph_get_or_add_node(ForeignKeyGraph * graph,
						RangeTblEntry *rte,
						Index varno)
{
	ListCell   *lc;

	foreach(lc, graph->nodes)
	{
		FKGraphNode *node = (FKGraphNode *) lfirst(lc);

		if (node->varno == varno)
			return node;
	}

	/* If not found, create a new node */
	{
		FKGraphNode *newnode = palloc0(sizeof(FKGraphNode));

		newnode->rte = rte;
		newnode->varno = varno;
		newnode->outedges = NIL;
		newnode->inedges = NIL;
		graph->nodes = lappend(graph->nodes, newnode);
		return newnode;
	}
}

/*
 * fkgraph_add_edge
 *
 * Create a new FKGraphEdge from one node to another and link it
 * into both nodes' edge lists.
 */
static FKGraphEdge *
fkgraph_add_edge(ForeignKeyGraph * graph,
				 FKGraphNode * from,
				 FKGraphNode * to)
{
	FKGraphEdge *edge = palloc0(sizeof(FKGraphEdge));

	edge->from = from;
	edge->to = to;

	/*
	 * Defaults. We'll set non_null_referencing after checking the referencing
	 * columns. We'll set outer_join_safe after analyzing the join type.
	 */
	edge->non_null_referencing = true;
	edge->outer_join_safe = false;

	from->outedges = lappend(from->outedges, edge);
	to->inedges = lappend(to->inedges, edge);

	graph->edges = lappend(graph->edges, edge);
	return edge;
}

/*
 * fkgraph_varno_in_jointree
 *
 * Check if the given varno is present in the specified jointree node
 * (RangeTblRef or JoinExpr).
 */
static bool
fkgraph_varno_in_jointree(List *rtable, Index varno, Node *jtnode)
{
	if (jtnode == NULL)
		return false;

	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;

		return (rtr->rtindex == varno);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;

		if (fkgraph_varno_in_jointree(rtable, varno, join->larg))
			return true;
		if (fkgraph_varno_in_jointree(rtable, varno, join->rarg))
			return true;
		return false;
	}

	return false;
}

/*
 * fkgraph_build_from_node
 *
 * Recursively walk a jointree node (RangeTblRef or JoinExpr), creating
 * FKGraphNodes for each RTE and FKGraphEdges for each foreign key join
 * discovered (join->fkJoin).
 *
 * Also checks referencing columns' nullability and sets edge->non_null_referencing
 * accordingly. We detect if the underlying join is left/right vs. referencing
 * side to set edge->outer_join_safe.
 */
static void
fkgraph_build_from_node(ParseState *pstate,
						Query *query,
						ForeignKeyGraph * graph,
						Node *jtnode,
						int location)
{
	if (jtnode == NULL)
		return;

	if (IsA(jtnode, RangeTblRef))
	{
		RangeTblRef *rtr = (RangeTblRef *) jtnode;
		RangeTblEntry *rte = rt_fetch(rtr->rtindex, query->rtable);

		(void) fkgraph_get_or_add_node(graph, rte, rtr->rtindex);
	}
	else if (IsA(jtnode, JoinExpr))
	{
		JoinExpr   *join = (JoinExpr *) jtnode;

		/* Recursively build from sub-nodes */
		fkgraph_build_from_node(pstate, query, graph, join->larg, location);
		fkgraph_build_from_node(pstate, query, graph, join->rarg, location);

		if (join->fkJoin != NULL)
		{
			ForeignKeyJoinNode *fkjn = castNode(ForeignKeyJoinNode, join->fkJoin);
			RangeTblEntry *referencing_rte =
				rt_fetch(fkjn->referencingVarno, query->rtable);
			RangeTblEntry *referenced_rte =
				rt_fetch(fkjn->referencedVarno, query->rtable);

			/* from_node is referencing side, to_node is referenced side */
			FKGraphNode *from_node =
				fkgraph_get_or_add_node(graph, referencing_rte, fkjn->referencingVarno);
			FKGraphNode *to_node =
				fkgraph_get_or_add_node(graph, referenced_rte, fkjn->referencedVarno);

			FKGraphEdge *edge = fkgraph_add_edge(graph, from_node, to_node);

			/*
			 * Determine referencing columns' nullability by drilling down to
			 * the base table and checking attnotnull for each column.
			 */
			{
				List	   *colaliases = NIL;
				List	   *base_colnames = NIL;
				Oid			base_relid;
				ListCell   *lc_attnum;
				bool		all_non_null = true;

				foreach(lc_attnum, fkjn->referencingAttnums)
				{
					int			attnum = lfirst_int(lc_attnum);
					char	   *colname = get_rte_attribute_name(referencing_rte, attnum);

					colaliases = lappend(colaliases, makeString(colname));
				}

				base_relid = drill_down_to_base_rel(pstate,
													referencing_rte,
													&base_colnames,
													colaliases,
													false,	/* is_referenced = false */
													location);

				foreach(lc_attnum, base_colnames)
				{
					char	   *colname = strVal(lfirst(lc_attnum));
					AttrNumber	base_attnum;
					HeapTuple	tuple;
					Form_pg_attribute attr;

					base_attnum = get_attnum(base_relid, colname);
					if (base_attnum == InvalidAttrNumber)
						elog(ERROR, "cache lookup failed for column \"%s\" of relation %u",
							 colname, base_relid);

					tuple = SearchSysCache2(ATTNUM,
											ObjectIdGetDatum(base_relid),
											Int16GetDatum(base_attnum));
					if (!HeapTupleIsValid(tuple))
						elog(ERROR, "cache lookup failed for attribute %d of relation %u",
							 base_attnum, base_relid);

					attr = (Form_pg_attribute) GETSTRUCT(tuple);

					if (!attr->attnotnull)
						all_non_null = false;

					ReleaseSysCache(tuple);
				}

				edge->non_null_referencing = all_non_null;
			}

			/*
			 * Determine if this is an outer-join-safe edge. If it's a LEFT
			 * JOIN with referencing side on the left, or a RIGHT JOIN with
			 * referencing side on the right, we set outer_join_safe = true.
			 * Otherwise false.
			 */
			{
				JoinType	jt = join->jointype;
				bool		referencing_is_left =
					fkgraph_varno_in_jointree(query->rtable,
											  fkjn->referencingVarno,
											  join->larg);

				if ((jt == JOIN_LEFT && referencing_is_left) ||
					(jt == JOIN_RIGHT && !referencing_is_left))
				{
					edge->outer_join_safe = true;
				}
				else
				{
					edge->outer_join_safe = false;
				}
			}
		}
	}
	else
	{
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("foreign key joins involving this type of node are not supported"),
				 parser_errposition(pstate, location)));
	}
}

/*
 * fkgraph_topological_check
 *
 * Ensures the graph is acyclic and can be topologically ordered.
 * We do a standard Kahn's algorithm or BFS approach, counting visited nodes.
 */
static bool
fkgraph_topological_check(ForeignKeyGraph * graph)
{
	int			n_nodes = list_length(graph->nodes);
	List	   *queue = NIL;
	ListCell   *lc;
	int			visited_count = 0;

	typedef struct
	{
		FKGraphNode *node;
		int			in_degree;
	}			NodeInfo;

	NodeInfo   *nodeinfos = palloc(sizeof(NodeInfo) * n_nodes);

	/* Fill in NodeInfo array */
	{
		int			i = 0;

		foreach(lc, graph->nodes)
		{
			FKGraphNode *node = (FKGraphNode *) lfirst(lc);

			nodeinfos[i].node = node;
			nodeinfos[i].in_degree = list_length(node->inedges);
			i++;
		}
	}

	/* Start with all nodes having in_degree=0 */
	for (int j = 0; j < n_nodes; j++)
	{
		if (nodeinfos[j].in_degree == 0)
			queue = lappend(queue, &nodeinfos[j]);
	}

	/* Process queue */
	while (queue != NIL)
	{
		NodeInfo   *current = (NodeInfo *) linitial(queue);

		queue = list_delete_first(queue);
		visited_count++;

		foreach(lc, current->node->outedges)
		{
			FKGraphEdge *edge = (FKGraphEdge *) lfirst(lc);

			/* Decrement in_degree for the 'to' node */
			for (int k = 0; k < n_nodes; k++)
			{
				if (nodeinfos[k].node == edge->to)
				{
					nodeinfos[k].in_degree--;
					if (nodeinfos[k].in_degree == 0)
						queue = lappend(queue, &nodeinfos[k]);
					break;
				}
			}
		}
	}

	pfree(nodeinfos);

	/* If we visited all nodes, it's acyclic; else there's a cycle. */
	return (visited_count == n_nodes);
}

/*
 * fkgraph_find_arborescence_root
 *
 * Attempts to find exactly one root node (a node with no inedges) in the graph,
 * and checks that edges == nodes - 1, and that a topological check passes.
 */
static FKGraphNode *
fkgraph_find_arborescence_root(ForeignKeyGraph * graph)
{
	int			n_nodes = list_length(graph->nodes);
	int			n_edges = list_length(graph->edges);

	if (n_nodes == 0)
		return NULL;

	{
		FKGraphNode *root = NULL;
		int			count_zero_inedges = 0;
		ListCell   *lc;

		foreach(lc, graph->nodes)
		{
			FKGraphNode *node = (FKGraphNode *) lfirst(lc);

			if (node->inedges == NIL)
			{
				count_zero_inedges++;
				if (count_zero_inedges > 1)
					break;
				root = node;
			}
		}

		/* Must have exactly 1 node with no incoming edges */
		if (count_zero_inedges != 1)
			return NULL;

		/* Must have exactly n_nodes - 1 edges in an arborescence */
		if (n_edges != n_nodes - 1)
			return NULL;

		/* Finally, check for no cycles (topological) */
		if (!fkgraph_topological_check(graph))
			return NULL;

		return root;
	}
}

/*
 * fkgraph_verify_dfs
 *
 * Recursively ensure that no foreign key join will filter rows.
 *
 * Rules in this design:
 *   - A child node might be missing if either:
 *       (1) the parent node might be missing, OR
 *       (2) the referencing columns are nullable.
 *   - If the child node might be missing, then the edge must be outer_join_safe,
 *     or we risk filtering out those cases in an inner join.
 */
static void
fkgraph_verify_dfs(ParseState *pstate, FKGraphNode *node, bool parentMightBeMissing, int location)
{
	ListCell   *lc;

	foreach(lc, node->outedges)
	{
		FKGraphEdge *edge = (FKGraphEdge *) lfirst(lc);

		/*
		 * childMightBeMissing becomes true if: - the parent node might be
		 * missing, - or the referencing columns are nullable
		 */
		bool		childMightBeMissing = (parentMightBeMissing ||
										   !edge->non_null_referencing);

		/*
		 * If childMightBeMissing is true, then we must have an outer join;
		 * otherwise we'd filter rows where referencing columns are NULL or
		 * where the parent node is absent due to an earlier outer join.
		 */
		if (childMightBeMissing && !edge->outer_join_safe)
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("foreign key join would filter rows"),
					 errdetail("Referencing columns are nullable or parent node might be missing, "
							   "but the join is not outer join safe."),
					 parser_errposition(pstate, location)));

		/* Recurse on the child, passing along whether it might be missing */
		fkgraph_verify_dfs(pstate, edge->to, childMightBeMissing, location);
	}
}
