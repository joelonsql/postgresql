/*-------------------------------------------------------------------------
 *
 * asyncdesc.c
 *	  rmgr descriptor routines for access/transam/async.c
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/rmgrdesc/asyncdesc.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/async_xlog.h"

void
async_desc(StringInfo buf, XLogReaderState *record)
{
	char	   *rec = XLogRecGetData(record);
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	if (info == XLOG_ASYNC_NOTIFY_DATA)
	{
		xl_async_notify_data *xlrec = (xl_async_notify_data *) rec;

		appendStringInfo(buf, "notify data: db %u xid %u pid %d notifications %u",
						 xlrec->dbid, xlrec->xid, xlrec->srcPid, xlrec->nnotifications);
	}
	else if (info == XLOG_ASYNC_NOTIFY_COMMIT)
	{
		xl_async_notify_commit *xlrec = (xl_async_notify_commit *) rec;

		appendStringInfo(buf, "notify commit: db %u xid %u notify_lsn %X/%X",
						 xlrec->dbid, xlrec->xid,
						 LSN_FORMAT_ARGS(xlrec->notify_lsn));
	}
}

const char *
async_identify(uint8 info)
{
	const char *id = NULL;

	switch (info & ~XLR_INFO_MASK)
	{
		case XLOG_ASYNC_NOTIFY_DATA:
			id = "NOTIFY_DATA";
			break;
		case XLOG_ASYNC_NOTIFY_COMMIT:
			id = "NOTIFY_COMMIT";
			break;
	}

	return id;
}