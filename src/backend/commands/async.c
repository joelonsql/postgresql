/*-------------------------------------------------------------------------
 *
 * async.c
 *	  Asynchronous notification: NOTIFY, LISTEN, UNLISTEN
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/commands/async.c
 *
 *-------------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 * Async Notification Model as of 9.0:
 *
 * 1. Multiple backends on same machine. Multiple backends listening on
 *	  several channels. (Channels are also called "conditions" in other
 *	  parts of the code.)
 *
 * 2. There is one central queue in disk-based storage (directory pg_notify/),
 *	  with actively-used pages mapped into shared memory by the slru.c module.
 *	  All notification messages are placed in the queue and later read out
 *	  by listening backends.
 *
 *	  We also maintain a dynamic shared hash table (dshash) that maps channel
 *	  names to the set of backends listening on each channel. This table is
 *	  created lazily on the first LISTEN command and grows dynamically as
 *	  needed.
 *
 *	  Although there is only one queue, notifications are treated as being
 *	  database-local; this is done by including the sender's database OID
 *	  in each notification message.  Listening backends ignore messages
 *	  that don't match their database OID.  This is important because it
 *	  ensures senders and receivers have the same database encoding and won't
 *	  misinterpret non-ASCII text in the channel name or payload string.
 *
 *	  Since notifications are not expected to survive database crashes,
 *	  we can simply clean out the pg_notify data at any reboot, and there
 *	  is no need for WAL support or fsync'ing.
 *
 * 3. Every backend that is listening on at least one channel registers by
 *	  entering its PID into the array in AsyncQueueControl. It then scans all
 *	  incoming notifications in the central queue and first compares the
 *	  database OID of the notification with its own database OID and then
 *	  compares the notified channel with the list of channels that it listens
 *	  to. In case there is a match it delivers the notification event to its
 *	  frontend.  Non-matching events are simply skipped.
 *
 * 4. The NOTIFY statement (routine Async_Notify) stores the notification in
 *	  a backend-local list which will not be processed until transaction end.
 *
 *	  Duplicate notifications from the same transaction are sent out as one
 *	  notification only. This is done to save work when for example a trigger
 *	  on a 2 million row table fires a notification for each row that has been
 *	  changed. If the application needs to receive every single notification
 *	  that has been sent, it can easily add some unique string into the extra
 *	  payload parameter.
 *
 *	  When the transaction is ready to commit, PreCommit_Notify() adds the
 *	  pending notifications to the head of the queue. The head pointer of the
 *	  queue always points to the next free position and a position is just a
 *	  page number and the offset in that page. This is done before marking the
 *	  transaction as committed in clog. If we run into problems writing the
 *	  notifications, we can still call elog(ERROR, ...) and the transaction
 *	  will roll back.
 *
 *	  Once we have put all of the notifications into the queue, we return to
 *	  CommitTransaction() which will then do the actual transaction commit.
 *
 *	  After commit we are called another time (AtCommit_Notify()). Here we
 *	  make any actual updates to the local listen state (listenChannelsHash) and
 *	  shared channel hash table (channelHash).  Then we signal any backends
 *	  that may be interested in our messages (including our own backend,
 *	  if listening).  This is done by SignalBackends(), which consults the
 *	  shared channel hash table to identify listeners for the channels that
 *	  have pending notifications in the current database.  Each selected
 *	  backend is marked as having a wakeup pending to avoid duplicate signals,
 *	  and a PROCSIG_NOTIFY_INTERRUPT signal is sent to it.
 *
 *	  When writing notifications, PreCommit_Notify() records the queue head
 *	  position both before and after the write.  Because all writers serialize
 *	  on a cluster-wide heavyweight lock, no backend can insert entries between
 *	  these two points.  SignalBackends() uses this fact to directly advance any
 *	  backend that is still positioned at the old head, avoiding unnecessary
 *	  wakeups for idle listeners that have nothing to read.
 *
 *	  Finally, after we are out of the transaction altogether and about to go
 *	  idle, we scan the queue for messages that need to be sent to our
 *	  frontend (which might be notifies from other backends, or self-notifies
 *	  from our own).  This step is not part of the CommitTransaction sequence
 *	  for two important reasons.  First, we could get errors while sending
 *	  data to our frontend, and it's really bad for errors to happen in
 *	  post-commit cleanup.  Second, in cases where a procedure issues commits
 *	  within a single frontend command, we don't want to send notifies to our
 *	  frontend until the command is done; but notifies to other backends
 *	  should go out immediately after each commit.
 *
 * 5. Upon receipt of a PROCSIG_NOTIFY_INTERRUPT signal, the signal handler
 *	  sets the process's latch, which triggers the event to be processed
 *	  immediately if this backend is idle (i.e., it is waiting for a frontend
 *	  command and is not within a transaction block. C.f.
 *	  ProcessClientReadInterrupt()).  Otherwise the handler may only set a
 *	  flag, which will cause the processing to occur just before we next go
 *	  idle.
 *
 *	  Inbound-notify processing consists of reading all of the notifications
 *	  that have arrived since scanning last time. We read every notification
 *	  until we reach either a notification from an uncommitted transaction or
 *	  the head pointer's position.
 *
 * 6. To limit disk space consumption, the tail pointer needs to be advanced
 *	  so that old pages can be truncated. This is relatively expensive
 *	  (notably, it requires an exclusive lock), so we don't want to do it
 *	  often. We make sending backends do this work if they advanced the queue
 *	  head into a new page, but only once every QUEUE_CLEANUP_DELAY pages.
 *
 * An application that listens on the same channel it notifies will get
 * NOTIFY messages for its own NOTIFYs.  These can be ignored, if not useful,
 * by comparing be_pid in the NOTIFY message to the application's own backend's
 * PID.  (As of FE/BE protocol 2.0, the backend's PID is provided to the
 * frontend during startup.)  The above design guarantees that notifies from
 * other backends will never be missed by ignoring self-notifies.
 *
 * The amount of shared memory used for notify management (notify_buffers)
 * can be varied without affecting anything but performance.  The maximum
 * amount of notification data that can be queued at one time is determined
 * by max_notify_queue_pages GUC.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include <limits.h>
#include <unistd.h>
#include <signal.h>
#include <string.h>

#include "access/parallel.h"
#include "access/slru.h"
#include "access/transam.h"
#include "access/xact.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "common/hashfn.h"
#include "funcapi.h"
#include "lib/dshash.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "storage/dsm_registry.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/procsignal.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/dsa.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"


/*
 * Maximum size of a NOTIFY payload, including terminating NULL.  This
 * must be kept small enough so that a notification message fits on one
 * SLRU page.  The magic fudge factor here is noncritical as long as it's
 * more than AsyncQueueEntryEmptySize --- we make it significantly bigger
 * than that, so changes in that data structure won't affect user-visible
 * restrictions.
 */
#define NOTIFY_PAYLOAD_MAX_LENGTH	(BLCKSZ - NAMEDATALEN - 128)

/*
 * Channel hash table definitions
 *
 * This hash table maps (database OID, channel name) keys to arrays of
 * ProcNumbers representing the backends listening on each channel.
 */

#define INITIAL_LISTENERS_ARRAY_SIZE 4

typedef struct ChannelHashKey
{
	Oid			dboid;
	char		channel[NAMEDATALEN];
} ChannelHashKey;

typedef struct ChannelEntry
{
	ChannelHashKey key;
	dsa_pointer listenersArray; /* DSA pointer to ProcNumber array */
	int			numListeners;	/* Number of listeners currently stored */
	int			allocatedListeners; /* Allocated size of array */
} ChannelEntry;

/*
 * Struct representing an entry in the global notify queue
 *
 * This struct declaration has the maximal length, but in a real queue entry
 * the data area is only big enough for the actual channel and payload strings
 * (each null-terminated).  AsyncQueueEntryEmptySize is the minimum possible
 * entry size, if both channel and payload strings are empty (but note it
 * doesn't include alignment padding).
 *
 * The "length" field should always be rounded up to the next QUEUEALIGN
 * multiple so that all fields are properly aligned.
 */
typedef struct AsyncQueueEntry
{
	int			length;			/* total allocated length of entry */
	Oid			dboid;			/* sender's database OID */
	TransactionId xid;			/* sender's XID */
	int32		srcPid;			/* sender's PID */
	char		data[NAMEDATALEN + NOTIFY_PAYLOAD_MAX_LENGTH];
} AsyncQueueEntry;

/* Currently, no field of AsyncQueueEntry requires more than int alignment */
#define QUEUEALIGN(len)		INTALIGN(len)

#define AsyncQueueEntryEmptySize	(offsetof(AsyncQueueEntry, data) + 2)

/*
 * Struct describing a queue position, and assorted macros for working with it
 */
typedef struct QueuePosition
{
	int64		page;			/* SLRU page number */
	int			offset;			/* byte offset within page */
} QueuePosition;

#define QUEUE_POS_PAGE(x)		((x).page)
#define QUEUE_POS_OFFSET(x)		((x).offset)

#define SET_QUEUE_POS(x,y,z) \
	do { \
		(x).page = (y); \
		(x).offset = (z); \
	} while (0)

#define QUEUE_POS_EQUAL(x,y) \
	((x).page == (y).page && (x).offset == (y).offset)

#define QUEUE_POS_IS_ZERO(x) \
	((x).page == 0 && (x).offset == 0)

/* choose logically smaller QueuePosition */
#define QUEUE_POS_MIN(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (x) : \
	 (x).page != (y).page ? (y) : \
	 (x).offset < (y).offset ? (x) : (y))

/* choose logically larger QueuePosition */
#define QUEUE_POS_MAX(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) ? (y) : \
	 (x).page != (y).page ? (x) : \
	 (x).offset > (y).offset ? (x) : (y))

/* returns true if x comes before y in queue order */
#define QUEUE_POS_PRECEDES(x,y) \
	(asyncQueuePagePrecedes((x).page, (y).page) || \
	 ((x).page == (y).page && (x).offset < (y).offset))

/*
 * Parameter determining how often we try to advance the tail pointer:
 * we do that after every QUEUE_CLEANUP_DELAY pages of NOTIFY data.  This is
 * also the distance by which a backend needs to be behind before we'll
 * decide we need to wake it up to advance its pointer.
 *
 * Resist the temptation to make this really large.  While that would save
 * work in some places, it would add cost in others.  In particular, this
 * should likely be less than notify_buffers, to ensure that backends
 * catch up before the pages they'll need to read fall out of SLRU cache.
 */
#define QUEUE_CLEANUP_DELAY 4

/*
 * Struct describing a listening backend's status
 */
typedef struct QueueBackendStatus
{
	int32		pid;			/* either a PID or InvalidPid */
	Oid			dboid;			/* backend's database OID, or InvalidOid */
	ProcNumber	nextListener;	/* id of next listener, or INVALID_PROC_NUMBER */
	QueuePosition pos;			/* backend has read queue up to here */
	bool		wakeupPending;	/* signal sent but not yet processed */
	bool		advancingPos;	/* backend is reading the queue */
} QueueBackendStatus;

