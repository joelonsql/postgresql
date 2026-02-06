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
#include "common/relpath.h"
#include "catalog/pg_database.h"
#include "commands/async.h"
#include "commands/event_trigger.h"
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
#include "utils/pgstat_internal.h"
#include "postmaster/backend_pool.h"
#include "postmaster/interrupt.h"
#include "postmaster/postmaster.h"
#include "replication/walsender.h"
#include "storage/bufmgr.h"
#include "storage/smgr.h"
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
#include "utils/relcache.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

/* These functions are declared here because they are not in public headers */
extern int	ProcessSSLStartup(Port *port);
extern int	ProcessStartupPacket(Port *port, bool ssl_done, bool gss_done);
extern void PerformAuthentication(Port *port);
extern void process_settings(Oid databaseid, Oid roleid);
extern HeapTuple GetDatabaseTupleByOid(Oid dboid);

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

		/* Process ProcSignalBarrier so we don't block other backends */
		if (ProcSignalBarrierPending)
			ProcessProcSignalBarrier();

		/* Process config file reload if requested */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Log memory contexts if requested */
		if (LogMemoryContextPending)
			ProcessLogMemoryContextInterrupt();

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
 * process_startup_options_for_reuse
 *
 * A variant of process_startup_options() for pooled backend reuse.
 *
 * In a normal backend startup, session_preload_libraries haven't been loaded
 * yet when process_startup_options runs, so custom GUCs from those libraries
 * are still placeholder variables (PGC_USERSET context).  Placeholder GUCs
 * accept any setting without permission checks.  When the library loads later,
 * define_custom_variable() replaces the placeholder and reapplies the stored
 * value with WARNING elevel, so permission failures produce warnings rather
 * than errors.
 *
 * In a pooled backend, the libraries are already loaded, so custom GUCs are
 * real variables with their actual context (e.g. PGC_SUSET).  If we use
 * the normal process_startup_options(), permission failures raise ERROR and
 * kill the connection.  To match the normal startup behavior, we use WARNING
 * elevel here, so that permission failures are logged as warnings and the
 * setting is silently ignored, just as it would be in a fresh backend.
 */
