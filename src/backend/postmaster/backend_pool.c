/*-------------------------------------------------------------------------
 *
 * backend_pool.c
 *	  Backend connection pooling - shared memory pool and FD passing.
 *
 * This module manages a pool of backend processes that can be reused
 * across client connections.  When a client disconnects, the backend
 * enters a "pooled" state.  When a new client connects, the postmaster
 * can send the client socket to a pooled backend instead of forking.
 *
 * FD passing between postmaster and backend uses Unix domain socketpairs
 * with SCM_RIGHTS ancillary messages.
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/backend_pool.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "libpq/pqcomm.h"
#include "miscadmin.h"
#include "port/pg_bswap.h"
#include "postmaster/backend_pool.h"
#include "storage/shmem.h"

/* Shared memory pointer */
static BackendPool *Pool = NULL;

/* Backend's end of the socketpair */
pgsocket	MyPoolSocket = PGINVALID_SOCKET;


/*
 * BackendPoolShmemSize
 *		Compute shared memory space needed for the backend pool.
 */
Size
BackendPoolShmemSize(void)
{
	Size		size;

	size = offsetof(BackendPool, slots);
	size = add_size(size, mul_size(MaxConnections, sizeof(BackendPoolSlot)));

	return size;
}

/*
 * BackendPoolShmemInit
 *		Allocate and initialize backend pool shared memory.
 */
void
BackendPoolShmemInit(void)
{
	bool		found;

	Pool = (BackendPool *)
		ShmemInitStruct("Backend Pool",
						BackendPoolShmemSize(),
						&found);

	if (!found)
	{
		/* First time through, so initialize */
		Pool->maxSlots = MaxConnections;
		SpinLockInit(&Pool->mutex);
		memset(Pool->slots, 0, MaxConnections * sizeof(BackendPoolSlot));

		for (int i = 0; i < MaxConnections; i++)
		{
			Pool->slots[i].pid = 0;
			Pool->slots[i].status = BPSLOT_UNUSED;
			Pool->slots[i].postmasterSock = PGINVALID_SOCKET;
		}
	}
}

/*
 * BackendPoolRegister
 *		Register a newly forked backend in the pool as ACTIVE.
 *
 * Called from the postmaster after a successful fork.
 */
void
BackendPoolRegister(pid_t pid, ProcNumber procNumber,
					Oid dbId, pgsocket pmSock)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].status == BPSLOT_UNUSED)
		{
			Pool->slots[i].pid = pid;
			Pool->slots[i].procNumber = procNumber;
			Pool->slots[i].databaseId = dbId;
			Pool->slots[i].postmasterSock = pmSock;
			Pool->slots[i].status = BPSLOT_ACTIVE;
			SpinLockRelease(&Pool->mutex);
			return;
		}
	}

	SpinLockRelease(&Pool->mutex);
	elog(WARNING, "backend pool: no free slots for pid %d", (int) pid);
}

/*
 * BackendPoolMarkPooled
 *		Mark a backend as pooled (available for reuse).
 *
 * Called from the backend process when it enters the pooled state.
 * dbname is the name of the database this backend is connected to,
 * used for matching incoming connections.
 *
 * Returns true if the backend was successfully marked as pooled.
 * Returns false if the pool is too full -- the caller should exit
 * instead of entering the pooled wait loop.  We limit pooled backends
 * to MaxConnections / 4 to ensure PGPROC slots remain available for
 * newly forked backends that can't be matched to a pooled one.
 */
bool
BackendPoolMarkPooled(pid_t pid, const char *dbname)
{
	int			pooled_count = 0;

	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].status == BPSLOT_POOLED ||
			Pool->slots[i].status == BPSLOT_REASSIGNING)
			pooled_count++;
	}

	/* Leave room for new connections that can't reuse a pooled backend */
	if (pooled_count >= Max(Pool->maxSlots / 4, 1))
	{
		SpinLockRelease(&Pool->mutex);
		return false;
	}

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].pid == pid &&
			Pool->slots[i].status == BPSLOT_ACTIVE)
		{
			/*
			 * If databaseId was cleared by BackendPoolEvictDatabase(), the
			 * database was dropped while we were cleaning up.  Don't enter
			 * the pool -- the caller should exit instead.
			 */
			if (!OidIsValid(Pool->slots[i].databaseId))
			{
				SpinLockRelease(&Pool->mutex);
				return false;
			}
			Pool->slots[i].status = BPSLOT_POOLED;
			strlcpy(Pool->slots[i].databaseName, dbname, NAMEDATALEN);
			SpinLockRelease(&Pool->mutex);
			return true;
		}
	}

	SpinLockRelease(&Pool->mutex);
	return false;
}