/*
 * Shared memory state for LISTEN/NOTIFY (excluding its SLRU stuff)
 *
 * The AsyncQueueControl structure is protected by the NotifyQueueLock and
 * NotifyQueueTailLock.
 *
 * When holding NotifyQueueLock in SHARED mode, backends may only inspect
 * their own entries as well as the head and tail pointers. Consequently we
 * can allow a backend to update its own record while holding only SHARED lock
 * (since no other backend will inspect it).
 *
 * When holding NotifyQueueLock in EXCLUSIVE mode, backends can inspect the
 * entries of other backends and also change the head pointer. They can
 * also advance other backends' queue positions, unless they are not
 * in the process of doing that themselves. When holding both NotifyQueueLock and
 * NotifyQueueTailLock in EXCLUSIVE mode, backends can change the tail pointers.
 *
 * SLRU buffer pool is divided in banks and bank wise SLRU lock is used as
 * the control lock for the pg_notify SLRU buffers.
 * In order to avoid deadlocks, whenever we need multiple locks, we first get
 * NotifyQueueTailLock, then NotifyQueueLock, and lastly SLRU bank lock.
 *
 * Each backend uses the backend[] array entry with index equal to its
 * ProcNumber.  We rely on this to make SendProcSignal fast.
 *
 * The backend[] array entries for actively-listening backends are threaded
 * together using firstListener and the nextListener links, so that we can
 * scan them without having to iterate over inactive entries.  We keep this
 * list in order by ProcNumber so that the scan is cache-friendly when there
 * are many active entries.
 */
typedef struct AsyncQueueControl
{
	QueuePosition head;			/* head points to the next free location */
	QueuePosition tail;			/* tail must be <= the queue position of every
								 * listening backend */
	int64		stopPage;		/* oldest unrecycled page; must be <=
								 * tail.page */
	ProcNumber	firstListener;	/* id of first listener, or
								 * INVALID_PROC_NUMBER */
	TimestampTz lastQueueFillWarn;	/* time of last queue-full msg */
	dsa_handle	channelHashDSA;
	dshash_table_handle channelHashDSH;
	QueueBackendStatus backend[FLEXIBLE_ARRAY_MEMBER];
} AsyncQueueControl;

static AsyncQueueControl *asyncQueueControl;

static dsa_area *channelDSA = NULL;
static dshash_table *channelHash = NULL;

#define QUEUE_HEAD					(asyncQueueControl->head)
#define QUEUE_TAIL					(asyncQueueControl->tail)
#define QUEUE_STOP_PAGE				(asyncQueueControl->stopPage)
#define QUEUE_FIRST_LISTENER		(asyncQueueControl->firstListener)
#define QUEUE_BACKEND_PID(i)		(asyncQueueControl->backend[i].pid)
#define QUEUE_BACKEND_DBOID(i)		(asyncQueueControl->backend[i].dboid)
#define QUEUE_NEXT_LISTENER(i)		(asyncQueueControl->backend[i].nextListener)
#define QUEUE_BACKEND_POS(i)		(asyncQueueControl->backend[i].pos)
#define QUEUE_BACKEND_WAKEUP_PENDING(i)	(asyncQueueControl->backend[i].wakeupPending)
#define QUEUE_BACKEND_ADVANCING_POS(i)	(asyncQueueControl->backend[i].advancingPos)

/*
 * The SLRU buffer area through which we access the notification queue
 */
static SlruCtlData NotifyCtlData;

#define NotifyCtl					(&NotifyCtlData)
#define QUEUE_PAGESIZE				BLCKSZ

#define QUEUE_FULL_WARN_INTERVAL	5000	/* warn at most once every 5s */

/*
 * listenChannelsHash identifies the channels we are actually listening to
 * (ie, have committed a LISTEN on).  It is a hash table of channel names,
 * allocated in TopMemoryContext.
 */
static HTAB *listenChannelsHash = NULL;

/*
 * State for pending LISTEN/UNLISTEN actions consists of an ordered list of
 * all actions requested in the current transaction.  As explained above,
 * we don't actually change listenChannelsHash until we reach transaction commit.
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions attach their lists to their parent's list.
 * Failed subtransactions simply discard their lists.
 */
typedef enum
{
	LISTEN_LISTEN,
	LISTEN_UNLISTEN,
	LISTEN_UNLISTEN_ALL,
} ListenActionKind;

typedef struct
{
	ListenActionKind action;
	char		channel[FLEXIBLE_ARRAY_MEMBER]; /* nul-terminated string */
} ListenAction;

typedef struct ActionList
{
	int			nestingLevel;	/* current transaction nesting depth */
	List	   *actions;		/* list of ListenAction structs */
	struct ActionList *upper;	/* details for upper transaction levels */
} ActionList;

static ActionList *pendingActions = NULL;

/*
 * State for outbound notifies consists of a list of all channels+payloads
 * NOTIFYed in the current transaction.  We do not actually perform a NOTIFY
 * until and unless the transaction commits.  pendingNotifies is NULL if no
 * NOTIFYs have been done in the current (sub) transaction.
 *
 * We discard duplicate notify events issued in the same transaction.
 * Hence, in addition to the list proper (which we need to track the order
 * of the events, since we guarantee to deliver them in order), we build a
 * hash table which we can probe to detect duplicates.  Since building the
 * hash table is somewhat expensive, we do so only once we have at least
 * MIN_HASHABLE_NOTIFIES events queued in the current (sub) transaction;
 * before that we just scan the events linearly.
 *
 * The list is kept in CurTransactionContext.  In subtransactions, each
 * subtransaction has its own list in its own CurTransactionContext, but
 * successful subtransactions add their entries to their parent's list.
 * Failed subtransactions simply discard their lists.  Since these lists
 * are independent, there may be notify events in a subtransaction's list
 * that duplicate events in some ancestor (sub) transaction; we get rid of
 * the dups when merging the subtransaction's list into its parent's.
 *
 * Note: the action and notify lists do not interact within a transaction.
 * In particular, if a transaction does NOTIFY and then LISTEN on the same
 * condition name, it will get a self-notify at commit.  This is a bit odd
 * but is consistent with our historical behavior.
 */
typedef struct Notification
{
	uint16		channel_len;	/* length of channel-name string */
	uint16		payload_len;	/* length of payload string */
	/* null-terminated channel name, then null-terminated payload follow */
	char		data[FLEXIBLE_ARRAY_MEMBER];
} Notification;

typedef struct NotificationList
{
	int			nestingLevel;	/* current transaction nesting depth */
	List	   *events;			/* list of Notification structs */
	HTAB	   *hashtab;		/* hash of NotificationHash structs, or NULL */
	HTAB	   *channelHashtab; /* hash of unique channel names, or NULL */
	struct NotificationList *upper; /* details for upper transaction levels */
} NotificationList;

#define MIN_HASHABLE_NOTIFIES 16	/* threshold to build hashtab */

struct NotificationHash
{
	Notification *event;		/* => the actual Notification struct */
};

struct ChannelHash
{
	char		channel[NAMEDATALEN];
};

static NotificationList *pendingNotifies = NULL;

/*
 * Inbound notifications are initially processed by HandleNotifyInterrupt(),
 * called from inside a signal handler. That just sets the
 * notifyInterruptPending flag and sets the process
 * latch. ProcessNotifyInterrupt() will then be called whenever it's safe to
 * actually deal with the interrupt.
 */
volatile sig_atomic_t notifyInterruptPending = false;

/* True if we've registered an on_shmem_exit cleanup */
static bool unlistenExitRegistered = false;

/* True if we're currently registered as a listener in asyncQueueControl */
static bool amRegisteredListener = false;

/*
 * Queue head positions for direct advancement.
 * These are captured during PreCommit_Notify while holding the heavyweight
 * lock on database 0, ensuring no other backend can insert notifications
 * between them.  SignalBackends uses these to advance idle backends.
 */
static QueuePosition queueHeadBeforeWrite;
static QueuePosition queueHeadAfterWrite;

/*
 * List of channels with pending notifications in the current transaction.
 */
static List *pendingNotifyChannels = NIL;

/* have we advanced to a page that's a multiple of QUEUE_CLEANUP_DELAY? */
static bool tryAdvanceTail = false;

/* GUC parameters */
bool		Trace_notify = false;

/* For 8 KB pages this gives 8 GB of disk space */
int			max_notify_queue_pages = 1048576;

/* local function prototypes */
static inline bool asyncQueuePagePrecedes(int64 p, int64 q);
static void queue_listen(ListenActionKind action, const char *channel);
static void Async_UnlistenOnExit(int code, Datum arg);
static void Exec_ListenPreCommit(void);
static void Exec_ListenCommit(const char *channel);
static void Exec_UnlistenCommit(const char *channel);
static void Exec_UnlistenAllCommit(void);
static bool IsListeningOn(const char *channel);
static void asyncQueueUnregister(void);
static bool asyncQueueIsFull(void);
static bool asyncQueueAdvance(volatile QueuePosition *position, int entryLength);
static void asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe);
static ListCell *asyncQueueAddEntries(ListCell *nextNotify);
static double asyncQueueUsage(void);
static void asyncQueueFillWarning(void);
static void SignalBackends(void);
static void asyncQueueReadAllNotifications(void);
static bool asyncQueueProcessPageEntries(volatile QueuePosition *current,
										 QueuePosition stop,
										 char *page_buffer,
										 Snapshot snapshot);
static void asyncQueueAdvanceTail(void);
static void ProcessIncomingNotify(bool flush);
static bool AsyncExistsPendingNotify(Notification *n);
static void AddEventToPendingNotifies(Notification *n);
static uint32 notification_hash(const void *key, Size keysize);
static int	notification_match(const void *key1, const void *key2, Size keysize);
static void ClearPendingActionsAndNotifies(void);
static inline void ChannelHashPrepareKey(ChannelHashKey *key, Oid dboid, const char *channel);
static dshash_hash channelHashFunc(const void *key, size_t size, void *arg);
static void initChannelHash(void);

/*
 * Determines whether p precedes q.
 * Previously this function accounted for a wraparound.
 */
static inline bool
asyncQueuePagePrecedes(int64 p, int64 q)
{
	return p < q;
}

/*
 * channelHashFunc
 *		Hash function for channel keys.
 */
static dshash_hash
channelHashFunc(const void *key, size_t size, void *arg)
{
	const ChannelHashKey *k = (const ChannelHashKey *) key;
	dshash_hash h;

	h = DatumGetUInt32(hash_uint32(k->dboid));
	h ^= DatumGetUInt32(hash_any((const unsigned char *) k->channel,
								 strnlen(k->channel, NAMEDATALEN)));

	return h;
}

/* parameters for the channel hash table */
static const dshash_parameters channelDSHParams = {
	sizeof(ChannelHashKey),
	sizeof(ChannelEntry),
	dshash_memcmp,
	channelHashFunc,
	dshash_memcpy,
	LWTRANCHE_NOTIFY_CHANNEL_HASH
};

/*
 * initChannelHash
 *		Lazy initialization of the channel hash table.
 */
static void
initChannelHash(void)
{
	MemoryContext oldcontext;

	/* Quick exit if we already did this */
	if (asyncQueueControl->channelHashDSH != DSHASH_HANDLE_INVALID &&
		channelHash != NULL)
		return;

	/* Otherwise, use a lock to ensure only one process creates the table */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);

	/* Be sure any local memory allocated by DSA routines is persistent */
	oldcontext = MemoryContextSwitchTo(TopMemoryContext);

	if (asyncQueueControl->channelHashDSH == DSHASH_HANDLE_INVALID)
	{
		/* Initialize dynamic shared hash table for channel hash */
		channelDSA = dsa_create(LWTRANCHE_NOTIFY_CHANNEL_HASH);
		dsa_pin(channelDSA);
		dsa_pin_mapping(channelDSA);
		channelHash = dshash_create(channelDSA, &channelDSHParams, NULL);

		/* Store handles in shared memory for other backends to use */
		asyncQueueControl->channelHashDSA = dsa_get_handle(channelDSA);
		asyncQueueControl->channelHashDSH =
			dshash_get_hash_table_handle(channelHash);
	}
	else if (!channelHash)
	{
		/* Attach to existing dynamic shared hash table */
		channelDSA = dsa_attach(asyncQueueControl->channelHashDSA);
		dsa_pin_mapping(channelDSA);
		channelHash = dshash_attach(channelDSA, &channelDSHParams,
									asyncQueueControl->channelHashDSH,
									NULL);
	}

	MemoryContextSwitchTo(oldcontext);
	LWLockRelease(NotifyQueueLock);
}

