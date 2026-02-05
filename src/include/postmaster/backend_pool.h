/*-------------------------------------------------------------------------
 *
 * backend_pool.h
 *	  Definitions for backend connection pooling (backend reuse).
 *
 * When a client disconnects, the backend process can enter a "pooled"
 * state instead of exiting.  The postmaster can then assign a new client
 * connection to the pooled backend, avoiding the cost of fork().
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * src/include/postmaster/backend_pool.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKEND_POOL_H
#define BACKEND_POOL_H

#include "libpq/libpq-be.h"
#include "storage/procnumber.h"
#include "storage/shmem.h"
#include "storage/spin.h"

/*
 * Status of a backend pool slot.
 */
typedef enum BackendPoolSlotStatus
{
	BPSLOT_UNUSED = 0,			/* slot not in use */
	BPSLOT_ACTIVE,				/* backend is serving a client */
	BPSLOT_POOLED,				/* backend is available for reuse */
	BPSLOT_REASSIGNING			/* postmaster sent socket, backend reconnecting */
} BackendPoolSlotStatus;

/*
 * Per-backend slot in the shared memory pool.
 */
typedef struct BackendPoolSlot
{
	pid_t		pid;
	ProcNumber	procNumber;
	Oid			databaseId;		/* last connected database */
	pgsocket	postmasterSock;	/* postmaster's end of socketpair */
	sig_atomic_t status;		/* BackendPoolSlotStatus */
} BackendPoolSlot;

/*
 * Shared memory structure for the backend pool.
 */
typedef struct BackendPool
{
	int			maxSlots;		/* = MaxConnections */
	slock_t		mutex;
	BackendPoolSlot slots[FLEXIBLE_ARRAY_MEMBER];
} BackendPool;

/* Global: backend's end of the socketpair with postmaster */
extern PGDLLIMPORT pgsocket MyPoolSocket;

/* Shared memory setup (called from ipci.c) */
extern Size BackendPoolShmemSize(void);
extern void BackendPoolShmemInit(void);

/* Pool management (called from postmaster) */
extern void BackendPoolRegister(pid_t pid, ProcNumber procNumber,
								Oid dbId, pgsocket pmSock);
extern void BackendPoolMarkPooled(pid_t pid);
extern void BackendPoolMarkActive(pid_t pid);
extern void BackendPoolRemove(pid_t pid);
extern void BackendPoolUpdateDatabaseId(pid_t pid, Oid dbId);
extern bool BackendPoolAssignConnection(ClientSocket *client_sock);

/* FD passing */
extern int	BackendPoolSendSocket(pgsocket pairEnd, ClientSocket *clientSock);
extern int	BackendPoolRecvSocket(pgsocket pairEnd, ClientSocket *clientSock);

#endif							/* BACKEND_POOL_H */