/*
 * BackendPoolMarkActive
 *		Mark a backend as active (serving a client).
 *
 * Called from the backend process when it receives a new client.
 */
void
BackendPoolMarkActive(pid_t pid)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].pid == pid)
		{
			Pool->slots[i].status = BPSLOT_ACTIVE;
			SpinLockRelease(&Pool->mutex);
			return;
		}
	}

	SpinLockRelease(&Pool->mutex);
}

/*
 * BackendPoolRemove
 *		Remove a backend from the pool (on exit or crash).
 *
 * Also closes the postmaster's end of the socketpair.
 * Called from the postmaster in CleanupBackend.
 */
void
BackendPoolRemove(pid_t pid)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].pid == pid)
		{
			if (Pool->slots[i].postmasterSock != PGINVALID_SOCKET)
			{
				closesocket(Pool->slots[i].postmasterSock);
				Pool->slots[i].postmasterSock = PGINVALID_SOCKET;
			}
			Pool->slots[i].pid = 0;
			Pool->slots[i].status = BPSLOT_UNUSED;
			SpinLockRelease(&Pool->mutex);
			return;
		}
	}

	SpinLockRelease(&Pool->mutex);
}

/*
 * BackendPoolShutdown
 *		Close all pool socketpairs and signal pooled backends to exit.
 *
 * Called from the postmaster during smart shutdown.  Closing the
 * socketpair causes WaitForNewClient() to see EOF and return false,
 * which leads the pooled backend to exit.
 */
void
BackendPoolShutdown(void)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].status == BPSLOT_POOLED)
		{
			/* Close the postmaster's end of the socketpair */
			if (Pool->slots[i].postmasterSock != PGINVALID_SOCKET)
			{
				closesocket(Pool->slots[i].postmasterSock);
				Pool->slots[i].postmasterSock = PGINVALID_SOCKET;
			}

			/* Also send SIGTERM so the backend's latch fires */
			if (Pool->slots[i].pid != 0)
				kill(Pool->slots[i].pid, SIGTERM);
		}
	}

	SpinLockRelease(&Pool->mutex);
}

/*
 * BackendPoolUpdateDatabaseId
 *		Update the database OID stored for a backend.
 *
 * Called from the backend after connecting to a database.
 */
void
BackendPoolUpdateDatabaseId(pid_t pid, Oid dbId)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].pid == pid)
		{
			Pool->slots[i].databaseId = dbId;
			SpinLockRelease(&Pool->mutex);
			return;
		}
	}

	SpinLockRelease(&Pool->mutex);
}

/*
 * BackendPoolEvictDatabase
 *		Evict all pooled backends connected to the given database.
 *
 * Sends SIGTERM to each matching pooled backend and clears its database
 * name so the postmaster won't assign new connections to it.  The
 * backend will see ProcDiePending in its WaitForNewClient() loop and
 * exit.  The postmaster will clean up the socketpair FD and PGPROC
 * slot via CleanupBackend() when it detects the backend's exit.
 *
 * Note: we cannot close the postmaster's socketpair FD from here because
 * this function runs in a backend process where those FD numbers refer to
 * unrelated files.  The postmaster-side cleanup happens when
 * BackendPoolRemove() runs from CleanupBackend().
 *
 * This must be called before CountOtherDBBackends() during DROP DATABASE,
 * because pooled backends clear their MyProc->databaseId to InvalidOid
 * (to avoid blocking DROP DATABASE) and would not be counted.  Without
 * eviction, those backends would be assigned to a new connection for the
 * recreated database (same name, different OID) and FATAL when they
 * discover their database OID no longer exists.
 */
