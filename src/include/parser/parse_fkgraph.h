/*-------------------------------------------------------------------------
 *
 * fkgraph.h
 *	  Handle graph traversal foreign key joins
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_fkgraph.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_FKGRAPH_H
#define PARSE_FKGRAPH_H

#include "parser/parse_node.h"

extern void fkgraph_verify(ParseState *pstate, Query *query, RangeTblEntry *trunk_rte, int location);

#endif							/* PARSE_FKGRAPH_H */