static void
process_startup_options_for_reuse(Port *port, bool am_superuser)
{
	GucContext	gucctx;
	ListCell   *gucopts;

	gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

	/*
	 * Process command-line switches from the startup packet (PGOPTIONS).
	 * These are typically "-c name=value" pairs.  We parse them the same
	 * way as process_startup_options/process_postgres_switches, but use
	 * WARNING elevel so that permission failures don't kill the connection.
	 */
	if (port->cmdline_options != NULL)
	{
		char	  **av;
		int			maxac;
		int			ac;
		int			i;

		maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;
		av = palloc_array(char *, maxac);
		ac = 0;
		av[ac++] = "postgres";
		pg_split_opts(av, &ac, port->cmdline_options);
		av[ac] = NULL;

		/*
		 * Walk the split arguments looking for "-c name=value" pairs.
		 * We handle both "-c name=value" (separate tokens) and
		 * "-cname=value" (concatenated, as sent by psql's PGOPTIONS).
		 * We skip other options since they are rare in PGOPTIONS and
		 * don't have permission issues.
		 */
		for (i = 1; i < ac; i++)
		{
			char	   *optarg = NULL;

			if (strcmp(av[i], "-c") == 0 && i + 1 < ac)
			{
				/* "-c name=value" form: argument is next token */
				optarg = av[++i];
			}
			else if (strncmp(av[i], "-c", 2) == 0 && av[i][2] != '\0')
			{
				/* "-cname=value" form: argument follows -c directly */
				optarg = av[i] + 2;
			}

			if (optarg != NULL)
			{
				char	   *name;
				char	   *value;

				ParseLongOption(optarg, &name, &value);
				if (name && value)
				{
					(void) set_config_option(name, value,
											 gucctx, PGC_S_CLIENT,
											 GUC_ACTION_SET, true,
											 WARNING, false);
				}
				if (name)
					pfree(name);
				if (value)
					pfree(value);
			}
		}
	}

	/*
	 * Process any additional GUC variable settings passed in the startup
	 * packet.  These are handled the same as command-line variables, but
	 * again with WARNING elevel.
	 */
	gucopts = list_head(port->guc_options);
	while (gucopts)
	{
		char	   *name;
		char	   *value;

		name = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		value = lfirst(gucopts);
		gucopts = lnext(port->guc_options, gucopts);

		(void) set_config_option(name, value,
								 gucctx, PGC_S_CLIENT,
								 GUC_ACTION_SET, true,
								 WARNING, false);
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
	char		pooledDbName[NAMEDATALEN];

	/* Save the database name before ProcessStartupPacket overwrites it */
	strlcpy(pooledDbName, MyProcPort->database_name, NAMEDATALEN);

	/*
	 * Step 1: Session cleanup (equivalent to DISCARD ALL).
	 *
	 * First abort any open transaction, then do cleanup.
	 */
	AbortOutOfAnyTransaction();
	PortalHashTableDeleteAll();
	DropAllPreparedStatements();
	LockReleaseAll(USER_LOCKMETHOD, true);

	SetSessionAuthorization(GetAuthenticatedUserId(),
							GetAuthenticatedUserIsSuperuser());
	SetCurrentRoleId(InvalidOid, false);
	ResetGUCSourceForReuse("session_authorization");
	ResetGUCSourceForReuse("role");
	ResetSessionGUCsForReuse();

	ResetAllOptions();
	ResetPlanCache();
	ResetSequenceCaches();

	/*
	 * Reset the client connection info so PerformAuthentication can set the
	 * authn_id again for the new client.
	 */
	if (MyClientConnectionInfo.authn_id)
	{
		pfree((void *) MyClientConnectionInfo.authn_id);
		MyClientConnectionInfo.authn_id = NULL;
	}
	MyClientConnectionInfo.auth_method = 0;

	/*
	 * Async_UnlistenAll() and ResetTempTableNamespace() need a transaction
	 * context.  Async_UnlistenAll() queues a pending UNLISTEN_ALL action
	 * using CurTransactionContext, and ResetTempTableNamespace() needs
	 * catalog access to drop temp tables.  The pending unlisten action is
	 * applied when the transaction commits.
	 */
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	Async_UnlistenAll();
	ResetTempTableNamespace();
	ResetTempNamespaceForReuse();
	PopActiveSnapshot();
	CommitTransactionCommand();

	/*
	 * Reset local buffer pool after temp tables are dropped, so the next
	 * session can reinitialize with a potentially different temp_buffers.
	 */
	ResetLocalBuffers();

	/*
	 * Release smgr references so that stale file handles from this session
	 * don't persist into the next one.  For example, ALTER DATABASE SET
	 * TABLESPACE moves relation files, and a reused backend must not use
	 * cached file handles from the old tablespace.
	 *
	 * Note: we do NOT call RelationCacheInvalidate() here because we need
	 * MyDatabaseTableSpace to be refreshed first.  That refresh happens
	 * during reconnection (Step 6), after which we invalidate the relcache
	 * so entries are rebuilt with the correct tablespace.
	 */
	smgrreleaseall();

	/*
	 * Flush the per-backend opclass cache.  LookupOpclassInfo() caches
	 * support procedure OIDs (from pg_amproc) and never invalidates them,
	 * which is fine when each connection gets a fresh backend.  With
	 * connection pooling the backend is reused, so we must invalidate
	 * this cache to pick up any pg_amproc changes made by previous sessions.
	 */
	InvalidateOpClassCache();

	/*
	 * Step 2: Flush pending stats for the disconnecting session, including
	 * the disconnect counter.  This ensures that stat consumers (e.g.,
	 * pg_stat_database, pg_stat_io) see up-to-date values even though the
	 * backend process doesn't actually exit.
	 */
	pgstat_report_disconnect(MyDatabaseId);
	pgstat_report_stat(true);

	/*
	 * Step 3: Close the client socket.
	 */
	CloseClientSocket();

	/*
	 * Step 4: Update shared state to indicate we're pooled.
	 *
	 * Clear databaseId so that CountOtherDBBackends() does not count this
	 * pooled backend as an active connection.  This allows DROP DATABASE
	 * and ALTER DATABASE to proceed while backends sit in the pool.
	 * MyDatabaseId is kept so we can restore it when we reconnect.
	 */
	MyProc->roleId = InvalidOid;
	MyProc->databaseId = InvalidOid;
	pgstat_report_activity(STATE_POOLED, NULL);
	set_ps_display("pooled");

	/*
	 * Remove the backend from pg_stat_activity by clearing st_procpid.
	 * This prevents tests and tools that wait for a specific PID to
	 * disappear from pg_stat_activity from hanging indefinitely.
	 * pgstat_bestart_final() restores it when the backend reconnects.
	 */
	{
		volatile PgBackendStatus *beentry = MyBEEntry;

		PGSTAT_BEGIN_WRITE_ACTIVITY(beentry);
		beentry->st_procpid = 0;
		PGSTAT_END_WRITE_ACTIVITY(beentry);
	}

	/*
	 * Verify our database still exists before entering the pool.  If it
	 * was dropped while we were cleaning up, entering the pool with a
	 * stale database name would cause the postmaster to assign new
	 * connections (for a recreated database with the same name) to us,
	 * only for us to FATAL because our MyDatabaseId no longer exists.
	 *
	 * Use GetDatabaseTupleByOid with criticalRelcachesBuilt temporarily
	 * cleared, same as we do in Step 6 during reconnection.
	 */
	{
		HeapTuple	dbTup;
		bool		saved_criticalRelcachesBuilt;

		saved_criticalRelcachesBuilt = criticalRelcachesBuilt;
		criticalRelcachesBuilt = false;

		SetCurrentStatementStartTimestamp();
		StartTransactionCommand();

		dbTup = GetDatabaseTupleByOid(MyDatabaseId);

		criticalRelcachesBuilt = saved_criticalRelcachesBuilt;

		if (!HeapTupleIsValid(dbTup))
		{
			CommitTransactionCommand();
			elog(DEBUG1, "database with OID %u was dropped, backend exiting instead of pooling",
				 MyDatabaseId);
			return false;
		}
		heap_freetuple(dbTup);
		CommitTransactionCommand();
	}

	/*
	 * Try to enter the pool.  If the pool is full, exit so that the
	 * PGPROC slot is freed for new connections.
	 */
	if (!BackendPoolMarkPooled(MyProcPid, pooledDbName))
		return false;

	/*
	 * Reset connection timing so that "connection ready" is logged for
	 * each new client when log_connections includes setup_durations.
	 */
	conn_timing.ready_for_use = TIMESTAMP_MINUS_INFINITY;

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

		/* Restore databaseId now that we're serving a client again */
		MyProc->databaseId = MyDatabaseId;
		BackendPoolMarkActive(MyProcPid);

		/*
		 * Reload the config file unconditionally before handling the new
		 * client.  This serves two purposes:
		 *
		 * 1. We can't rely on ConfigReloadPending because the SIGHUP from
		 *    the postmaster may not have been delivered yet (race between
		 *    signal delivery and the new client arriving on the socketpair).
		 *
		 * 2. PGC_SU_BACKEND GUCs (like log_connections) are normally fixed
		 *    for the lifetime of a backend and ignored during SIGHUP.
		 *    But a pooled backend effectively starts a new session, so it
		 *    should pick up config changes.  We set guc_apply_backend_gucs
		 *    to allow PGC_SU_BACKEND/PGC_BACKEND values to be updated.
		 */
		ConfigReloadPending = false;
		guc_apply_backend_gucs = true;
		ProcessConfigFile(PGC_SIGHUP);
		guc_apply_backend_gucs = false;

		/*
		 * Step 5: Reinitialize the connection with the new client socket.
		 */
		conn_timing.socket_create = GetCurrentTimestamp();
		conn_timing.fork_start = conn_timing.socket_create;
		conn_timing.fork_end = conn_timing.socket_create;
		AcceptNewClient(&newClientSocket);

		/* Log connection received, same as backend_startup.c */
		if (log_connections & LOG_CONNECTION_RECEIPT)
		{
			if (MyProcPort->remote_port[0])
				ereport(LOG,
						(errmsg("connection received: host=%s port=%s",
								MyProcPort->remote_host,
								MyProcPort->remote_port)));
			else
				ereport(LOG,
						(errmsg("connection received: host=%s",
								MyProcPort->remote_host)));
		}

		/*
		 * Process SSL/GSS handshake and startup packet.
		 */
		status = ProcessSSLStartup(MyProcPort);
		if (status == STATUS_OK)
			status = ProcessStartupPacket(MyProcPort, false, false);

		if (status != STATUS_OK)
		{
			CloseClientSocket();
			MyProc->databaseId = InvalidOid;
			if (!BackendPoolMarkPooled(MyProcPid, pooledDbName))
				return false;
			pgstat_report_activity(STATE_POOLED, NULL);
			set_ps_display("pooled");
			continue;		/* Loop back to wait for another client */
		}

		/*
		 * Replication (walsender) connections cannot be served by a
		 * pooled backend because walsender state (MyWalSnd, replication
		 * slots, etc.) requires dedicated initialization at process
		 * startup.  If this somehow got through the postmaster's peek
		 * check, exit so the postmaster will fork a proper walsender.
		 */
		if (am_walsender)
		{
			CloseClientSocket();
			ereport(FATAL,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("replication connections cannot be served by pooled backends")));
		}

		/*
		 * Step 6: Refresh MyDatabaseTableSpace, check if the database
		 * still exists, and look up its name.
		 *
		 * MyDatabaseTableSpace may be stale if ALTER DATABASE SET
		 * TABLESPACE ran while we were pooled.  We must refresh it
		 * before any per-database relcache access, because relcache
		 * entries with reltablespace=0 derive their physical path
		 * from MyDatabaseTableSpace.
		 *
		 * To avoid a chicken-and-egg problem (refreshing from the
		 * catalog needs the relcache, but the relcache needs the
		 * correct tablespace), we temporarily clear criticalRelcachesBuilt.
		 * This prevents RelationReloadNailed() from trying to reload
		 * nailed catalog entries via ScanPgRelation (which would access
		 * pg_class in the wrong tablespace).  Shared catalogs like
		 * pg_database live in the global tablespace and are unaffected.
		 */
		SetCurrentStatementStartTimestamp();

		{
			HeapTuple	dbTup;
			bool		saved_criticalRelcachesBuilt;

			saved_criticalRelcachesBuilt = criticalRelcachesBuilt;
			criticalRelcachesBuilt = false;

			StartTransactionCommand();
			XactIsoLevel = XACT_READ_COMMITTED;

			dbTup = GetDatabaseTupleByOid(MyDatabaseId);

			criticalRelcachesBuilt = saved_criticalRelcachesBuilt;

			if (!HeapTupleIsValid(dbTup))
			{
				/*
				 * Database was dropped while we were pooled.  This is a
				 * race condition: BackendPoolEvictDatabase() ran before
				 * we entered the pool, but we got assigned a new client
				 * for a recreated database with the same name.
				 *
				 * Close the client socket and exit.  The postmaster will
				 * fork a fresh backend when the client retries.  We use
				 * proc_exit(0) instead of FATAL to avoid sending an error
				 * message to the client -- they'll see a connection reset.
				 */
				CommitTransactionCommand();
				elog(LOG, "database with OID %u was dropped while backend was pooled, exiting",
					 MyDatabaseId);
				CloseClientSocket();
				proc_exit(0);
			}

			{
				Form_pg_database dbForm;

				dbForm = (Form_pg_database) GETSTRUCT(dbTup);
				MyDatabaseTableSpace = dbForm->dattablespace;
				MyDatabaseHasLoginEventTriggers = dbForm->dathasloginevt;
				need_db_switch = (strcmp(MyProcPort->database_name,
										  NameStr(dbForm->datname)) != 0);
			}
			heap_freetuple(dbTup);

			/*
			 * Update DatabasePath to match the (potentially new) tablespace.
			 * DatabasePath is normally set once during startup, but with
			 * connection pooling it must be refreshed when the tablespace
			 * changes.  Free the old path first.
			 */
			if (DatabasePath)
				pfree(DatabasePath);
			DatabasePath = NULL;
			SetDatabasePath(GetDatabasePath(MyDatabaseId,
											   MyDatabaseTableSpace));
		}

		/*
		 * Now that MyDatabaseTableSpace is correct, invalidate the
		 * relcache so that entries are rebuilt with the right tablespace.
		 * Also release smgr references in case file paths changed.
		 */
		smgrreleaseall();
		RelationCacheInvalidate(false);

		if (need_db_switch)
		{
			/*
			 * Database mismatch despite postmaster-side matching.  This
			 * shouldn't normally happen, but can if the startup packet
			 * couldn't be peeked (e.g., SSL wrapping).  Exit instead of
			 * looping to avoid infinite assignment cycles.
			 */
			CommitTransactionCommand();

			ereport(FATAL,
					(errcode(ERRCODE_CONNECTION_FAILURE),
					 errmsg("connection to database \"%s\" failed: "
							"pooled backend is connected to database \"%s\"",
							MyProcPort->database_name,
							pooledDbName)));
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
		conn_timing.auth_start = GetCurrentTimestamp();
		PerformAuthentication(MyProcPort);
		conn_timing.auth_end = GetCurrentTimestamp();

		/*
		 * Reset user identity state from previous session so that
		 * InitializeSessionUserId() can set it up for the new client.
		 */
		ResetAuthenticatedUserId();
		InitializeSessionUserId(MyProcPort->user_name, InvalidOid, false);

		/* Initialize SYSTEM_USER for the new client session */
		if (MyClientConnectionInfo.authn_id)
			InitializeSystemUser(MyClientConnectionInfo.authn_id,
								 hba_authname(MyClientConnectionInfo.auth_method));

		am_superuser = superuser();

		InvalidateCatalogSnapshot();
		BackendPoolUpdateDatabaseId(MyProcPid, MyDatabaseId);

		/*
		 * MyDatabaseTableSpace and MyDatabaseHasLoginEventTriggers were
		 * already refreshed from the catalog in Step 6 above.
		 */

		process_startup_options_for_reuse(MyProcPort, am_superuser);
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
	 *
	 * Reinitialize the backend status entry from scratch: st_procpid,
	 * st_clientaddr, etc.  pgstat_bestart_initial() sets st_procpid =
	 * MyProcPid (which we zeroed when entering the pool), updates the
	 * client address from the new MyProcPort, and resets activity
	 * timestamps.  pgstat_bestart_security() refreshes SSL/GSS state.
	 * pgstat_bestart_final() fills in databaseid, userid, appname.
	 */
	pgstat_report_connect(MyDatabaseId);
	pgstat_bestart_initial();
	pgstat_bestart_security();
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

	ResetReportedGUCOptions();
	BeginReportingGUCOptions();

	/* Fire any defined login event triggers, if appropriate */
	EventTriggerOnLogin();

	return true;
}
