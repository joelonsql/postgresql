/*-------------------------------------------------------------------------
 *
 * backend_reuse.c
 *	  Backend connection reuse (pooling) logic.
 *
 * When a client disconnects, instead of exiting, the backend enters a
 * "pooled" state: it cleans up the session, closes the client socket,
 * and waits on the socketpair for a new client socket from the postmaster.
 *
 * Copyright (c) 1996-2026, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/tcop/backend_reuse.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#include "access/xact.h"
#include "catalog/namespace.h"
#include "commands/async.h"
#include "commands/prepare.h"
#include "commands/sequence.h"
#include "common/ip.h"
#include "libpq/auth.h"
#include "libpq/hba.h"
#include "libpq/libpq.h"
#include "libpq/libpq-be.h"
#include "libpq/pqformat.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/backend_pool.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/proc.h"
#include "storage/procsignal.h"
#include "storage/sinvaladt.h"
#include "tcop/backend_reuse.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

/* These functions are declared here because they are not in public headers */
extern int	ProcessSSLStartup(Port *port);
extern int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
extern void PerformAuthentication(Port *port);
extern void process_startup_options(Port *port, bool am_superuser);
extern void process_settings(Oid databaseid, Oid roleid);

/*
 * Close the current client socket and clean up libpq state.
 * Used when entering the pooled state or when rejecting a mismatched client.
 */
static void
CloseClientSocket(void)
{
	if (MyProcPort->sock != PGINVALID_SOCKET)
	{
		/* Shut down SSL/GSS if active */
		secure_close(MyProcPort);

		closesocket(MyProcPort->sock);
		MyProcPort->sock = PGINVALID_SOCKET;
		ReleaseExternalFD();
	}

	/* Free the WaitEventSet that references the old socket */
	if (FeBeWaitSet != NULL)
	{
		FreeWaitEventSet(FeBeWaitSet);
		FeBeWaitSet = NULL;
	}

	whereToSendOutput = DestNone;
}

/*
 * Wait for a new client socket from the postmaster on the pool socketpair.
 *
 * Returns true if a new client socket was received.
 * Returns false if we should exit (postmaster death, shutdown signal, etc.)
 *
 * On success, the new client socket is stored in *newClientSocket.
 */
static bool
WaitForNewClient(ClientSocket *newClientSocket)
{
	WaitEventSet *waitSet;
	WaitEvent	event;

	waitSet = CreateWaitEventSet(NULL, 3);
	AddWaitEventToSet(waitSet, WL_SOCKET_READABLE, MyPoolSocket, NULL, NULL);
	AddWaitEventToSet(waitSet, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);
	AddWaitEventToSet(waitSet, WL_POSTMASTER_DEATH, PGINVALID_SOCKET, NULL, NULL);

	for (;;)
	{
		int			rc;

		/* Check for signals */
		if (ProcDiePending)
		{
			FreeWaitEventSet(waitSet);
			return false;
		}

		/* Process config file reload if requested */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Accept invalidation messages so we don't block the sinval queue */
		AcceptInvalidationMessages();

		rc = WaitEventSetWait(waitSet, 10000 /* 10s */, &event, 1,
							  WAIT_EVENT_CLIENT_READ);

		if (rc == 0)
			continue;			/* Timeout -- loop and recheck signals */

		if (event.events & WL_POSTMASTER_DEATH)
		{
			FreeWaitEventSet(waitSet);
			return false;
		}

		if (event.events & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);
			continue;			/* Loop to check signals above */
		}

		if (event.events & WL_SOCKET_READABLE)
			break;				/* New client incoming */
	}

	FreeWaitEventSet(waitSet);

	/* Receive the new client socket */
	if (BackendPoolRecvSocket(MyPoolSocket, newClientSocket) != 0)
		return false;

	return true;
}

/*
 * Set up the new client socket on our Port and resolve remote address.
 */
