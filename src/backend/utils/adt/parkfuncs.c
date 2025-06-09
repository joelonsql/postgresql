/*-------------------------------------------------------------------------
 *
 * parkfuncs.c
 *	  Functions for backend parking support
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/adt/parkfuncs.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "storage/procarray.h"
#include "utils/builtins.h"

/*
 * pg_park
 *		Park the current backend if parking is enabled
 *
 * Returns true if successfully parked, false if parking was disabled
 * or vetoed by an extension.
 */
Datum
pg_park(PG_FUNCTION_ARGS)
{
	if (!enable_parking)
		PG_RETURN_BOOL(false);

	if (IsTransactionState())
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("cannot park while a transaction is active")));

	if (!ParkMyBackend())
		PG_RETURN_BOOL(false);		/* vetoed */

	PG_RETURN_BOOL(true);
}