/*
 * initListenChannelsHash
 *		Lazy initialization of the local listen channels hash table.
 */
static void
initListenChannelsHash(void)
{
	HASHCTL		hash_ctl;

	/* Quick exit if we already did this */
	if (listenChannelsHash != NULL)
		return;

	/* Initialize local hash table for this backend's listened channels */
	memset(&hash_ctl, 0, sizeof(hash_ctl));
	hash_ctl.keysize = NAMEDATALEN;
	hash_ctl.entrysize = sizeof(struct ChannelHash);

	listenChannelsHash =
		hash_create("Listen Channels",
					64,
					&hash_ctl,
					HASH_ELEM | HASH_STRINGS);
}

/*
 * Report space needed for our shared memory area
 */
Size
AsyncShmemSize(void)
{
	Size		size;

	/* This had better match AsyncShmemInit */
	size = mul_size(MaxBackends, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	size = add_size(size, SimpleLruShmemSize(notify_buffers, 0));

	return size;
}

/*
 * Initialize our shared memory area
 */
void
AsyncShmemInit(void)
{
	bool		found;
	Size		size;

	/*
	 * Create or attach to the AsyncQueueControl structure.
	 */
	size = mul_size(MaxBackends, sizeof(QueueBackendStatus));
	size = add_size(size, offsetof(AsyncQueueControl, backend));

	asyncQueueControl = (AsyncQueueControl *)
		ShmemInitStruct("Async Queue Control", size, &found);

	if (!found)
	{
		/* First time through, so initialize it */
		SET_QUEUE_POS(QUEUE_HEAD, 0, 0);
		SET_QUEUE_POS(QUEUE_TAIL, 0, 0);
		QUEUE_STOP_PAGE = 0;
		QUEUE_FIRST_LISTENER = INVALID_PROC_NUMBER;
		asyncQueueControl->lastQueueFillWarn = 0;
		asyncQueueControl->channelHashDSA = DSA_HANDLE_INVALID;
		asyncQueueControl->channelHashDSH = DSHASH_HANDLE_INVALID;

		for (int i = 0; i < MaxBackends; i++)
		{
			QUEUE_BACKEND_PID(i) = InvalidPid;
			QUEUE_BACKEND_DBOID(i) = InvalidOid;
			QUEUE_NEXT_LISTENER(i) = INVALID_PROC_NUMBER;
			SET_QUEUE_POS(QUEUE_BACKEND_POS(i), 0, 0);
			QUEUE_BACKEND_WAKEUP_PENDING(i) = false;
			QUEUE_BACKEND_ADVANCING_POS(i) = false;
		}
	}

	/*
	 * Set up SLRU management of the pg_notify data. Note that long segment
	 * names are used in order to avoid wraparound.
	 */
	NotifyCtl->PagePrecedes = asyncQueuePagePrecedes;
	SimpleLruInit(NotifyCtl, "notify", notify_buffers, 0,
				  "pg_notify", LWTRANCHE_NOTIFY_BUFFER, LWTRANCHE_NOTIFY_SLRU,
				  SYNC_HANDLER_NONE, true);

	if (!found)
	{
		/*
		 * During start or reboot, clean out the pg_notify directory.
		 */
		(void) SlruScanDirectory(NotifyCtl, SlruScanDirCbDeleteAll, NULL);
	}
}


/*
 * pg_notify -
 *	  SQL function to send a notification event
 */
Datum
pg_notify(PG_FUNCTION_ARGS)
{
	const char *channel;
	const char *payload;

	if (PG_ARGISNULL(0))
		channel = "";
	else
		channel = text_to_cstring(PG_GETARG_TEXT_PP(0));

	if (PG_ARGISNULL(1))
		payload = "";
	else
		payload = text_to_cstring(PG_GETARG_TEXT_PP(1));

	/* For NOTIFY as a statement, this is checked in ProcessUtility */
	PreventCommandDuringRecovery("NOTIFY");

	Async_Notify(channel, payload);

	PG_RETURN_VOID();
}


/*
 * Async_Notify
 *
 *		This is executed by the SQL notify command.
 *
 *		Adds the message to the list of pending notifies.
 *		Actual notification happens during transaction commit.
 *		^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 */
void
Async_Notify(const char *channel, const char *payload)
{
	int			my_level = GetCurrentTransactionNestLevel();
	size_t		channel_len;
	size_t		payload_len;
	Notification *n;
	MemoryContext oldcontext;

	if (IsParallelWorker())
		elog(ERROR, "cannot send notifications from a parallel worker");

	if (Trace_notify)
		elog(DEBUG1, "Async_Notify(%s)", channel);

	channel_len = channel ? strlen(channel) : 0;
	payload_len = payload ? strlen(payload) : 0;

	/* a channel name must be specified */
	if (channel_len == 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name cannot be empty")));

	/* enforce length limits */
	if (channel_len >= NAMEDATALEN)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("channel name too long")));

	if (payload_len >= NOTIFY_PAYLOAD_MAX_LENGTH)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("payload string too long")));

	/*
	 * We must construct the Notification entry, even if we end up not using
	 * it, in order to compare it cheaply to existing list entries.
	 *
	 * The notification list needs to live until end of transaction, so store
	 * it in the transaction context.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	n = (Notification *) palloc(offsetof(Notification, data) +
								channel_len + payload_len + 2);
	n->channel_len = channel_len;
	n->payload_len = payload_len;
	strcpy(n->data, channel);
	if (payload)
		strcpy(n->data + channel_len + 1, payload);
	else
		n->data[channel_len + 1] = '\0';

	if (pendingNotifies == NULL || my_level > pendingNotifies->nestingLevel)
	{
		NotificationList *notifies;

		/*
		 * First notify event in current (sub)xact. Note that we allocate the
		 * NotificationList in TopTransactionContext; the nestingLevel might
		 * get changed later by AtSubCommit_Notify.
		 */
		notifies = (NotificationList *)
			MemoryContextAlloc(TopTransactionContext,
							   sizeof(NotificationList));
		notifies->nestingLevel = my_level;
		notifies->events = list_make1(n);
		/* We certainly don't need a hashtable yet */
		notifies->hashtab = NULL;
		notifies->channelHashtab = NULL;
		notifies->upper = pendingNotifies;
		pendingNotifies = notifies;
	}
	else
	{
		/* Now check for duplicates */
		if (AsyncExistsPendingNotify(n))
		{
			/* It's a dup, so forget it */
			pfree(n);
			MemoryContextSwitchTo(oldcontext);
			return;
		}

		/* Append more events to existing list */
		AddEventToPendingNotifies(n);
	}

	MemoryContextSwitchTo(oldcontext);
}

/*
 * queue_listen
 *		Common code for listen, unlisten, unlisten all commands.
 *
 *		Adds the request to the list of pending actions.
 *		Actual update of the listenChannelsHash happens during transaction
 *		commit.
 */
static void
queue_listen(ListenActionKind action, const char *channel)
{
	MemoryContext oldcontext;
	ListenAction *actrec;
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * Unlike Async_Notify, we don't try to collapse out duplicates. It would
	 * be too complicated to ensure we get the right interactions of
	 * conflicting LISTEN/UNLISTEN/UNLISTEN_ALL, and it's unlikely that there
	 * would be any performance benefit anyway in sane applications.
	 */
	oldcontext = MemoryContextSwitchTo(CurTransactionContext);

	/* space for terminating null is included in sizeof(ListenAction) */
	actrec = (ListenAction *) palloc(offsetof(ListenAction, channel) +
									 strlen(channel) + 1);
	actrec->action = action;
	strcpy(actrec->channel, channel);

	if (pendingActions == NULL || my_level > pendingActions->nestingLevel)
	{
		ActionList *actions;

		/*
		 * First action in current sub(xact). Note that we allocate the
		 * ActionList in TopTransactionContext; the nestingLevel might get
		 * changed later by AtSubCommit_Notify.
		 */
		actions = (ActionList *)
			MemoryContextAlloc(TopTransactionContext, sizeof(ActionList));
		actions->nestingLevel = my_level;
		actions->actions = list_make1(actrec);
		actions->upper = pendingActions;
		pendingActions = actions;
	}
	else
		pendingActions->actions = lappend(pendingActions->actions, actrec);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Async_Listen
 *
 *		This is executed by the SQL listen command.
 */
void
Async_Listen(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Listen(%s,%d)", channel, MyProcPid);

	queue_listen(LISTEN_LISTEN, channel);
}

/*
 * Async_Unlisten
 *
 *		This is executed by the SQL unlisten command.
 */
void
Async_Unlisten(const char *channel)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_Unlisten(%s,%d)", channel, MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN, channel);
}

/*
 * Async_UnlistenAll
 *
 *		This is invoked by UNLISTEN * command, and also at backend exit.
 */
void
Async_UnlistenAll(void)
{
	if (Trace_notify)
		elog(DEBUG1, "Async_UnlistenAll(%d)", MyProcPid);

	/* If we couldn't possibly be listening, no need to queue anything */
	if (pendingActions == NULL && !unlistenExitRegistered)
		return;

	queue_listen(LISTEN_UNLISTEN_ALL, "");
}

/*
 * SQL function: return a set of the channel names this backend is actively
 * listening to.
 *
 * Note: this coding relies on the fact that the listenChannelsHash cannot
 * change within a transaction.
 */
Datum
pg_listening_channels(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	HASH_SEQ_STATUS *status;

	/* stuff done only on the first call of the function */
	if (SRF_IS_FIRSTCALL())
	{
		MemoryContext oldcontext;

		/* create a function context for cross-call persistence */
		funcctx = SRF_FIRSTCALL_INIT();

		/* Initialize hash table iteration if we have any channels */
		if (listenChannelsHash != NULL)
		{
			oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
			status = (HASH_SEQ_STATUS *) palloc(sizeof(HASH_SEQ_STATUS));
			hash_seq_init(status, listenChannelsHash);
			funcctx->user_fctx = status;
			MemoryContextSwitchTo(oldcontext);
		}
		else
		{
			funcctx->user_fctx = NULL;
		}
	}

	/* stuff done on every call of the function */
	funcctx = SRF_PERCALL_SETUP();
	status = (HASH_SEQ_STATUS *) funcctx->user_fctx;

	if (status != NULL)
	{
		struct ChannelHash *entry;

		entry = (struct ChannelHash *) hash_seq_search(status);
		if (entry != NULL)
			SRF_RETURN_NEXT(funcctx, CStringGetTextDatum(entry->channel));
	}

	SRF_RETURN_DONE(funcctx);
}

/*
 * Async_UnlistenOnExit
 *
 * This is executed at backend exit if we have done any LISTENs in this
 * backend.  It might not be necessary anymore, if the user UNLISTENed
 * everything, but we don't try to detect that case.
 */
static void
Async_UnlistenOnExit(int code, Datum arg)
{
	Exec_UnlistenAllCommit();
	asyncQueueUnregister();
}

/*
 * AtPrepare_Notify
 *
 *		This is called at the prepare phase of a two-phase
 *		transaction.  Save the state for possible commit later.
 */
void
AtPrepare_Notify(void)
{
	/* It's not allowed to have any pending LISTEN/UNLISTEN/NOTIFY actions */
	if (pendingActions || pendingNotifies)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot PREPARE a transaction that has executed LISTEN, UNLISTEN, or NOTIFY")));
}

