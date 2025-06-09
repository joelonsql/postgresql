# **Backend Parking for PostgreSQL — Detailed Design & Implementation Blueprint**

---

## Introduction

PostgreSQL’s **process-per-connection** architecture means every open session owns a backend process that participates in catalogue‐wide activities such as snapshot building, `xmin` horizon calculations, and lock-table scans.  When thousands of mostly-idle sessions are present, those O(N) paths dominate latency and erode cache locality, even though the idle clients are doing no useful work.

This blueprint defines “**backend parking**”: a new idle state in which a backend completely withdraws from the database’s critical shared structures while keeping its network socket, TLS session, authentication identity, and GUC stack alive.  Parked sessions consume only a few hundred kilobytes and **do not appear in any O(N) loops**; when the next client byte arrives, the backend quickly “un-parks” and resumes normal service.

The document below is written for a Code-Generation LLM tasked with producing the actual patch series.  It spells out *what* must be added or changed, *where* in the tree, and *why*, in a commit-by-commit order that keeps PostgreSQL compiling and passing tests at every step.

---

## Motivation

1. **Performance scaling** – Convert snapshot, vacuum horizon, and lock-table algorithms from O(N = all connections) to O(A = active connections).
2. **Memory economy** – Reduce idle-session footprint from \~10–40 MiB to ≈0.5 MiB, allowing tens of thousands of connections on moderate RAM.
3. **Zero wire-protocol impact** – Everything still speaks protocol 3.0; no new frontend requirements.
4. **Operational simplicity** – One new monitoring state (`parked`) and three GUCs; default OFF for painless upgrade.
5. **Extensibility & safety** – Hook for extensions to veto parking; feature guarded by `enable_parking`.

---

## 0 · Global goals & constraints

| Item                 | Value                                       |
| -------------------- | ------------------------------------------- |
| Feature / GUC prefix | **backend parking**                         |
| Wire-protocol impact | **None** — still v3.0                       |
| Default state        | Disabled (`enable_parking = off`)           |
| Review target        | `REL_18_STABLE` tip (June 2025)             |
| Build & test         | `meson test` + `make check-world` must pass |
| Catversion           | **Unchanged** (no on-disk format change)    |

---

## 1 · New backend states & flags

**File:** `src/include/storage/proc.h`

```c
typedef struct PGPROC
{
    /* …existing fields… */
    uint8  backend_state;          /* PROC_STATE_ACTIVE or PROC_STATE_PARKED */
} PGPROC;

#define PROC_STATE_ACTIVE 0
#define PROC_STATE_PARKED 1

static inline bool
IS_PROC_PARKED(const PGPROC *p)
{
    return p->backend_state == PROC_STATE_PARKED;
}
```

*(Member appended to preserve extension ABI assumptions.)*

---

## 2 · Dual linked lists inside `ProcArray`

### 2.1 Structure extension

**File:** `src/include/storage/procarray.h`

```c
typedef struct ProcArrayStruct
{
    /* …existing fields… */
    PGPROC *activeProcs;   /* head of active singly-linked list */
    PGPROC *parkedProcs;   /* head of parked singly-linked list */
    int     numActiveProcs;
} ProcArrayStruct;
```

Initialisers in `procarray.c` and postmaster fork paths must be updated.

### 2.2 Helpers (in `procarray.c`)

```c
static inline void LinkIntoActiveList(ProcArrayStruct *array, PGPROC *proc);
static inline void LinkIntoParkedList(ProcArrayStruct *array, PGPROC *proc);
```

---

## 3 · Transition functions

**File:** `src/backend/storage/ipc/procarray.c`

### 3.1 `ParkMyBackend(void)`

1. `Assert(!IsTransactionState());`
2. `LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);`
3. Move `MyProc` from `activeProcs` to `parkedProcs`; decrement `numActiveProcs`; set `backend_state = PROC_STATE_PARKED`.
4. `LWLockRelease(ProcArrayLock);`
5. `ResourceOwnerReleaseAll(PARKED_CLEANUP);`   *(new enum phase in §6)*
6. `pgstat_report_state(STATE_PARKED);`

### 3.2 `UnparkMyBackend(void)`

Reverse the above, plus `RebuildTopTransactionContext()`.

### 3.3 Safety

* Inside a transaction → `ERRCODE_ACTIVE_SQL_TRANSACTION`.
* Holding listener slots, advisory locks, or prepared xacts → refuse to park.

---

## 4 · Idle-loop integration

**File:** `src/backend/tcop/postgres.c`

* Load new GUCs.
* After each `ReadyForQuery`, compute `deadline = now + park_after`.
* Custom wait loop:

```c
while (true)
{
    int rc = WaitLatchOrSocket(MyLatch,
        WL_SOCKET_READABLE | WL_LATCH_SET | WL_TIMEOUT,
        deadline_reached ? -1 : park_after_ms,
        client_socket,
        WAIT_EVENT_CLIENT_READ);

    if (rc & WL_SOCKET_READABLE)
        break;                /* byte ready → unpark later */

    if (enable_parking && !deadline_reached)
    {
        ParkMyBackend();
        deadline_reached = true;
    }
}
```

