# Backend Connection Reuse - Handover

## What This Is

Server-side connection pooling for PostgreSQL. When a client disconnects,
the backend process stays alive in a "pooled" state instead of exiting.
The postmaster routes new connections to pooled backends via Unix socketpair
FD passing (SCM_RIGHTS), avoiding fork() overhead entirely.

## Current State

**The feature works for same-database reuse.** Manual testing confirms:
- Backend PID is reused across consecutive connections
- Session state (GUCs, temp tables, prepared statements) is properly cleaned
- DB mismatch sends a clean FATAL error to the client, backend returns to pool
- 20+ consecutive reuses work without issues

**Regression tests have NOT been run successfully yet.** The `meson test`
framework uses `pg_regress` which creates a `regression` database, then
runs psql against it. The pooled backend from the initial `postgres`
connection gets the `regression` DB request, sends FATAL (DB mismatch),
and `pg_regress` retries, eventually getting a fresh fork. This should
work, but there may be subtle issues with test infrastructure expectations.

The tests were attempted on macOS but macOS has pre-existing issues with
some regression tests. **Your task is to run the regression tests on this
Linux machine, investigate any failures, fix them, and get all tests passing.**

## Build Instructions

```bash
# Build
meson setup ~/build-postgresql --prefix=$HOME/install-postgresql -Dcassert=true --buildtype=debugoptimized
ninja -C ~/build-postgresql

# Run regression tests
cd ~/build-postgresql
rm -rf tmp_install/
DESTDIR=tmp_install/ ninja install
rm -rf tmp_install/initdb-template
tmp_install/$HOME/install-postgresql/bin/initdb -D tmp_install/initdb-template --no-sync --no-instructions -A trust
meson test --suite setup
meson test --suite regress --timeout-multiplier 3
```

## Architecture Overview

### New Files
| File | Purpose |
|------|---------|
| `src/backend/postmaster/backend_pool.c` | Shared memory pool, FD passing (SCM_RIGHTS), pool management |
| `src/include/postmaster/backend_pool.h` | Pool data structures, function declarations, `MyPoolSocket` global |
| `src/backend/tcop/backend_reuse.c` | Core reuse logic: session cleanup, wait loop, reconnect |
| `src/include/tcop/backend_reuse.h` | `BackendEnterPooledState()` declaration |

### Key Modified Files
| File | Change |
|------|--------|
| `src/backend/tcop/postgres.c` | EOF/Terminate handler calls `BackendEnterPooledState()` |
| `src/backend/postmaster/postmaster.c` | ServerLoop checks pool before fork; BackendStartup creates socketpair |
| `src/backend/tcop/backend_startup.c` | Saves `MyPoolSocket` from startup data |
| `src/backend/libpq/pqcomm.c` | `pq_reinit()` reinitializes Port with new socket |
| `src/backend/utils/init/postinit.c` | Made `PerformAuthentication`, `process_startup_options`, `process_settings` non-static |
| `src/backend/libpq/hba.c` | Added `hba_clear_stale_state()` to reset dangling HBA pointers |
| `src/backend/utils/init/miscinit.c` | Added `ResetAuthenticatedUserId()` |
| `src/backend/storage/ipc/procsignal.c` | Added `ProcSignalUpdateCancelKey()` |
| `src/include/utils/backend_status.h` | Added `STATE_POOLED` |

### Flow

1. **Postmaster** receives connection, checks `BackendPoolAssignConnection()`
2. If pooled backend available: sends client FD via socketpair (`SCM_RIGHTS`)
3. If none available: normal `BackendStartup()` (fork)
4. **Backend** on client disconnect: `BackendEnterPooledState()` in postgres.c
5. Cleanup (DISCARD ALL equivalent), close socket, mark pooled
6. Wait on socketpair for new client FD
7. Receive FD, `pq_reinit()`, process startup packet, authenticate, resume

### Bugs Fixed During Development

1. **ResetTempTableNamespace outside transaction** - needs `StartTransactionCommand()` + `PushActiveSnapshot()` because it does catalog access
2. **PostmasterContext deleted** - `parsed_hba_lines` became dangling pointers after PostgresMain deletes PostmasterContext. Fixed by `hba_clear_stale_state()` + `load_hba()` reload before authentication
3. **AuthenticatedUserId "call only once" assert** - `SetAuthenticatedUserId()` asserts `!OidIsValid(AuthenticatedUserId)`. Fixed by `ResetAuthenticatedUserId()` before `InitializeSessionUserId()`
4. **InitializeClientEncoding "call only once" assert** - `backend_startup_complete` flag. Fixed by skipping `InitializeClientEncoding()` and `InitializeSearchPath()` on reuse (they're first-time-only setup)

## What To Do

1. **Build and run `meson test --suite regress`** on this Linux machine
2. **Investigate any failures** - check `meson-logs/testlog.txt` and the test output directory `testrun/regress/regress/`
3. **Fix failures** - likely causes:
   - Regression test infrastructure makes multiple DB connections that may hit pooled backends for wrong DB
   - Parallel test workers may have edge cases with pool state
   - Some tests may check `pg_stat_activity` and see unexpected `pooled` state
4. **Run the full test suite** once regress passes: `meson test --timeout-multiplier 3`
5. **Clean up** - remove `contrib/conn_scale_bench/` from the commit if it's not needed

## Key Function: BackendEnterPooledState()

Located in `src/backend/tcop/backend_reuse.c`. This is the core function.
The sequence is:

```
Session cleanup (DISCARD ALL equivalent)
  -> AbortOutOfAnyTransaction, PortalHashTableDeleteAll,
     DropAllPreparedStatements, Async_UnlistenAll,
     LockReleaseAll, ResetAllOptions, ResetPlanCache,
     ResetSequenceCaches, ResetTempTableNamespace (in txn+snapshot)
Close client socket
Mark pooled in shared memory
Wait loop (socketpair readable / postmaster death / signals)
Receive new client FD
pq_reinit() with new socket
ProcessSSLStartup + ProcessStartupPacket
Check DB match (need transaction for get_database_name)
  -> Mismatch: send FATAL, close, loop back to wait
Reload pg_hba.conf (PostmasterContext was deleted)
PerformAuthentication
ResetAuthenticatedUserId + InitializeSessionUserId
process_startup_options + process_settings
Generate new cancel key + BackendKeyData
Update pgstat + ps display
BeginReportingGUCOptions
Return true -> PostgresMain resumes query loop
```