/*
 * PreCommit_Notify
 *
 *		This is called at transaction commit, before actually committing to
 *		clog.
 *
 *		If there are pending LISTEN actions, make sure we are listed in the
 *		shared-memory listener array.  This must happen before commit to
 *		ensure we don't miss any notifies from transactions that commit
 *		just after ours.
 *
 *		If there are outbound notify requests in the pendingNotifies list,
 *		add them to the global queue.  We do that before commit so that
 *		we can still throw error if we run out of queue space.
 */
void
PreCommit_Notify(void)
{
	ListCell   *p;

	if (!pendingActions && !pendingNotifies)
		return;					/* no relevant statements in this xact */

	if (Trace_notify)
		elog(DEBUG1, "PreCommit_Notify");

	/* Preflight for any pending listen/unlisten actions */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenPreCommit();
					break;
				case LISTEN_UNLISTEN:
					/* there is no Exec_UnlistenPreCommit() */
					break;
				case LISTEN_UNLISTEN_ALL:
					/* there is no Exec_UnlistenAllPreCommit() */
					break;
			}
		}
	}

	/* Queue any pending notifies (must happen after the above) */
	if (pendingNotifies)
	{
		ListCell   *nextNotify;
		bool		firstIteration = true;

		/*
		 * Build list of unique channels for SignalBackends().
		 *
		 * If we have a channelHashtab, use it to efficiently get the unique
		 * channels.  Otherwise, fall back to the linear approach.
		 */
		pendingNotifyChannels = NIL;
		if (pendingNotifies->channelHashtab != NULL)
		{
			HASH_SEQ_STATUS status;
			struct ChannelHash *channelEntry;

			hash_seq_init(&status, pendingNotifies->channelHashtab);
			while ((channelEntry = (struct ChannelHash *) hash_seq_search(&status)) != NULL)
				pendingNotifyChannels = lappend(pendingNotifyChannels, channelEntry->channel);
		}
		else
		{
			/* Linear approach for small number of notifications */
			foreach_ptr(Notification, n, pendingNotifies->events)
			{
				char	   *channel = n->data;

				/* Add if not already in list */
				if (!list_member_ptr(pendingNotifyChannels, channel))
					pendingNotifyChannels = lappend(pendingNotifyChannels, channel);
			}
		}

		/*
		 * Make sure that we have an XID assigned to the current transaction.
		 * GetCurrentTransactionId is cheap if we already have an XID, but not
		 * so cheap if we don't, and we'd prefer not to do that work while
		 * holding NotifyQueueLock.
		 */
		(void) GetCurrentTransactionId();

		/*
		 * Serialize writers by acquiring a special lock that we hold till
		 * after commit.  This ensures that queue entries appear in commit
		 * order, and in particular that there are never uncommitted queue
		 * entries ahead of committed ones, so an uncommitted transaction
		 * can't block delivery of deliverable notifications.
		 *
		 * We use a heavyweight lock so that it'll automatically be released
		 * after either commit or abort.  This also allows deadlocks to be
		 * detected, though really a deadlock shouldn't be possible here.
		 *
		 * The lock is on "database 0", which is pretty ugly but it doesn't
		 * seem worth inventing a special locktag category just for this.
		 * (Historical note: before PG 9.0, a similar lock on "database 0" was
		 * used by the flatfiles mechanism.)
		 */
		LockSharedObject(DatabaseRelationId, InvalidOid, 0,
						 AccessExclusiveLock);
		/*
		 * For the direct advancement optimization in SignalBackends(), we
		 * need to ensure that no other backend can insert queue entries
		 * between queueHeadBeforeWrite and queueHeadAfterWrite.  The
		 * heavyweight lock above provides this guarantee, since it serializes
		 * all writers.
		 *
		 * Note: if the heavyweight lock were ever removed for scalability
		 * reasons, we could achieve the same guarantee by holding
		 * NotifyQueueLock in EXCLUSIVE mode across all our insertions, rather
		 * than releasing and reacquiring it for each page as we do below.
		 */

		/* Initialize queueHeadBeforeWrite to a safe default */
		SET_QUEUE_POS(queueHeadBeforeWrite, 0, 0);

		/* Now push the notifications into the queue */
		nextNotify = list_head(pendingNotifies->events);
		while (nextNotify != NULL)
		{
			/*
			 * Add the pending notifications to the queue.  We acquire and
			 * release NotifyQueueLock once per page, which might be overkill
			 * but it does allow readers to get in while we're doing this.
			 *
			 * A full queue is very uncommon and should really not happen,
			 * given that we have so much space available in the SLRU pages.
			 * Nevertheless we need to deal with this possibility. Note that
			 * when we get here we are in the process of committing our
			 * transaction, but we have not yet committed to clog, so at this
			 * point in time we can still roll the transaction back.
			 */
			LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
			if (firstIteration)
			{
				queueHeadBeforeWrite = QUEUE_HEAD;
				firstIteration = false;
			}

			asyncQueueFillWarning();
			if (asyncQueueIsFull())
				ereport(ERROR,
						(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
						 errmsg("too many notifications in the NOTIFY queue")));
			nextNotify = asyncQueueAddEntries(nextNotify);
			queueHeadAfterWrite = QUEUE_HEAD;

			LWLockRelease(NotifyQueueLock);
		}

		/* Note that we don't clear pendingNotifies; AtCommit_Notify will. */
	}
}

/*
 * AtCommit_Notify
 *
 *		This is called at transaction commit, after committing to clog.
 *
 *		Update listenChannelsHash and clear transaction-local state.
 *
 *		If we issued any notifications in the transaction, send signals to
 *		listening backends (possibly including ourselves) to process them.
 *		Also, if we filled enough queue pages with new notifies, try to
 *		advance the queue tail pointer.
 */
void
AtCommit_Notify(void)
{
	ListCell   *p;

	/*
	 * Allow transactions that have not executed LISTEN/UNLISTEN/NOTIFY to
	 * return as soon as possible
	 */
	if (!pendingActions && !pendingNotifies)
		return;

	if (Trace_notify)
		elog(DEBUG1, "AtCommit_Notify");

	/* Perform any pending listen/unlisten actions */
	if (pendingActions != NULL)
	{
		foreach(p, pendingActions->actions)
		{
			ListenAction *actrec = (ListenAction *) lfirst(p);

			switch (actrec->action)
			{
				case LISTEN_LISTEN:
					Exec_ListenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN:
					Exec_UnlistenCommit(actrec->channel);
					break;
				case LISTEN_UNLISTEN_ALL:
					Exec_UnlistenAllCommit();
					break;
			}
		}
	}

	/* If no longer listening to anything, get out of listener array */
	if (amRegisteredListener &&
		(listenChannelsHash == NULL || hash_get_num_entries(listenChannelsHash) == 0))
		asyncQueueUnregister();

	/*
	 * Send signals to listening backends.  We need do this only if there are
	 * pending notifies, which were previously added to the shared queue by
	 * PreCommit_Notify().
	 */
	if (pendingNotifies != NULL)
		SignalBackends();

	/*
	 * If it's time to try to advance the global tail pointer, do that.
	 *
	 * (It might seem odd to do this in the sender, when more than likely the
	 * listeners won't yet have read the messages we just sent.  However,
	 * there's less contention if only the sender does it, and there is little
	 * need for urgency in advancing the global tail.  So this typically will
	 * be clearing out messages that were sent some time ago.)
	 */
	if (tryAdvanceTail)
	{
		tryAdvanceTail = false;
		asyncQueueAdvanceTail();
	}

	/* And clean up */
	ClearPendingActionsAndNotifies();
}

/*
 * Exec_ListenPreCommit --- subroutine for PreCommit_Notify
 *
 * This function must make sure we are ready to catch any incoming messages.
 */
static void
Exec_ListenPreCommit(void)
{
	QueuePosition head;
	QueuePosition max;
	ProcNumber	prevListener;

	/*
	 * Nothing to do if we are already listening to something, nor if we
	 * already ran this routine in this transaction.
	 */
	if (amRegisteredListener)
		return;

	if (Trace_notify)
		elog(DEBUG1, "Exec_ListenPreCommit(%d)", MyProcPid);

	/*
	 * Before registering, make sure we will unlisten before dying. (Note:
	 * this action does not get undone if we abort later.)
	 */
	if (!unlistenExitRegistered)
	{
		before_shmem_exit(Async_UnlistenOnExit, 0);
		unlistenExitRegistered = true;
	}

	/*
	 * This is our first LISTEN, so establish our pointer.
	 *
	 * We set our pointer to the global tail pointer and then move it forward
	 * over already-committed notifications.  This ensures we cannot miss any
	 * not-yet-committed notifications.  We might get a few more but that
	 * doesn't hurt.
	 *
	 * In some scenarios there might be a lot of committed notifications that
	 * have not yet been pruned away (because some backend is being lazy about
	 * reading them).  To reduce our startup time, we can look at other
	 * backends and adopt the maximum "pos" pointer of any backend that's in
	 * our database; any notifications it's already advanced over are surely
	 * committed and need not be re-examined by us.  (We must consider only
	 * backends connected to our DB, because others will not have bothered to
	 * check committed-ness of notifications in our DB.)
	 *
	 * We need exclusive lock here so we can look at other backends' entries
	 * and manipulate the list links.
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	head = QUEUE_HEAD;
	max = QUEUE_TAIL;
	prevListener = INVALID_PROC_NUMBER;
	for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
	{
		if (QUEUE_BACKEND_DBOID(i) == MyDatabaseId)
			max = QUEUE_POS_MAX(max, QUEUE_BACKEND_POS(i));
		/* Also find last listening backend before this one */
		if (i < MyProcNumber)
			prevListener = i;
	}
	QUEUE_BACKEND_POS(MyProcNumber) = max;
	QUEUE_BACKEND_PID(MyProcNumber) = MyProcPid;
	QUEUE_BACKEND_DBOID(MyProcNumber) = MyDatabaseId;
	/* Insert backend into list of listeners at correct position */
	if (prevListener != INVALID_PROC_NUMBER)
	{
		QUEUE_NEXT_LISTENER(MyProcNumber) = QUEUE_NEXT_LISTENER(prevListener);
		QUEUE_NEXT_LISTENER(prevListener) = MyProcNumber;
	}
	else
	{
		QUEUE_NEXT_LISTENER(MyProcNumber) = QUEUE_FIRST_LISTENER;
		QUEUE_FIRST_LISTENER = MyProcNumber;
	}
	LWLockRelease(NotifyQueueLock);

	/* Now we are listed in the global array, so remember we're listening */
	amRegisteredListener = true;

	/*
	 * Try to move our pointer forward as far as possible.  This will skip
	 * over already-committed notifications, which we want to do because they
	 * might be quite stale.  Note that we are not yet listening on anything,
	 * so we won't deliver such notifications to our frontend.  Also, although
	 * our transaction might have executed NOTIFY, those message(s) aren't
	 * queued yet so we won't skip them here.
	 */
	if (!QUEUE_POS_EQUAL(max, head))
		asyncQueueReadAllNotifications();
}

/*
 * Exec_ListenCommit --- subroutine for AtCommit_Notify
 *
 * Add the channel to the list of channels we are listening on.
 */
