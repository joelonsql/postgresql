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

/* RTEId set helper functions */
extern bool rteid_list_member(List *list, RTEId *rteid);
extern List *rteid_list_add(List *list, RTEId *rteid);
extern List *rteid_list_remove(List *list, RTEId *rteid);
extern List *rteid_list_union(List *a, List *b);

/* Chain set helper functions */
extern List *chain_set_add(List *chains, RTEId *source, RTEId *target);
extern List *chain_set_union(List *a, List *b);
extern List *chain_set_filter_by_source(List *chains, List *sources);

#endif							/* PARSE_FKJOIN_H */