void
BackendPoolEvictDatabase(Oid dbId)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].databaseId != dbId)
			continue;

		if (Pool->slots[i].status == BPSLOT_POOLED)
		{
			/*
			 * Mark the slot as REASSIGNING so BackendPoolAssignConnection
			 * won't match it to new connections.  Clear the database name
			 * and OID for good measure.  Send SIGTERM so the backend's
			 * WaitForNewClient() loop sees ProcDiePending and exits.
			 *
			 * We use BPSLOT_REASSIGNING rather than BPSLOT_UNUSED because
			 * BackendPoolRemove() still needs to find this slot by PID to
			 * close the postmaster's socketpair FD.
			 */
			Pool->slots[i].status = BPSLOT_REASSIGNING;
			Pool->slots[i].databaseName[0] = '\0';
			Pool->slots[i].databaseId = InvalidOid;
			if (Pool->slots[i].pid != 0)
				kill(Pool->slots[i].pid, SIGTERM);
		}
		else if (Pool->slots[i].status == BPSLOT_ACTIVE)
		{
			/*
			 * The backend is still active (likely in the cleanup phase of
			 * BackendEnterPooledState).  Clear its databaseId so that
			 * BackendPoolMarkPooled() will refuse to pool it.  We can't
			 * SIGTERM an active backend -- it may be serving a client --
			 * but clearing the databaseId ensures it exits instead of
			 * entering the pool with a stale database reference.
			 */
			Pool->slots[i].databaseId = InvalidOid;
		}
	}

	SpinLockRelease(&Pool->mutex);
}

/*
 * PeekStartupDatabase
 *		Peek at the startup packet on a client socket to extract the
 *		requested database name, without consuming the data.
 *
 * Returns true if the database name was successfully extracted.
 * Returns false if the packet can't be peeked (SSL/GSS negotiation,
 * cancel request, or unreadable).
 */
static bool
PeekStartupDatabase(pgsocket sock, char *dbname, size_t dbname_size)
{
	char		buf[1024];
	ssize_t		n;
	uint32		len;
	uint32		proto;
	char	   *p;
	char	   *end;
	const char *user = NULL;
	bool		found_db = false;
	bool		is_replication = false;

	dbname[0] = '\0';

	/*
	 * Use non-blocking mode so we never stall the postmaster's main loop.
	 * If the client hasn't sent a startup packet yet, we'll get EAGAIN and
	 * fall through to fork a new backend normally.
	 */
	if (!pg_set_noblock(sock))
		return false;

	n = recv(sock, buf, sizeof(buf), MSG_PEEK);

	if (!pg_set_block(sock))
		elog(LOG, "backend pool: could not restore blocking mode on socket");

	if (n < 8)
		return false;

	memcpy(&len, buf, 4);
	len = pg_ntoh32(len);
	memcpy(&proto, buf + 4, 4);
	proto = pg_ntoh32(proto);

	/* Can't determine DB for SSL, GSS, or cancel requests */
	if (proto == NEGOTIATE_SSL_CODE || proto == NEGOTIATE_GSS_CODE)
		return false;
	if (proto == CANCEL_REQUEST_CODE)
		return false;

	/* Parse key-value pairs from the startup packet */
	p = buf + 8;
	end = buf + Min(n, (ssize_t) len);

	while (p < end && *p != '\0')
	{
		char	   *key = p;
		size_t		keylen = strnlen(key, end - p);

		if (key + keylen >= end)
			break;
		p = key + keylen + 1;

		{
			char	   *val = p;
			size_t		vallen = strnlen(val, end - p);

			if (val + vallen >= end)
				break;
			p = val + vallen + 1;

			if (strcmp(key, "database") == 0)
			{
				strlcpy(dbname, val, dbname_size);
				found_db = true;
			}
			else if (strcmp(key, "user") == 0)
				user = val;
			else if (strcmp(key, "replication") == 0)
				is_replication = true;
		}
	}

	/* Replication connections must not be sent to pooled backends */
	if (is_replication)
		return false;

	if (found_db)
		return true;

	/*
	 * If no "database" key, PostgreSQL defaults to the user name.
	 */
	if (user != NULL)
	{
		strlcpy(dbname, user, dbname_size);
		return true;
	}

	return false;
}

/*
 * BackendPoolAssignConnection
 *		Try to assign a new client connection to a pooled backend.
 *
 * Returns true if a pooled backend was found and the socket was sent.
 * Returns false if no pooled backend is available (caller should fork).
 *
 * Peeks at the client's startup packet to determine the requested database,
 * then only assigns to a pooled backend connected to the same database.
 *
 * Called from the postmaster in ServerLoop.
 */