static void
Exec_ListenCommit(const char *channel)
{
	ChannelHashKey key;
	ChannelEntry *entry;
	bool		found;
	ProcNumber *listeners;

	/* Do nothing if we are already listening on this channel */
	if (IsListeningOn(channel))
		return;

	/*
	 * Add the new channel name to listenChannelsHash.
	 *
	 * XXX It is theoretically possible to get an out-of-memory failure here,
	 * which would be bad because we already committed.  For the moment it
	 * doesn't seem worth trying to guard against that, but maybe improve this
	 * later.
	 */
	initListenChannelsHash();
	(void) hash_search(listenChannelsHash, channel, HASH_ENTER, NULL);

	/* Now update the shared channelHash for SignalBackends() to use */
	initChannelHash();

	ChannelHashPrepareKey(&key, MyDatabaseId, channel);

	/*
	 * For new entries, we initialize listenersArray to InvalidDsaPointer as a
	 * marker. This handles both the initial creation and potential retry
	 * after OOM.
	 */
	entry = dshash_find_or_insert(channelHash, &key, &found);

	if (!found)
		entry->listenersArray = InvalidDsaPointer;

	if (!DsaPointerIsValid(entry->listenersArray))
	{
		/* First listener for this channel */
		entry->listenersArray = dsa_allocate(channelDSA,
											 sizeof(ProcNumber) * INITIAL_LISTENERS_ARRAY_SIZE);
		entry->numListeners = 0;
		entry->allocatedListeners = INITIAL_LISTENERS_ARRAY_SIZE;
	}

	listeners = (ProcNumber *) dsa_get_address(channelDSA,
											   entry->listenersArray);

	for (int i = 0; i < entry->numListeners; i++)
	{
		if (listeners[i] == MyProcNumber)
		{
			dshash_release_lock(channelHash, entry);
			return;				/* Already registered */
		}
	}

	if (entry->numListeners >= entry->allocatedListeners)
	{
		int			new_size = entry->allocatedListeners * 2;
		dsa_pointer new_array = dsa_allocate(channelDSA,
											 sizeof(ProcNumber) * new_size);
		ProcNumber *new_listeners = (ProcNumber *) dsa_get_address(channelDSA,
																   new_array);

		memcpy(new_listeners, listeners,
			   sizeof(ProcNumber) * entry->numListeners);

		dsa_free(channelDSA, entry->listenersArray);
		entry->listenersArray = new_array;
		entry->allocatedListeners = new_size;
		listeners = new_listeners;
	}

	listeners[entry->numListeners] = MyProcNumber;
	entry->numListeners++;

	dshash_release_lock(channelHash, entry);
}

/*
 * Exec_UnlistenCommit --- subroutine for AtCommit_Notify
 *
 *		Unlisten the specified channel for this backend.
 */
static void
Exec_UnlistenCommit(const char *channel)
{
	ChannelHashKey key;
	ChannelEntry *entry;
	ProcNumber *listeners;
	int			i;

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenCommit(%s,%d)", channel, MyProcPid);

	/* Remove from our local cache */
	if (listenChannelsHash != NULL)
		(void) hash_search(listenChannelsHash, channel, HASH_REMOVE, NULL);

	/* Now remove from the shared channelHash */
	if (channelHash == NULL)
		return;

	ChannelHashPrepareKey(&key, MyDatabaseId, channel);

	/* Look up the channel with exclusive lock so we can modify it */
	entry = dshash_find(channelHash, &key, true);
	if (entry == NULL)
		return;

	listeners = (ProcNumber *) dsa_get_address(channelDSA,
											   entry->listenersArray);

	for (i = 0; i < entry->numListeners; i++)
	{
		if (listeners[i] == MyProcNumber)
		{
			entry->numListeners--;
			if (i < entry->numListeners)
				memmove(&listeners[i], &listeners[i + 1],
						sizeof(ProcNumber) * (entry->numListeners - i));

			if (entry->numListeners == 0)
			{
				/* Last listener for this channel */
				dsa_free(channelDSA, entry->listenersArray);
				dshash_delete_entry(channelHash, entry);
			}
			else
			{
				dshash_release_lock(channelHash, entry);
			}

			return;
		}
	}

	dshash_release_lock(channelHash, entry);

	/*
	 * We do not complain about unlistening something not being listened;
	 * should we?
	 */
}

/*
 * Exec_UnlistenAllCommit --- subroutine for AtCommit_Notify
 *
 *		Unlisten on all channels for this backend.
 */
static void
Exec_UnlistenAllCommit(void)
{
	dshash_seq_status status;
	ChannelEntry *entry;

	if (Trace_notify)
		elog(DEBUG1, "Exec_UnlistenAllCommit(%d)", MyProcPid);

	/* Clear our local cache */
	if (listenChannelsHash != NULL)
	{
		hash_destroy(listenChannelsHash);
		listenChannelsHash = NULL;
	}

	/* Now remove from the shared channelHash */
	if (channelHash == NULL)
		return;

	dshash_seq_init(&status, channelHash, true);
	while ((entry = dshash_seq_next(&status)) != NULL)
	{
		if (entry->key.dboid == MyDatabaseId)
		{
			ProcNumber *listeners;
			int			i;

			listeners = (ProcNumber *) dsa_get_address(channelDSA,
													   entry->listenersArray);

			for (i = 0; i < entry->numListeners; i++)
			{
				if (listeners[i] == MyProcNumber)
				{
					entry->numListeners--;
					if (i < entry->numListeners)
						memmove(&listeners[i], &listeners[i + 1],
								sizeof(ProcNumber) * (entry->numListeners - i));

					if (entry->numListeners == 0)
					{
						dsa_free(channelDSA, entry->listenersArray);
						dshash_delete_current(&status);
					}
					break;
				}
			}
		}
	}
	dshash_seq_term(&status);
}

/*
 * Test whether we are actively listening on the given channel name.
 *
 * Note: this function is executed for every notification found in the queue.
 */
static bool
IsListeningOn(const char *channel)
{
	if (listenChannelsHash == NULL)
		return false;

	return (hash_search(listenChannelsHash, channel, HASH_FIND, NULL) != NULL);
}

/*
 * Remove our entry from the listeners array when we are no longer listening
 * on any channel.  NB: must not fail if we're already not listening.
 */
static void
asyncQueueUnregister(void)
{
	Assert(listenChannelsHash == NULL || hash_get_num_entries(listenChannelsHash) == 0);	/* else caller error */

	if (!amRegisteredListener)	/* nothing to do */
		return;

	/*
	 * Need exclusive lock here to manipulate list links.
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	/* Mark our entry as invalid */
	QUEUE_BACKEND_PID(MyProcNumber) = InvalidPid;
	QUEUE_BACKEND_DBOID(MyProcNumber) = InvalidOid;
	QUEUE_BACKEND_WAKEUP_PENDING(MyProcNumber) = false;
	/* and remove it from the list */
	if (QUEUE_FIRST_LISTENER == MyProcNumber)
		QUEUE_FIRST_LISTENER = QUEUE_NEXT_LISTENER(MyProcNumber);
	else
	{
		for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
		{
			if (QUEUE_NEXT_LISTENER(i) == MyProcNumber)
			{
				QUEUE_NEXT_LISTENER(i) = QUEUE_NEXT_LISTENER(MyProcNumber);
				break;
			}
		}
	}
	QUEUE_NEXT_LISTENER(MyProcNumber) = INVALID_PROC_NUMBER;
	LWLockRelease(NotifyQueueLock);

	/* mark ourselves as no longer listed in the global array */
	amRegisteredListener = false;
}

/*
 * Test whether there is room to insert more notification messages.
 *
 * Caller must hold at least shared NotifyQueueLock.
 */
static bool
asyncQueueIsFull(void)
{
	int64		headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int64		tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int64		occupied = headPage - tailPage;

	return occupied >= max_notify_queue_pages;
}

/*
 * Advance the QueuePosition to the next entry, assuming that the current
 * entry is of length entryLength.  If we jump to a new page the function
 * returns true, else false.
 */
static bool
asyncQueueAdvance(volatile QueuePosition *position, int entryLength)
{
	int64		pageno = QUEUE_POS_PAGE(*position);
	int			offset = QUEUE_POS_OFFSET(*position);
	bool		pageJump = false;

	/*
	 * Move to the next writing position: First jump over what we have just
	 * written or read.
	 */
	offset += entryLength;
	Assert(offset <= QUEUE_PAGESIZE);

	/*
	 * In a second step check if another entry can possibly be written to the
	 * page. If so, stay here, we have reached the next position. If not, then
	 * we need to move on to the next page.
	 */
	if (offset + QUEUEALIGN(AsyncQueueEntryEmptySize) > QUEUE_PAGESIZE)
	{
		pageno++;
		offset = 0;
		pageJump = true;
	}

	SET_QUEUE_POS(*position, pageno, offset);
	return pageJump;
}

/*
 * Fill the AsyncQueueEntry at *qe with an outbound notification message.
 */
static void
asyncQueueNotificationToEntry(Notification *n, AsyncQueueEntry *qe)
{
	size_t		channellen = n->channel_len;
	size_t		payloadlen = n->payload_len;
	int			entryLength;

	Assert(channellen < NAMEDATALEN);
	Assert(payloadlen < NOTIFY_PAYLOAD_MAX_LENGTH);

	/* The terminators are already included in AsyncQueueEntryEmptySize */
	entryLength = AsyncQueueEntryEmptySize + payloadlen + channellen;
	entryLength = QUEUEALIGN(entryLength);
	qe->length = entryLength;
	qe->dboid = MyDatabaseId;
	qe->xid = GetCurrentTransactionId();
	qe->srcPid = MyProcPid;
	memcpy(qe->data, n->data, channellen + payloadlen + 2);
}

/*
 * Add pending notifications to the queue.
 *
 * We go page by page here, i.e. we stop once we have to go to a new page but
 * we will be called again and then fill that next page. If an entry does not
 * fit into the current page, we write a dummy entry with an InvalidOid as the
 * database OID in order to fill the page. So every page is always used up to
 * the last byte which simplifies reading the page later.
 *
 * We are passed the list cell (in pendingNotifies->events) containing the next
 * notification to write and return the first still-unwritten cell back.
 * Eventually we will return NULL indicating all is done.
 *
 * We are holding NotifyQueueLock already from the caller and grab
 * page specific SLRU bank lock locally in this function.
 */