static void
AcceptNewClient(ClientSocket *newClientSocket)
{
	pq_reinit(newClientSocket);

	/* Resolve remote host/port for logging */
	{
		char		remote_host[NI_MAXHOST];
		char		remote_port[NI_MAXSERV];

		remote_host[0] = '\0';
		remote_port[0] = '\0';
		pg_getnameinfo_all(&MyProcPort->raddr.addr, MyProcPort->raddr.salen,
						   remote_host, sizeof(remote_host),
						   remote_port, sizeof(remote_port),
						   NI_NUMERICHOST | NI_NUMERICSERV);

		if (MyProcPort->remote_host)
			pfree(MyProcPort->remote_host);
		MyProcPort->remote_host = MemoryContextStrdup(TopMemoryContext, remote_host);
		if (MyProcPort->remote_port)
			pfree(MyProcPort->remote_port);
		MyProcPort->remote_port = MemoryContextStrdup(TopMemoryContext, remote_port);
	}
}

/*
 * BackendEnterPooledState
 *
 * Called when the client disconnects (EOF or Terminate message).
 * Cleans up the session, enters pooled state, and waits for a new client.
 *
 * Returns true if a new client was successfully connected (caller should
 * resume the main query loop).  Returns false if the backend should exit
 * (postmaster death, shutdown signal, etc.).
 */