bool
BackendPoolAssignConnection(ClientSocket *client_sock)
{
	int			found = -1;
	char		dbname[NAMEDATALEN];

	/*
	 * Peek at the startup packet to determine which database the client
	 * wants.  If we can't determine it (SSL negotiation, cancel request,
	 * etc.), fall through to fork a new backend.
	 */
	if (!PeekStartupDatabase(client_sock->sock, dbname, sizeof(dbname)))
		return false;

	SpinLockAcquire(&Pool->mutex);

	/*
	 * Find a pooled backend for the same database.  Use LIFO order (scan
	 * backwards) to prefer the most recently pooled backend, which likely
	 * has warm caches.
	 */
	for (int i = Pool->maxSlots - 1; i >= 0; i--)
	{
		if (Pool->slots[i].status == BPSLOT_POOLED &&
			strcmp(Pool->slots[i].databaseName, dbname) == 0)
		{
			found = i;
			break;
		}
	}

	if (found >= 0)
	{
		Pool->slots[found].status = BPSLOT_REASSIGNING;
		SpinLockRelease(&Pool->mutex);

		/* Send the client socket to the pooled backend */
		if (BackendPoolSendSocket(Pool->slots[found].postmasterSock,
								  client_sock) != 0)
		{
			/* Failed to send; mark back as pooled */
			elog(LOG, "backend pool: failed to send socket to pid %d",
				 (int) Pool->slots[found].pid);
			SpinLockAcquire(&Pool->mutex);
			Pool->slots[found].status = BPSLOT_POOLED;
			SpinLockRelease(&Pool->mutex);
			return false;
		}

		elog(DEBUG2, "backend pool: assigned connection to pooled backend pid %d (db=%s)",
			 (int) Pool->slots[found].pid, dbname);
		return true;
	}

	SpinLockRelease(&Pool->mutex);
	return false;
}

/*
 * BackendPoolSendSocket
 *		Send a client socket FD to a backend via the socketpair.
 *
 * Uses sendmsg() with SCM_RIGHTS to pass the file descriptor.
 * The ClientSocket struct (raddr) is sent as the message payload.
 */
int
BackendPoolSendSocket(pgsocket pairEnd, ClientSocket *clientSock)
{
	struct msghdr msg;
	struct iovec iov[1];
	struct cmsghdr *cmsg;

	/*
	 * We need a control message buffer large enough for one file descriptor.
	 */
	union
	{
		struct cmsghdr cm;
		char		buf[CMSG_SPACE(sizeof(int))];
	}			cmsg_buf;

	memset(&msg, 0, sizeof(msg));
	memset(&cmsg_buf, 0, sizeof(cmsg_buf));

	/* The regular message payload carries the SockAddr */
	iov[0].iov_base = &clientSock->raddr;
	iov[0].iov_len = sizeof(clientSock->raddr);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	/* Set up the control message with SCM_RIGHTS */
	msg.msg_control = &cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &clientSock->sock, sizeof(int));

	for (;;)
	{
		ssize_t		rc = sendmsg(pairEnd, &msg, 0);

		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			elog(LOG, "backend pool: sendmsg failed: %m");
			return -1;
		}
		break;
	}

	return 0;
}

/*
 * BackendPoolRecvSocket
 *		Receive a client socket FD from the postmaster via the socketpair.
 *
 * Uses recvmsg() with SCM_RIGHTS to receive the file descriptor.
 * The ClientSocket struct (raddr) is received as the message payload.
 */
int
BackendPoolRecvSocket(pgsocket pairEnd, ClientSocket *clientSock)
{
	struct msghdr msg;
	struct iovec iov[1];
	struct cmsghdr *cmsg;
	ssize_t		rc;

	union
	{
		struct cmsghdr cm;
		char		buf[CMSG_SPACE(sizeof(int))];
	}			cmsg_buf;

	memset(&msg, 0, sizeof(msg));
	memset(&cmsg_buf, 0, sizeof(cmsg_buf));
	memset(clientSock, 0, sizeof(ClientSocket));

	iov[0].iov_base = &clientSock->raddr;
	iov[0].iov_len = sizeof(clientSock->raddr);
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;

	msg.msg_control = &cmsg_buf;
	msg.msg_controllen = sizeof(cmsg_buf);

	for (;;)
	{
		rc = recvmsg(pairEnd, &msg, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			elog(LOG, "backend pool: recvmsg failed: %m");
			return -1;
		}
		break;
	}

	if (rc == 0)
	{
		elog(LOG, "backend pool: socketpair closed");
		return -1;
	}

	/* Extract the file descriptor from the control message */
	cmsg = CMSG_FIRSTHDR(&msg);
	if (cmsg == NULL ||
		cmsg->cmsg_level != SOL_SOCKET ||
		cmsg->cmsg_type != SCM_RIGHTS ||
		cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
	{
		elog(LOG, "backend pool: invalid control message");
		return -1;
	}

	memcpy(&clientSock->sock, CMSG_DATA(cmsg), sizeof(int));

	return 0;
}