static ListCell *
asyncQueueAddEntries(ListCell *nextNotify)
{
	AsyncQueueEntry qe;
	QueuePosition queue_head;
	int64		pageno;
	int			offset;
	int			slotno;
	LWLock	   *prevlock;

	/*
	 * We work with a local copy of QUEUE_HEAD, which we write back to shared
	 * memory upon exiting.  The reason for this is that if we have to advance
	 * to a new page, SimpleLruZeroPage might fail (out of disk space, for
	 * instance), and we must not advance QUEUE_HEAD if it does.  (Otherwise,
	 * subsequent insertions would try to put entries into a page that slru.c
	 * thinks doesn't exist yet.)  So, use a local position variable.  Note
	 * that if we do fail, any already-inserted queue entries are forgotten;
	 * this is okay, since they'd be useless anyway after our transaction
	 * rolls back.
	 */
	queue_head = QUEUE_HEAD;

	/*
	 * If this is the first write since the postmaster started, we need to
	 * initialize the first page of the async SLRU.  Otherwise, the current
	 * page should be initialized already, so just fetch it.
	 */
	pageno = QUEUE_POS_PAGE(queue_head);
	prevlock = SimpleLruGetBankLock(NotifyCtl, pageno);

	/* We hold both NotifyQueueLock and SLRU bank lock during this operation */
	LWLockAcquire(prevlock, LW_EXCLUSIVE);

	if (QUEUE_POS_IS_ZERO(queue_head))
		slotno = SimpleLruZeroPage(NotifyCtl, pageno);
	else
		slotno = SimpleLruReadPage(NotifyCtl, pageno, true,
								   InvalidTransactionId);

	/* Note we mark the page dirty before writing in it */
	NotifyCtl->shared->page_dirty[slotno] = true;

	while (nextNotify != NULL)
	{
		Notification *n = (Notification *) lfirst(nextNotify);

		/* Construct a valid queue entry in local variable qe */
		asyncQueueNotificationToEntry(n, &qe);

		offset = QUEUE_POS_OFFSET(queue_head);

		/* Check whether the entry really fits on the current page */
		if (offset + qe.length <= QUEUE_PAGESIZE)
		{
			/* OK, so advance nextNotify past this item */
			nextNotify = lnext(pendingNotifies->events, nextNotify);
		}
		else
		{
			/*
			 * Write a dummy entry to fill up the page. Actually readers will
			 * only check dboid and since it won't match any reader's database
			 * OID, they will ignore this entry and move on.
			 */
			qe.length = QUEUE_PAGESIZE - offset;
			qe.dboid = InvalidOid;
			qe.data[0] = '\0';	/* empty channel */
			qe.data[1] = '\0';	/* empty payload */
		}

		/* Now copy qe into the shared buffer page */
		memcpy(NotifyCtl->shared->page_buffer[slotno] + offset,
			   &qe,
			   qe.length);

		/* Advance queue_head appropriately, and detect if page is full */
		if (asyncQueueAdvance(&(queue_head), qe.length))
		{
			LWLock	   *lock;

			pageno = QUEUE_POS_PAGE(queue_head);
			lock = SimpleLruGetBankLock(NotifyCtl, pageno);
			if (lock != prevlock)
			{
				LWLockRelease(prevlock);
				LWLockAcquire(lock, LW_EXCLUSIVE);
				prevlock = lock;
			}

			/*
			 * Page is full, so we're done here, but first fill the next page
			 * with zeroes.  The reason to do this is to ensure that slru.c's
			 * idea of the head page is always the same as ours, which avoids
			 * boundary problems in SimpleLruTruncate.  The test in
			 * asyncQueueIsFull() ensured that there is room to create this
			 * page without overrunning the queue.
			 */
			slotno = SimpleLruZeroPage(NotifyCtl, QUEUE_POS_PAGE(queue_head));

			/*
			 * If the new page address is a multiple of QUEUE_CLEANUP_DELAY,
			 * set flag to remember that we should try to advance the tail
			 * pointer (we don't want to actually do that right here).
			 */
			if (QUEUE_POS_PAGE(queue_head) % QUEUE_CLEANUP_DELAY == 0)
				tryAdvanceTail = true;

			/* And exit the loop */
			break;
		}
	}

	/* Success, so update the global QUEUE_HEAD */
	QUEUE_HEAD = queue_head;

	LWLockRelease(prevlock);

	return nextNotify;
}

/*
 * SQL function to return the fraction of the notification queue currently
 * occupied.
 */
Datum
pg_notification_queue_usage(PG_FUNCTION_ARGS)
{
	double		usage;

	/* Advance the queue tail so we don't report a too-large result */
	asyncQueueAdvanceTail();

	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	usage = asyncQueueUsage();
	LWLockRelease(NotifyQueueLock);

	PG_RETURN_FLOAT8(usage);
}

/*
 * Return the fraction of the queue that is currently occupied.
 *
 * The caller must hold NotifyQueueLock in (at least) shared mode.
 *
 * Note: we measure the distance to the logical tail page, not the physical
 * tail page.  In some sense that's wrong, but the relative position of the
 * physical tail is affected by details such as SLRU segment boundaries,
 * so that a result based on that is unpleasantly unstable.
 */
static double
asyncQueueUsage(void)
{
	int64		headPage = QUEUE_POS_PAGE(QUEUE_HEAD);
	int64		tailPage = QUEUE_POS_PAGE(QUEUE_TAIL);
	int64		occupied = headPage - tailPage;

	if (occupied == 0)
		return (double) 0;		/* fast exit for common case */

	return (double) occupied / (double) max_notify_queue_pages;
}

/*
 * Check whether the queue is at least half full, and emit a warning if so.
 *
 * This is unlikely given the size of the queue, but possible.
 * The warnings show up at most once every QUEUE_FULL_WARN_INTERVAL.
 *
 * Caller must hold exclusive NotifyQueueLock.
 */
static void
asyncQueueFillWarning(void)
{
	double		fillDegree;
	TimestampTz t;

	fillDegree = asyncQueueUsage();
	if (fillDegree < 0.5)
		return;

	t = GetCurrentTimestamp();

	if (TimestampDifferenceExceeds(asyncQueueControl->lastQueueFillWarn,
								   t, QUEUE_FULL_WARN_INTERVAL))
	{
		QueuePosition min = QUEUE_HEAD;
		int32		minPid = InvalidPid;

		for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
		{
			Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
			min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
			if (QUEUE_POS_EQUAL(min, QUEUE_BACKEND_POS(i)))
				minPid = QUEUE_BACKEND_PID(i);
		}

		ereport(WARNING,
				(errmsg("NOTIFY queue is %.0f%% full", fillDegree * 100),
				 (minPid != InvalidPid ?
				  errdetail("The server process with PID %d is among those with the oldest transactions.", minPid)
				  : 0),
				 (minPid != InvalidPid ?
				  errhint("The NOTIFY queue cannot be emptied until that process ends its current transaction.")
				  : 0)));

		asyncQueueControl->lastQueueFillWarn = t;
	}
}

/*
 * Send signals to listening backends.
 *
 * Normally we signal only backends in our own database, that are
 * listening on the channels with pending notifies, since only those
 * backends are interested in notifies we send.
 *
 * Backends that are known to still be positioned at the queue head
 * from before our commit can be safely advanced directly to the new
 * head, since the queue region we wrote is known to contain only our
 * own notifications.  This avoids unnecessary wakeups when there is
 * nothing of interest to them.
 *
 * Since we know the ProcNumber and the Pid the signaling is quite cheap.
 *
 * This is called during CommitTransaction(), so it's important for it
 * to have very low probability of failure.
 */
static void
SignalBackends(void)
{
	int32	   *pids;
	ProcNumber *procnos;
	int			count;
	ListCell   *lc;

	/*
	 * Attach to the channel hash if needed.  We might not have one if this
	 * backend hasn't done LISTEN, but we need it to find listeners.
	 */
	initChannelHash();

	/*
	 * Identify backends that we need to signal.  We don't want to send
	 * signals while holding the NotifyQueueLock, so this loop just builds a
	 * list of target PIDs.
	 *
	 * XXX in principle these pallocs could fail, which would be bad. Maybe
	 * preallocate the arrays?  They're not that large, though.
	 */
	pids = (int32 *) palloc(MaxBackends * sizeof(int32));
	procnos = (ProcNumber *) palloc(MaxBackends * sizeof(ProcNumber));
	count = 0;

	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	foreach(lc, pendingNotifyChannels)
	{
		char	   *channel = (char *) lfirst(lc);
		ChannelEntry *entry = NULL;
		ProcNumber *listeners;

		if (channelHash != NULL)
		{
			ChannelHashKey key;

			ChannelHashPrepareKey(&key, MyDatabaseId, channel);
			entry = dshash_find(channelHash, &key, false);
		}

		if (entry == NULL)
			continue;			/* No listeners registered for this channel */

		listeners = (ProcNumber *) dsa_get_address(channelDSA,
												   entry->listenersArray);

		for (int j = 0; j < entry->numListeners; j++)
		{
			ProcNumber	i = listeners[j];
			int32		pid;
			QueuePosition pos;

			if (QUEUE_BACKEND_WAKEUP_PENDING(i))
				continue;

			pos = QUEUE_BACKEND_POS(i);
			pid = QUEUE_BACKEND_PID(i);

			/* Skip if caught up */
			if (QUEUE_POS_EQUAL(pos, QUEUE_HEAD))
				continue;

			Assert(pid != InvalidPid);

			QUEUE_BACKEND_WAKEUP_PENDING(i) = true;
			pids[count] = pid;
			procnos[count] = i;
			count++;
		}

		dshash_release_lock(channelHash, entry);
	}

	/*
	 * Direct advancement: avoid waking non-caught up backends that aren't
	 * interested in our notifications.
	 */
	if (pendingNotifies != NULL)
	{
		for (ProcNumber i = QUEUE_FIRST_LISTENER;
			 i != INVALID_PROC_NUMBER;
			 i = QUEUE_NEXT_LISTENER(i))
		{
			QueuePosition pos;
			int32		pid;

			if (QUEUE_BACKEND_WAKEUP_PENDING(i))
				continue;

			pos = QUEUE_BACKEND_POS(i);
			pid = QUEUE_BACKEND_PID(i);

			if (QUEUE_POS_EQUAL(pos, queueHeadBeforeWrite))
			{
				/*
				 * Safe to directly update a backend's shared pos if it isn't
				 * currently advancing its position.
				 */
				if (!QUEUE_BACKEND_ADVANCING_POS(i))
					QUEUE_BACKEND_POS(i) = queueHeadAfterWrite;
			}
			else if (QUEUE_POS_PRECEDES(pos, queueHeadBeforeWrite))
			{
				/*
				 * Need to signal, cannot skip over, since we don't know if
				 * the notifications between pos and the queue head before our
				 * write are of interest for this backend or not.
				 */
				Assert(pid != InvalidPid);

				QUEUE_BACKEND_WAKEUP_PENDING(i) = true;
				pids[count] = pid;
				procnos[count] = i;
				count++;
			}
			else
			{
				/*
				 * The backend is already ahead of the notifications we wrote.
				 * No need to do anything.
				 */
				Assert(QUEUE_POS_PRECEDES(queueHeadBeforeWrite, pos));
			}
		}
	}
	LWLockRelease(NotifyQueueLock);

	/* Now send signals */
	for (int i = 0; i < count; i++)
	{
		int32		pid = pids[i];

		/*
		 * If we are signaling our own process, no need to involve the kernel;
		 * just set the flag directly.
		 */
		if (pid == MyProcPid)
		{
			notifyInterruptPending = true;
			continue;
		}

		/*
		 * Note: assuming things aren't broken, a signal failure here could
		 * only occur if the target backend exited since we released
		 * NotifyQueueLock; which is unlikely but certainly possible. So we
		 * just log a low-level debug message if it happens.
		 */
		if (SendProcSignal(pid, PROCSIG_NOTIFY_INTERRUPT, procnos[i]) < 0)
			elog(DEBUG3, "could not signal backend with PID %d: %m", pid);
	}

	pfree(pids);
	pfree(procnos);
}

/*
 * AtAbort_Notify
 *
 *	This is called at transaction abort.
 *
 *	Gets rid of pending actions and outbound notifies that we would have
 *	executed if the transaction got committed.
 */
void
AtAbort_Notify(void)
{
	/*
	 * If we LISTEN but then roll back the transaction after PreCommit_Notify,
	 * we have registered as a listener but have not made any entry in
	 * listenChannelsHash.  In that case, deregister again.
	 */
	if (amRegisteredListener &&
		(listenChannelsHash == NULL || hash_get_num_entries(listenChannelsHash) == 0))
		asyncQueueUnregister();

	/* And clean up */
	ClearPendingActionsAndNotifies();
}

/*
 * AtSubCommit_Notify() --- Take care of subtransaction commit.
 *
 * Reassign all items in the pending lists to the parent transaction.
 */