`UnparkMyBackend()` executes just before processing the incoming protocol message.

---

## 5 · Snapshot & autovacuum paths

### 5.1 `GetSnapshotData()`

Iterate only `activeProcs`:

```c
for (PGPROC *proc = array->activeProcs; proc; proc = proc->links.next)
```

### 5.2 `GetOldestXmin()` and similar

Start from `activeProcs`; update all call sites; unit tests must pass.

---

## 6 · Resource-owner PARK release

**Files:** `src/backend/utils/resowner/*.c`, `resowner.h`

* Add `RESOURCE_RELEASE_PARK` enum.
* `ResourceOwnerReleaseAll(PARKED_CLEANUP)` drops portals, catcaches, relcaches, temp schemas, and releases all locks.
* Provide `ReleaseResourcesForPark()` wrapper.

---

## 7 · Configuration (GUCs)

**File:** `src/backend/utils/misc/guc_tables.c`

| Name                  | Type (unit) | Default          | Context    |
| --------------------- | ----------- | ---------------- | ---------- |
| `enable_parking`      | bool        | `off`            | SIGHUP     |
| `park_after`          | integer ms  | 5000             | superuser  |
| `max_active_backends` | integer     | `MaxConnections` | postmaster |

`max_active_backends` checked when un-parking; raise ERROR if exceeded.

---

## 8 · SQL interface

### 8.1 Function

**Files:** `src/backend/utils/adt/parkfuncs.c`, header `parkfuncs.h`

```c
PG_FUNCTION_INFO_V1(pg_park);

Datum
pg_park(PG_FUNCTION_ARGS)
{
    if (!enable_parking)
        PG_RETURN_BOOL(false);

    if (IsTransactionState())
        ereport(ERROR,
            (errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
             errmsg("cannot park while a transaction is active")));

    if (!ParkMyBackend())
        PG_RETURN_BOOL(false);      /* vetoed */

    PG_RETURN_BOOL(true);
}
```

### 8.2 `pg_proc.dat`

```
{ pg_park | 0 | PGNSP | PGUID | f | f | v | f | 0 | 2278 | 1024 | 0 | f | f | f | f | s | 0 0 0 0 0 }
```

### 8.3 Regression test

`src/test/regress/sql/park.sql`

```sql
SELECT pg_park();
SELECT 1;    -- should succeed after unpark
```

---

## 9 · `pg_stat_activity` changes

* Add `'parked'` to `state` enum in `catalog/pg_stat_activity.table`.
* Collector sets that state when `backend_state == PROC_STATE_PARKED`.

---

## 10 · Parking veto hook

**File:** `miscadmin.h`

```c
typedef bool (*parking_hook_type)(void);
extern PGDLLIMPORT parking_hook_type parking_hook;
```

Invoke inside `ParkMyBackend()`; if any registered hook returns `false`, parking is aborted.

---

## 11 · Signals, latches, postmaster

* No changes to postmaster child list or signal routing.
* `pg_signal_backend()` continues to work; PID unchanged.

---

## 12 · Documentation

* `doc/src/sgml/config.sgml` — new GUCs.
* `doc/src/sgml/monitoring.sgml` — add `'parked'` backend state.
* `doc/src/sgml/func.sgml` — document `pg_park()`.
* `release-18.sgml` — note under *Performance Features*.

---

## 13 · Performance benchmark harness

Directory `src/tools/parkbench/`:

```bash
pgbench -c 10000 -j 32 -T 60 -S --protocol=simple
```

Run twice (parking off/on); store TPS and 95th-percentile latency to CSV; CI fails if latency drop <20 %.

---

## 14 · Patch series

| # | Commit title                                       | Touches                              |
| - | -------------------------------------------------- | ------------------------------------ |
| 1 | **procarray: split active / parked lists**         | proc.h, procarray.\*                 |
| 2 | **snapshot: iterate active list only**             | procarray.c                          |
| 3 | **backend: add parking transition logic**          | procarray.c, postgres.c, resowner.\* |
| 4 | **guc: introduce enable\_parking / park\_after …** | guc\_tables.c, docs                  |
| 5 | **stat: expose ‘parked’ backend state**            | pgstat.\*                            |
| 6 | **sql: add pg\_park()**                            | parkfuncs.c, pg\_proc.dat, tests     |
| 7 | **resource owner: PARK release phase**             | resowner.c/h                         |
| 8 | **benchmarks & docs**                              | tools/, sgml                         |

Each commit must compile and pass regression tests independently.

---

## 15 · Validation checklist

1. `meson test` and `make check-world` green on GCC & Clang.
2. `pgbench -C -j64 -c40000` shows ≥25 % latency drop with parking enabled.
3. `SELECT pg_park()` on unpatched server returns SQLSTATE 42883.
4. Valgrind: no leaks across park/unpark cycles.
5. Coverity/clang-tidy: 0 new issues.
6. Catversion unchanged; `pg_upgrade` unaffected.
7. Extensions (pg\_stat\_statements, logical decoding) work or veto via hook.
8. Documentation builds (`make docs`) without warnings.

---

**End of blueprint — ready for Code Generation.**
