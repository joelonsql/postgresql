/*-------------------------------------------------------------------------
 *
 * parse_fkjoin.h
 *	  Handle foreign key joins in parser
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/parse_fkjoin.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_FKJOIN_H
#define PARSE_FKJOIN_H

#include "parser/parse_node.h"

extern void transformAndValidateForeignKeyJoin(ParseState *pstate, JoinExpr *j, ParseNamespaceItem *r_nsitem, List *l_namespace);

#endif							/* PARSE_FKJOIN_H */
