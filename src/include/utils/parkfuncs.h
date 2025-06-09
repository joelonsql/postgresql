/*-------------------------------------------------------------------------
 *
 * parkfuncs.h
 *	  Functions for backend parking support
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/parkfuncs.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARKFUNCS_H
#define PARKFUNCS_H

#include "fmgr.h"

/* Functions for backend parking */
extern Datum pg_park(PG_FUNCTION_ARGS);
