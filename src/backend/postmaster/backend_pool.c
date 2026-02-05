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

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "miscadmin.h"
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
 */
void
BackendPoolMarkPooled(pid_t pid)
{
	SpinLockAcquire(&Pool->mutex);

	for (int i = 0; i < Pool->maxSlots; i++)
	{
		if (Pool->slots[i].pid == pid &&
			Pool->slots[i].status == BPSLOT_ACTIVE)
		{
			Pool->slots[i].status = BPSLOT_POOLED;
			SpinLockRelease(&Pool->mutex);
			return;
		}
	}

	SpinLockRelease(&Pool->mutex);
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
 * BackendPoolAssignConnection
 *		Try to assign a new client connection to a pooled backend.
 *
 * Returns true if a pooled backend was found and the socket was sent.
 * Returns false if no pooled backend is available (caller should fork).
 *
 * Called from the postmaster in ServerLoop.
 */
bool
BackendPoolAssignConnection(ClientSocket *client_sock)
{
	int			found = -1;

	SpinLockAcquire(&Pool->mutex);

	/*
	 * Find a pooled backend.  Use LIFO order (scan backwards) to prefer the
	 * most recently pooled backend, which likely has warm caches for the most
	 * common workload.
	 */
	for (int i = Pool->maxSlots - 1; i >= 0; i--)
	{
		if (Pool->slots[i].status == BPSLOT_POOLED)
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

		elog(DEBUG2, "backend pool: assigned connection to pooled backend pid %d",
			 (int) Pool->slots[found].pid);
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