void
AtSubCommit_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/* If there are actions at our nesting level, we must reparent them. */
	if (pendingActions != NULL &&
		pendingActions->nestingLevel >= my_level)
	{
		if (pendingActions->upper == NULL ||
			pendingActions->upper->nestingLevel < my_level - 1)
		{
			/* nothing to merge; give the whole thing to the parent */
			--pendingActions->nestingLevel;
		}
		else
		{
			ActionList *childPendingActions = pendingActions;

			pendingActions = pendingActions->upper;

			/*
			 * Mustn't try to eliminate duplicates here --- see queue_listen()
			 */
			pendingActions->actions =
				list_concat(pendingActions->actions,
							childPendingActions->actions);
			pfree(childPendingActions);
		}
	}

	/* If there are notifies at our nesting level, we must reparent them. */
	if (pendingNotifies != NULL &&
		pendingNotifies->nestingLevel >= my_level)
	{
		Assert(pendingNotifies->nestingLevel == my_level);

		if (pendingNotifies->upper == NULL ||
			pendingNotifies->upper->nestingLevel < my_level - 1)
		{
			/* nothing to merge; give the whole thing to the parent */
			--pendingNotifies->nestingLevel;
		}
		else
		{
			/*
			 * Formerly, we didn't bother to eliminate duplicates here, but
			 * now we must, else we fall foul of "Assert(!found)", either here
			 * or during a later attempt to build the parent-level hashtable.
			 */
			NotificationList *childPendingNotifies = pendingNotifies;
			ListCell   *l;

			pendingNotifies = pendingNotifies->upper;
			/* Insert all the subxact's events into parent, except for dups */
			foreach(l, childPendingNotifies->events)
			{
				Notification *childn = (Notification *) lfirst(l);

				if (!AsyncExistsPendingNotify(childn))
					AddEventToPendingNotifies(childn);
			}
			pfree(childPendingNotifies);
		}
	}
}

/*
 * AtSubAbort_Notify() --- Take care of subtransaction abort.
 */
void
AtSubAbort_Notify(void)
{
	int			my_level = GetCurrentTransactionNestLevel();

	/*
	 * All we have to do is pop the stack --- the actions/notifies made in
	 * this subxact are no longer interesting, and the space will be freed
	 * when CurTransactionContext is recycled. We still have to free the
	 * ActionList and NotificationList objects themselves, though, because
	 * those are allocated in TopTransactionContext.
	 *
	 * Note that there might be no entries at all, or no entries for the
	 * current subtransaction level, either because none were ever created, or
	 * because we reentered this routine due to trouble during subxact abort.
	 */
	while (pendingActions != NULL &&
		   pendingActions->nestingLevel >= my_level)
	{
		ActionList *childPendingActions = pendingActions;

		pendingActions = pendingActions->upper;
		pfree(childPendingActions);
	}

	while (pendingNotifies != NULL &&
		   pendingNotifies->nestingLevel >= my_level)
	{
		NotificationList *childPendingNotifies = pendingNotifies;

		pendingNotifies = pendingNotifies->upper;
		pfree(childPendingNotifies);
	}
}

/*
 * HandleNotifyInterrupt
 *
 *		Signal handler portion of interrupt handling. Let the backend know
 *		that there's a pending notify interrupt. If we're currently reading
 *		from the client, this will interrupt the read and
 *		ProcessClientReadInterrupt() will call ProcessNotifyInterrupt().
 */
void
HandleNotifyInterrupt(void)
{
	/*
	 * Note: this is called by a SIGNAL HANDLER. You must be very wary what
	 * you do here.
	 */

	/* signal that work needs to be done */
	notifyInterruptPending = true;

	/* make sure the event is processed in due course */
	SetLatch(MyLatch);
}

/*
 * ProcessNotifyInterrupt
 *
 *		This is called if we see notifyInterruptPending set, just before
 *		transmitting ReadyForQuery at the end of a frontend command, and
 *		also if a notify signal occurs while reading from the frontend.
 *		HandleNotifyInterrupt() will cause the read to be interrupted
 *		via the process's latch, and this routine will get called.
 *		If we are truly idle (ie, *not* inside a transaction block),
 *		process the incoming notifies.
 *
 *		If "flush" is true, force any frontend messages out immediately.
 *		This can be false when being called at the end of a frontend command,
 *		since we'll flush after sending ReadyForQuery.
 */
void
ProcessNotifyInterrupt(bool flush)
{
	if (IsTransactionOrTransactionBlock())
		return;					/* not really idle */

	/* Loop in case another signal arrives while sending messages */
	while (notifyInterruptPending)
		ProcessIncomingNotify(flush);
}


/*
 * Read all pending notifications from the queue, and deliver appropriate
 * ones to my frontend.  Stop when we reach queue head or an uncommitted
 * notification.
 */
static void
asyncQueueReadAllNotifications(void)
{
	volatile QueuePosition pos;
	QueuePosition head;
	Snapshot	snapshot;

	/* page_buffer must be adequately aligned, so use a union */
	union
	{
		char		buf[QUEUE_PAGESIZE];
		AsyncQueueEntry align;
	}			page_buffer;

	/*
	 * Fetch current state, indicate to others that we have woken up, and that
	 * we now will be advancing our position.
	 */
	LWLockAcquire(NotifyQueueLock, LW_SHARED);
	/* Assert checks that we have a valid state entry */
	Assert(MyProcPid == QUEUE_BACKEND_PID(MyProcNumber));
	QUEUE_BACKEND_WAKEUP_PENDING(MyProcNumber) = false;
	QUEUE_BACKEND_ADVANCING_POS(MyProcNumber) = true;
	pos = QUEUE_BACKEND_POS(MyProcNumber);
	head = QUEUE_HEAD;

	if (QUEUE_POS_EQUAL(pos, head))
	{
		/* Nothing to do, we have read all notifications already. */
		QUEUE_BACKEND_ADVANCING_POS(MyProcNumber) = false;
		LWLockRelease(NotifyQueueLock);
		return;
	}
	LWLockRelease(NotifyQueueLock);

	/*----------
	 * Get snapshot we'll use to decide which xacts are still in progress.
	 * This is trickier than it might seem, because of race conditions.
	 * Consider the following example:
	 *
	 * Backend 1:					 Backend 2:
	 *
	 * transaction starts
	 * UPDATE foo SET ...;
	 * NOTIFY foo;
	 * commit starts
	 * queue the notify message
	 *								 transaction starts
	 *								 LISTEN foo;  -- first LISTEN in session
	 *								 SELECT * FROM foo WHERE ...;
	 * commit to clog
	 *								 commit starts
	 *								 add backend 2 to array of listeners
	 *								 advance to queue head (this code)
	 *								 commit to clog
	 *
	 * Transaction 2's SELECT has not seen the UPDATE's effects, since that
	 * wasn't committed yet.  Ideally we'd ensure that client 2 would
	 * eventually get transaction 1's notify message, but there's no way
	 * to do that; until we're in the listener array, there's no guarantee
	 * that the notify message doesn't get removed from the queue.
	 *
	 * Therefore the coding technique transaction 2 is using is unsafe:
	 * applications must commit a LISTEN before inspecting database state,
	 * if they want to ensure they will see notifications about subsequent
	 * changes to that state.
	 *
	 * What we do guarantee is that we'll see all notifications from
	 * transactions committing after the snapshot we take here.
	 * Exec_ListenPreCommit has already added us to the listener array,
	 * so no not-yet-committed messages can be removed from the queue
	 * before we see them.
	 *----------
	 */
	snapshot = RegisterSnapshot(GetLatestSnapshot());

	/*
	 * It is possible that we fail while trying to send a message to our
	 * frontend (for example, because of encoding conversion failure).  If
	 * that happens it is critical that we not try to send the same message
	 * over and over again.  Therefore, we place a PG_TRY block here that will
	 * forcibly advance our queue position before we lose control to an error.
	 * (We could alternatively retake NotifyQueueLock and move the position
	 * before handling each individual message, but that seems like too much
	 * lock traffic.)
	 */
	PG_TRY();
	{
		bool		reachedStop;

		do
		{
			int64		curpage = QUEUE_POS_PAGE(pos);
			int			curoffset = QUEUE_POS_OFFSET(pos);
			int			slotno;
			int			copysize;

			/*
			 * We copy the data from SLRU into a local buffer, so as to avoid
			 * holding the SLRU lock while we are examining the entries and
			 * possibly transmitting them to our frontend.  Copy only the part
			 * of the page we will actually inspect.
			 */
			slotno = SimpleLruReadPage_ReadOnly(NotifyCtl, curpage,
												InvalidTransactionId);
			if (curpage == QUEUE_POS_PAGE(head))
			{
				/* we only want to read as far as head */
				copysize = QUEUE_POS_OFFSET(head) - curoffset;
				if (copysize < 0)
					copysize = 0;	/* just for safety */
			}
			else
			{
				/* fetch all the rest of the page */
				copysize = QUEUE_PAGESIZE - curoffset;
			}
			memcpy(page_buffer.buf + curoffset,
				   NotifyCtl->shared->page_buffer[slotno] + curoffset,
				   copysize);
			/* Release lock that we got from SimpleLruReadPage_ReadOnly() */
			LWLockRelease(SimpleLruGetBankLock(NotifyCtl, curpage));

			/*
			 * Process messages up to the stop position, end of page, or an
			 * uncommitted message.
			 *
			 * Our stop position is what we found to be the head's position
			 * when we entered this function. It might have changed already.
			 * But if it has, we will receive (or have already received and
			 * queued) another signal and come here again.
			 *
			 * We are not holding NotifyQueueLock here! The queue can only
			 * extend beyond the head pointer (see above) and we leave our
			 * backend's pointer where it is so nobody will truncate or
			 * rewrite pages under us. Especially we don't want to hold a lock
			 * while sending the notifications to the frontend.
			 */
			reachedStop = asyncQueueProcessPageEntries(&pos, head,
													   page_buffer.buf,
													   snapshot);
		} while (!reachedStop);
	}
	PG_FINALLY();
	{
		/* Update shared state */
		LWLockAcquire(NotifyQueueLock, LW_SHARED);

		QUEUE_BACKEND_ADVANCING_POS(MyProcNumber) = false;
		QUEUE_BACKEND_POS(MyProcNumber) = pos;

		LWLockRelease(NotifyQueueLock);
	}
	PG_END_TRY();

	/* Done with snapshot */
	UnregisterSnapshot(snapshot);
}

/*
 * Fetch notifications from the shared queue, beginning at position current,
 * and deliver relevant ones to my frontend.
 *
 * The current page must have been fetched into page_buffer from shared
 * memory.  (We could access the page right in shared memory, but that
 * would imply holding the SLRU bank lock throughout this routine.)
 *
 * We stop if we reach the "stop" position, or reach a notification from an
 * uncommitted transaction, or reach the end of the page.
 *
 * The function returns true once we have reached the stop position or an
 * uncommitted notification, and false if we have finished with the page.
 * In other words: once it returns true there is no need to look further.
 * The QueuePosition *current is advanced past all processed messages.
 */
static bool
asyncQueueProcessPageEntries(volatile QueuePosition *current,
							 QueuePosition stop,
							 char *page_buffer,
							 Snapshot snapshot)
{
	bool		reachedStop = false;
	bool		reachedEndOfPage;
	AsyncQueueEntry *qe;

	do
	{
		QueuePosition thisentry = *current;

		if (QUEUE_POS_EQUAL(thisentry, stop))
			break;

		qe = (AsyncQueueEntry *) (page_buffer + QUEUE_POS_OFFSET(thisentry));

		/*
		 * Advance *current over this message, possibly to the next page. As
		 * noted in the comments for asyncQueueReadAllNotifications, we must
		 * do this before possibly failing while processing the message.
		 */
		reachedEndOfPage = asyncQueueAdvance(current, qe->length);

		/* Ignore messages destined for other databases */
		if (qe->dboid == MyDatabaseId)
		{
			if (XidInMVCCSnapshot(qe->xid, snapshot))
			{
				/*
				 * The source transaction is still in progress, so we can't
				 * process this message yet.  Break out of the loop, but first
				 * back up *current so we will reprocess the message next
				 * time.  (Note: it is unlikely but not impossible for
				 * TransactionIdDidCommit to fail, so we can't really avoid
				 * this advance-then-back-up behavior when dealing with an
				 * uncommitted message.)
				 *
				 * Note that we must test XidInMVCCSnapshot before we test
				 * TransactionIdDidCommit, else we might return a message from
				 * a transaction that is not yet visible to snapshots; compare
				 * the comments at the head of heapam_visibility.c.
				 *
				 * Also, while our own xact won't be listed in the snapshot,
				 * we need not check for TransactionIdIsCurrentTransactionId
				 * because our transaction cannot (yet) have queued any
				 * messages.
				 */
				*current = thisentry;
				reachedStop = true;
				break;
			}
			else if (TransactionIdDidCommit(qe->xid))
			{
				/* qe->data is the null-terminated channel name */
				char	   *channel = qe->data;

				if (IsListeningOn(channel))
				{
					/* payload follows channel name */
					char	   *payload = qe->data + strlen(channel) + 1;

					NotifyMyFrontEnd(channel, payload, qe->srcPid);
				}
			}
			else
			{
				/*
				 * The source transaction aborted or crashed, so we just
				 * ignore its notifications.
				 */
			}
		}

		/* Loop back if we're not at end of page */
	} while (!reachedEndOfPage);

	if (QUEUE_POS_EQUAL(*current, stop))
		reachedStop = true;

	return reachedStop;
}

