/*-------------------------------------------------------------------------
 *
 * parse_fk_join.h
 *	  handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_fk_join.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_FK_JOIN_H
#define PARSE_FK_JOIN_H

#include "parser/parse_node.h"

extern Node *transformFkJoinClause(ParseState *pstate,
								   JoinExpr *j,
								   ParseNamespaceItem *l_nsitem,
								   ParseNamespaceItem *r_nsitem,
								   List *l_colnames,
								   List *r_colnames,
								   ParseNamespaceColumn *l_nscolumns,
								   ParseNamespaceColumn *r_nscolumns,
								   List *my_namespace);

extern void validateFkJoinView(Oid viewOid);

#endif							/* PARSE_FK_JOIN_H */
