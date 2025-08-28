/*-------------------------------------------------------------------------
 *
 * async.h
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/async.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ASYNC_H
#define ASYNC_H

#include <signal.h>
#include "access/xlogreader.h"

extern PGDLLIMPORT bool Trace_notify;
extern PGDLLIMPORT int max_notify_queue_pages;
extern PGDLLIMPORT volatile sig_atomic_t notifyInterruptPending;

/*
 * Compact SLRU queue entry - stores metadata pointing to WAL data
 */
typedef struct AsyncQueueEntry
{
	Oid			dbid;			/* database ID for quick filtering */
	TransactionId	xid;			/* transaction ID */
	XLogRecPtr	notify_lsn;		/* LSN of notification data in WAL */
} AsyncQueueEntry;

#define ASYNC_QUEUE_ENTRY_SIZE	sizeof(AsyncQueueEntry)

extern Size AsyncShmemSize(void);
extern void AsyncShmemInit(void);

extern void NotifyMyFrontEnd(const char *channel,
							 const char *payload,
							 int32 srcPid);

/* notify-related SQL statements */
extern void Async_Notify(const char *channel, const char *payload);
extern void Async_Listen(const char *channel);
extern void Async_Unlisten(const char *channel);
extern void Async_UnlistenAll(void);

/* perform (or cancel) outbound notify processing at transaction commit */
extern void PreCommit_Notify(void);
extern void AtCommit_Notify(void);
extern void AtAbort_Notify(void);
extern void AtSubCommit_Notify(void);
extern void AtSubAbort_Notify(void);
extern void AtPrepare_Notify(void);

/* signal handler for inbound notifies (PROCSIG_NOTIFY_INTERRUPT) */
extern void HandleNotifyInterrupt(void);

/* process interrupts */
extern void ProcessNotifyInterrupt(bool flush);

/* WAL-based notification functions */
extern XLogRecPtr LogAsyncNotifyData(Oid dboid, TransactionId xid, int32 srcPid,
									 uint32 nnotifications, Size data_len, char *data);
extern XLogRecPtr LogAsyncNotifyCommit(Oid dboid, TransactionId xid, XLogRecPtr notify_lsn);
extern void async_redo(XLogReaderState *record);

#endif							/* ASYNC_H */