/*
 * Advance the shared queue tail variable to the minimum of all the
 * per-backend tail pointers.  Truncate pg_notify space if possible.
 *
 * This is (usually) called during CommitTransaction(), so it's important for
 * it to have very low probability of failure.
 */
static void
asyncQueueAdvanceTail(void)
{
	QueuePosition min;
	int64		oldtailpage;
	int64		newtailpage;
	int64		boundary;

	/* Restrict task to one backend per cluster; see SimpleLruTruncate(). */
	LWLockAcquire(NotifyQueueTailLock, LW_EXCLUSIVE);

	/*
	 * Compute the new tail.  Pre-v13, it's essential that QUEUE_TAIL be exact
	 * (ie, exactly match at least one backend's queue position), so it must
	 * be updated atomically with the actual computation.  Since v13, we could
	 * get away with not doing it like that, but it seems prudent to keep it
	 * so.
	 *
	 * Also, because incoming backends will scan forward from QUEUE_TAIL, that
	 * must be advanced before we can truncate any data.  Thus, QUEUE_TAIL is
	 * the logical tail, while QUEUE_STOP_PAGE is the physical tail, or oldest
	 * un-truncated page.  When QUEUE_STOP_PAGE != QUEUE_POS_PAGE(QUEUE_TAIL),
	 * there are pages we can truncate but haven't yet finished doing so.
	 *
	 * For concurrency's sake, we don't want to hold NotifyQueueLock while
	 * performing SimpleLruTruncate.  This is OK because no backend will try
	 * to access the pages we are in the midst of truncating.
	 */
	LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
	min = QUEUE_HEAD;
	for (ProcNumber i = QUEUE_FIRST_LISTENER; i != INVALID_PROC_NUMBER; i = QUEUE_NEXT_LISTENER(i))
	{
		Assert(QUEUE_BACKEND_PID(i) != InvalidPid);
		min = QUEUE_POS_MIN(min, QUEUE_BACKEND_POS(i));
	}
	QUEUE_TAIL = min;
	oldtailpage = QUEUE_STOP_PAGE;
	LWLockRelease(NotifyQueueLock);

	/*
	 * We can truncate something if the global tail advanced across an SLRU
	 * segment boundary.
	 *
	 * XXX it might be better to truncate only once every several segments, to
	 * reduce the number of directory scans.
	 */
	newtailpage = QUEUE_POS_PAGE(min);
	boundary = newtailpage - (newtailpage % SLRU_PAGES_PER_SEGMENT);
	if (asyncQueuePagePrecedes(oldtailpage, boundary))
	{
		/*
		 * SimpleLruTruncate() will ask for SLRU bank locks but will also
		 * release the lock again.
		 */
		SimpleLruTruncate(NotifyCtl, newtailpage);

		LWLockAcquire(NotifyQueueLock, LW_EXCLUSIVE);
		QUEUE_STOP_PAGE = newtailpage;
		LWLockRelease(NotifyQueueLock);
	}

	LWLockRelease(NotifyQueueTailLock);
}

/*
 * ProcessIncomingNotify
 *
 *		Scan the queue for arriving notifications and report them to the front
 *		end.  The notifications might be from other sessions, or our own;
 *		there's no need to distinguish here.
 *
 *		If "flush" is true, force any frontend messages out immediately.
 *
 *		NOTE: since we are outside any transaction, we must create our own.
 */
static void
ProcessIncomingNotify(bool flush)
{
	/* We *must* reset the flag */
	notifyInterruptPending = false;

	/* Do nothing else if we aren't actively listening */
	if (listenChannelsHash == NULL || hash_get_num_entries(listenChannelsHash) == 0)
		return;

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify");

	set_ps_display("notify interrupt");

	/*
	 * We must run asyncQueueReadAllNotifications inside a transaction, else
	 * bad things happen if it gets an error.
	 */
	StartTransactionCommand();

	asyncQueueReadAllNotifications();

	CommitTransactionCommand();

	/*
	 * If this isn't an end-of-command case, we must flush the notify messages
	 * to ensure frontend gets them promptly.
	 */
	if (flush)
		pq_flush();

	set_ps_display("idle");

	if (Trace_notify)
		elog(DEBUG1, "ProcessIncomingNotify: done");
}

/*
 * Send NOTIFY message to my front end.
 */
void
NotifyMyFrontEnd(const char *channel, const char *payload, int32 srcPid)
{
	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, PqMsg_NotificationResponse);
		pq_sendint32(&buf, srcPid);
		pq_sendstring(&buf, channel);
		pq_sendstring(&buf, payload);
		pq_endmessage(&buf);

		/*
		 * NOTE: we do not do pq_flush() here.  Some level of caller will
		 * handle it later, allowing this message to be combined into a packet
		 * with other ones.
		 */
	}
	else
		elog(INFO, "NOTIFY for \"%s\" payload \"%s\"", channel, payload);
}

/* Does pendingNotifies include a match for the given event? */
static bool
AsyncExistsPendingNotify(Notification *n)
{
	if (pendingNotifies == NULL)
		return false;

	if (pendingNotifies->hashtab != NULL)
	{
		/* Use the hash table to probe for a match */
		if (hash_search(pendingNotifies->hashtab,
						&n,
						HASH_FIND,
						NULL))
			return true;
	}
	else
	{
		/* Must scan the event list */
		ListCell   *l;

		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);

			if (n->channel_len == oldn->channel_len &&
				n->payload_len == oldn->payload_len &&
				memcmp(n->data, oldn->data,
					   n->channel_len + n->payload_len + 2) == 0)
				return true;
		}
	}

	return false;
}

/*
 * Add a notification event to a pre-existing pendingNotifies list.
 *
 * Because pendingNotifies->events is already nonempty, this works
 * correctly no matter what CurrentMemoryContext is.
 */
static void
AddEventToPendingNotifies(Notification *n)
{
	Assert(pendingNotifies->events != NIL);

	/* Create the hash tables if it's time to */
	if (list_length(pendingNotifies->events) >= MIN_HASHABLE_NOTIFIES &&
		pendingNotifies->hashtab == NULL)
	{
		HASHCTL		hash_ctl;
		ListCell   *l;

		Assert(pendingNotifies->channelHashtab == NULL);

		/* Create the hash table */
		hash_ctl.keysize = sizeof(Notification *);
		hash_ctl.entrysize = sizeof(struct NotificationHash);
		hash_ctl.hash = notification_hash;
		hash_ctl.match = notification_match;
		hash_ctl.hcxt = CurTransactionContext;
		pendingNotifies->hashtab =
			hash_create("Pending Notifies",
						256L,
						&hash_ctl,
						HASH_ELEM | HASH_FUNCTION | HASH_COMPARE | HASH_CONTEXT);

		/* Create the channel hash table */
		memset(&hash_ctl, 0, sizeof(hash_ctl));
		hash_ctl.keysize = NAMEDATALEN;
		hash_ctl.entrysize = sizeof(struct ChannelHash);
		hash_ctl.hcxt = CurTransactionContext;
		pendingNotifies->channelHashtab =
			hash_create("Pending Notify Channels",
						64L,
						&hash_ctl,
						HASH_ELEM | HASH_STRINGS | HASH_CONTEXT);

		/* Insert all the already-existing events */
		foreach(l, pendingNotifies->events)
		{
			Notification *oldn = (Notification *) lfirst(l);
			char	   *channel = oldn->data;
			bool		found;

			(void) hash_search(pendingNotifies->hashtab,
							   &oldn,
							   HASH_ENTER,
							   &found);
			Assert(!found);

			/* Insert channel into channelHashtab */
			(void) hash_search(pendingNotifies->channelHashtab,
							   channel,
							   HASH_ENTER,
							   &found);
			/* found may be true if multiple events on same channel */
		}
	}

	/* Add new event to the list, in order */
	pendingNotifies->events = lappend(pendingNotifies->events, n);

	/* Add event to the hash tables if needed */
	if (pendingNotifies->hashtab != NULL)
	{
		bool		found;

		Assert(pendingNotifies->channelHashtab != NULL);

		(void) hash_search(pendingNotifies->hashtab,
						   &n,
						   HASH_ENTER,
						   &found);
		Assert(!found);

		/* Add channel to channelHashtab */
		{
			char	   *channel = n->data;

			(void) hash_search(pendingNotifies->channelHashtab,
							   channel,
							   HASH_ENTER,
							   &found);
			/* found may be true if we already have an event on this channel */
		}
	}
}

/*
 * notification_hash: hash function for notification hash table
 *
 * The hash "keys" are pointers to Notification structs.
 */
static uint32
notification_hash(const void *key, Size keysize)
{
	const Notification *k = *(const Notification *const *) key;

	Assert(keysize == sizeof(Notification *));
	/* We don't bother to include the payload's trailing null in the hash */
	return DatumGetUInt32(hash_any((const unsigned char *) k->data,
								   k->channel_len + k->payload_len + 1));
}

/*
 * notification_match: match function to use with notification_hash
 */
static int
notification_match(const void *key1, const void *key2, Size keysize)
{
	const Notification *k1 = *(const Notification *const *) key1;
	const Notification *k2 = *(const Notification *const *) key2;

	Assert(keysize == sizeof(Notification *));
	if (k1->channel_len == k2->channel_len &&
		k1->payload_len == k2->payload_len &&
		memcmp(k1->data, k2->data,
			   k1->channel_len + k1->payload_len + 2) == 0)
		return 0;				/* equal */
	return 1;					/* not equal */
}

/* Clear the pendingActions, pendingNotifies, and pendingNotifyChannels lists. */
static void
ClearPendingActionsAndNotifies(void)
{
	/*
	 * Everything's allocated in either TopTransactionContext or the context
	 * for the subtransaction to which it corresponds.  So, there's nothing to
	 * do here except reset the pointers; the space will be reclaimed when the
	 * contexts are deleted.
	 */
	pendingActions = NULL;
	pendingNotifies = NULL;
	pendingNotifyChannels = NIL;
}

/*
 * GUC check_hook for notify_buffers
 */
bool
check_notify_buffers(int *newval, void **extra, GucSource source)
{
	return check_slru_buffers("notify_buffers", newval);
}


/*
 * ChannelHashPrepareKey
 *		Prepare a channel key for use as a hash key.
 */
static inline void
ChannelHashPrepareKey(ChannelHashKey *key, Oid dboid, const char *channel)
{
	memset(key, 0, sizeof(ChannelHashKey));
	key->dboid = dboid;
	strlcpy(key->channel, channel, NAMEDATALEN);
}
