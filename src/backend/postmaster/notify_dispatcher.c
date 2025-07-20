/*-------------------------------------------------------------------------
 *
 * notify_dispatcher.c
 *	  Notify dispatcher background worker
 *
 * The notify dispatcher is responsible for waking up LISTEN/NOTIFY listeners
 * in a controlled manner to prevent thundering herd problems. Instead of
 * waking all listeners at once, it wakes them in configurable batches.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/postmaster/notify_dispatcher.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "commands/async.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/notify_dispatcher.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/shmem.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"

/* GUC variables */
int notify_dispatcher_batch_size = 1;
int notify_dispatcher_wake_interval = 10000;  /* milliseconds */

/* Flag to control main loop */
static volatile sig_atomic_t got_sigterm = false;

/* Signal handlers */
static void notify_dispatcher_sigterm(SIGNAL_ARGS);
static void notify_dispatcher_sighup(SIGNAL_ARGS);

/*
 * Main entry point for notify dispatcher worker
 */
void
NotifyDispatcherMain(Datum main_arg)
{
	/* Establish signal handlers */
	pqsignal(SIGTERM, notify_dispatcher_sigterm);
	pqsignal(SIGHUP, notify_dispatcher_sighup);
	BackgroundWorkerUnblockSignals();

	/* Connect to shared memory */
	BackgroundWorkerInitializeConnection(NULL, NULL, 0);

	/* Store our ProcNumber in shared memory for async.c to use */
	AsyncNotifySetDispatcherProc(MyProcNumber);

	/* Main loop */
	while (!got_sigterm)
	{
		int rc;

		/* Clear any already-pending wakeups */
		ResetLatch(MyLatch);

		/* Process any pending interrupts */
		CHECK_FOR_INTERRUPTS();

		/* Wait for either timeout or latch set */
		rc = WaitLatch(MyLatch,
					   WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
					   notify_dispatcher_wake_interval,
					   WAIT_EVENT_NOTIFY_DISPATCHER_MAIN);

		/* Emergency bailout if postmaster has died */
		if (rc & WL_POSTMASTER_DEATH)
			proc_exit(1);

		/* Process SIGHUP if needed */
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}

		/* Do the actual work of waking listeners */
		if (rc & WL_LATCH_SET)
			AsyncNotifyDispatcherWakeListeners(notify_dispatcher_batch_size);
	}

	proc_exit(0);
}

/*
 * Register the notify dispatcher worker
 */
void
NotifyDispatcherRegister(void)
{
	BackgroundWorker worker;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	snprintf(worker.bgw_library_name, BGW_MAXLEN, "postgres");
	snprintf(worker.bgw_function_name, BGW_MAXLEN, "NotifyDispatcherMain");
	snprintf(worker.bgw_name, BGW_MAXLEN, "notify dispatcher");
	snprintf(worker.bgw_type, BGW_MAXLEN, "notify dispatcher");
	worker.bgw_restart_time = BGW_NEVER_RESTART;
	worker.bgw_main_arg = (Datum) 0;

	RegisterBackgroundWorker(&worker);
}

/*
 * Signal handler for SIGTERM
 */
static void
notify_dispatcher_sigterm(SIGNAL_ARGS)
{
	int save_errno = errno;

	got_sigterm = true;
	SetLatch(MyLatch);

	errno = save_errno;
}

/*
 * Signal handler for SIGHUP
 */
static void
notify_dispatcher_sighup(SIGNAL_ARGS)
{
	int save_errno = errno;

	ConfigReloadPending = true;
	SetLatch(MyLatch);

	errno = save_errno;
}