bool
BackendEnterPooledState(void)
{
	ClientSocket newClientSocket;

	/*
	 * Step 1: Session cleanup (equivalent to DISCARD ALL).
	 *
	 * First abort any open transaction, then do cleanup that doesn't need
	 * catalog access.
	 */
	AbortOutOfAnyTransaction();
	PortalHashTableDeleteAll();
	DropAllPreparedStatements();
	Async_UnlistenAll();
	LockReleaseAll(USER_LOCKMETHOD, true);
	ResetAllOptions();
	ResetPlanCache();
	ResetSequenceCaches();

	/*
	 * ResetTempTableNamespace() needs catalog access (it drops temp tables),
	 * so it must run inside a transaction with an active snapshot.
	 */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	ResetTempTableNamespace();
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * Step 2: Close the client socket.
	 */
	CloseClientSocket();

	/*
	 * Step 3: Update shared state to indicate we're pooled.
	 */
	MyProc->roleId = InvalidOid;
	pgstat_report_activity(STATE_POOLED, NULL);
	set_ps_display("pooled");
	BackendPoolMarkPooled(MyProcPid);

	/*
	 * Step 4: Wait for a new client, process it, and loop back if needed
	 * (e.g., if the client requests a different database).
	 */
	for (;;)
	{
		int			status;
		bool		need_db_switch;
		bool		am_superuser;

		if (!WaitForNewClient(&newClientSocket))
			return false;

		BackendPoolMarkActive(MyProcPid);

		/*
		 * Step 5: Reinitialize the connection with the new client socket.
		 */
		AcceptNewClient(&newClientSocket);

		/*
		 * Process SSL/GSS handshake and startup packet.
		 */
		status = ProcessSSLStartup(MyProcPort);
		if (status == STATUS_OK)
			status = ProcessStartupPacket(MyProcPort, false, false);

		if (status != STATUS_OK)
		{
			CloseClientSocket();
			BackendPoolMarkPooled(MyProcPid);
			pgstat_report_activity(STATE_POOLED, NULL);
			set_ps_display("pooled");
			continue;		/* Loop back to wait for another client */
		}

		/*
		 * Step 6: Check if the client requests a different database.
		 * We need a transaction to look up the database name.
		 */
		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();
		XactIsoLevel = XACT_READ_COMMITTED;

		need_db_switch = (strcmp(MyProcPort->database_name,
								get_database_name(MyDatabaseId)) != 0);

		if (need_db_switch)
		{
			/*
			 * Database switch is not yet supported for pooled backends.
			 * Send a FATAL error to the client so it gets a clean error
			 * message, then close the socket and go back to the pooled
			 * wait state for the next client.
			 *
			 * We cannot use ereport(FATAL) here because that would call
			 * proc_exit(), and we want this backend to stay alive and
			 * return to the pool.  Instead, send the error message
			 * manually using the libpq protocol.
			 */
			CommitTransactionCommand();

			{
				StringInfoData buf;

				pq_beginmessage(&buf, PqMsg_ErrorResponse);
				pq_sendbyte(&buf, PG_DIAG_SEVERITY);
				pq_sendstring(&buf, "FATAL");
				pq_sendbyte(&buf, PG_DIAG_SEVERITY_NONLOCALIZED);
				pq_sendstring(&buf, "FATAL");
				pq_sendbyte(&buf, PG_DIAG_SQLSTATE);
				pq_sendstring(&buf, "08006");
				pq_sendbyte(&buf, PG_DIAG_MESSAGE_PRIMARY);
				pq_sendstring(&buf, psprintf(
					"connection to database \"%s\" failed: "
					"pooled backend is connected to a different database",
					MyProcPort->database_name));
				pq_sendbyte(&buf, '\0');	/* terminator */
				pq_endmessage(&buf);
				pq_flush();
			}

			/* Close client and return to pooled state */
			CloseClientSocket();
			BackendPoolMarkPooled(MyProcPid);
			pgstat_report_activity(STATE_POOLED, NULL);
			set_ps_display("pooled");
			continue;		/* Loop back to wait for another client */
		}

		/*
		 * Step 7: Reload pg_hba.conf and pg_ident.conf.
		 *
		 * PostmasterContext (which held the parsed HBA data inherited from
		 * the postmaster via fork) was deleted during the first
		 * PostgresMain() cycle.  We must reload the auth config files so
		 * that PerformAuthentication has valid parsed_hba_lines to work with.
		 */
		if (PostmasterContext == NULL)
			PostmasterContext = AllocSetContextCreate(TopMemoryContext,
													 "Postmaster",
													 ALLOCSET_DEFAULT_SIZES);
		hba_clear_stale_state();
		if (!load_hba())
			ereport(FATAL,
					(errmsg("could not load %s", "pg_hba.conf")));
		load_ident();

		/*
		 * Step 8: Authenticate the new client and set up the session.
		 */
		PerformAuthentication(MyProcPort);

		/*
		 * Reset user identity state from previous session so that
		 * InitializeSessionUserId() can set it up for the new client.
		 */
		ResetAuthenticatedUserId();
		InitializeSessionUserId(MyProcPort->user_name, InvalidOid, false);
		am_superuser = superuser();

		InvalidateCatalogSnapshot();
		BackendPoolUpdateDatabaseId(MyProcPid, MyDatabaseId);

		process_startup_options(MyProcPort, am_superuser);
		process_settings(MyDatabaseId, GetSessionUserId());

		/*
		 * We skip InitializeSearchPath() and InitializeClientEncoding()
		 * here because they are designed for first-time startup.  The
		 * search path callbacks were already registered during the initial
		 * startup, and client encoding is already set up.  ResetAllOptions()
		 * in the cleanup step already reset GUCs to defaults, and
		 * process_startup_options/process_settings above applied the new
		 * client's settings.
		 */

		CommitTransactionCommand();
		break;
	}

	/*
	 * Step 9: Generate new cancel key and send BackendKeyData.
	 */
	{
		int			len;

		len = (MyProcPort->proto >= PG_PROTOCOL(3, 2))
			? MAX_CANCEL_KEY_LENGTH : 4;
		if (!pg_strong_random(&MyCancelKey, len))
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("could not generate random cancel key")));
		MyCancelKeyLength = len;
		ProcSignalUpdateCancelKey(MyCancelKey, MyCancelKeyLength);
	}

	if (whereToSendOutput == DestRemote)
	{
		StringInfoData buf;

		pq_beginmessage(&buf, PqMsg_BackendKeyData);
		pq_sendint32(&buf, (int32) MyProcPid);
		pq_sendbytes(&buf, MyCancelKey, MyCancelKeyLength);
		pq_endmessage(&buf);
	}

	/*
	 * Step 10: Update pgstat and ps display.
	 */
	pgstat_report_connect(MyDatabaseId);
	pgstat_bestart_final();

	{
		StringInfoData ps_data;

		initStringInfo(&ps_data);
		appendStringInfo(&ps_data, "%s ", MyProcPort->user_name);
		if (MyProcPort->database_name[0] != '\0')
			appendStringInfo(&ps_data, "%s ", MyProcPort->database_name);
		appendStringInfoString(&ps_data, MyProcPort->remote_host);
		if (MyProcPort->remote_port[0] != '\0')
			appendStringInfo(&ps_data, "(%s)", MyProcPort->remote_port);
		set_ps_display(ps_data.data);
		pfree(ps_data.data);
	}

	BeginReportingGUCOptions();

	elog(DEBUG2, "backend pool: reused backend pid %d for new connection",
		 MyProcPid);

	return true;
}
