/*-------------------------------------------------------------------------
 *
 * async_xlog.h
 *	  Async notification WAL definitions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/async_xlog.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_XLOG_H
#define ASYNC_XLOG_H

#include "access/xlogdefs.h"
#include "access/xlogreader.h"
#include "lib/stringinfo.h"

/*
 * WAL record types for async notifications
 */
#define XLOG_ASYNC_NOTIFY_DATA	0x00	/* notification data */
#define XLOG_ASYNC_NOTIFY_COMMIT	0x10	/* commit with notify reference */

/*
 * WAL record for notification data (written in PreCommit_Notify)
 */
typedef struct xl_async_notify_data
{
	Oid			dbid;			/* database ID */
	TransactionId xid;			/* transaction ID */
	int32		srcPid;			/* source backend PID */
	uint32		nnotifications;	/* number of notifications */
	/* followed by serialized notification data */
} xl_async_notify_data;

#define SizeOfAsyncNotifyData	(offsetof(xl_async_notify_data, nnotifications) + sizeof(uint32))

/*
 * WAL record for commit with notification reference
 */
typedef struct xl_async_notify_commit
{
	Oid			dbid;			/* database ID */
	TransactionId xid;			/* transaction ID */
	XLogRecPtr	notify_lsn;		/* LSN of corresponding notify data record */
} xl_async_notify_commit;

#define SizeOfAsyncNotifyCommit	(offsetof(xl_async_notify_commit, notify_lsn) + sizeof(XLogRecPtr))

extern void async_redo(XLogReaderState *record);
extern void async_desc(StringInfo buf, XLogReaderState *record);
extern const char *async_identify(uint8 info);

#endif							/* ASYNC_XLOG_H */