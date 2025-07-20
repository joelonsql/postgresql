# PostgreSQL LISTEN/NOTIFY Thundering Herd Enhancement

## Overview

This project enhances PostgreSQL's LISTEN/NOTIFY mechanism to eliminate the "thundering herd" problem, where a single NOTIFY command wakes up all listening backends simultaneously, causing unnecessary system load.

## Problem Statement

In the original implementation, when a NOTIFY is issued, the `SignalBackends()` function in `async.c` sends a `PROCSIG_NOTIFY_INTERRUPT` signal to *every* listening backend, regardless of whether they're interested in that particular notification. This causes all listeners to wake up and scan the queue, creating a thundering herd effect that can overwhelm the system when there are many listeners.

## Solution Design

The solution implements a dual-strategy approach:

1. **Immediate, Targeted Wakeup**: The NOTIFY-ing backend immediately signals a single listening backend that is at the queue tail (furthest behind). This ensures the queue tail can advance promptly, preventing queue bloat.

2. **Dedicated Dispatcher Daemon**: A new background worker, the "notify dispatcher," handles controlled wakeup of remaining listeners. Instead of signaling all listeners, `SignalBackends()` now signals only the tail backend and the dispatcher. The dispatcher then wakes listeners in configurable batches, preventing system overload.

## Implementation Status

### Completed Components

1. **Notify Dispatcher Background Worker** (`src/backend/postmaster/notify_dispatcher.c`)
   - Main function: `NotifyDispatcherMain()`
   - Latch-based sleep with configurable wake interval
   - Stores its ProcNumber in shared memory on startup
   - Wakes lagging listeners in controlled batches via `AsyncNotifyDispatcherWakeListeners()`

2. **Configuration Parameters (GUCs)**
   - `notify_dispatcher_batch_size` (default: 1) - Number of listeners to wake per cycle
   - `notify_dispatcher_wake_interval` (default: 10000ms) - Dispatcher wake interval
   - Both parameters can be changed at runtime with SIGHUP

3. **Modified async.c**
   - Added `dispatcherProc` field to `AsyncQueueControl` structure
   - Completely rewrote `SignalBackends()` to:
     - Find and signal ONE backend at the tail (condition: `asyncQueuePageDiff(QUEUE_POS_PAGE(QUEUE_TAIL), QUEUE_POS_PAGE(pos)) == 0`)
     - Signal the dispatcher daemon via SetLatch()
   - Added support functions:
     - `AsyncNotifySetDispatcherProc()` - Called by dispatcher to register itself
     - `AsyncNotifyDispatcherWakeListeners()` - Called by dispatcher to wake listeners

4. **Integration**
   - Registered `NotifyDispatcherMain` in `bgworker.c` internal functions
   - Added `NotifyDispatcherRegister()` call in `postmaster.c` startup
   - Updated Makefile to include `notify_dispatcher.o`
   - Added documentation for new GUCs in `config.sgml`

### Key Files Modified

- `src/backend/postmaster/notify_dispatcher.c` (new)
- `src/include/postmaster/notify_dispatcher.h` (new)
- `src/backend/commands/async.c`
- `src/include/commands/async.h`
- `src/backend/postmaster/bgworker.c`
- `src/backend/postmaster/postmaster.c`
- `src/backend/postmaster/Makefile`
- `src/backend/utils/misc/guc_tables.c`
- `doc/src/sgml/config.sgml`

## How It Works

1. When `NOTIFY` is issued, `SignalBackends()` now:
   - Scans for ONE backend at the tail (whose position matches global tail)
   - Signals that backend immediately to advance the queue
   - Signals the dispatcher daemon to handle remaining listeners

2. The dispatcher daemon:
   - Wakes periodically (every `notify_dispatcher_wake_interval` ms)
   - Also wakes when its latch is set by `SignalBackends()`
   - Calls `AsyncNotifyDispatcherWakeListeners()` to wake up to `notify_dispatcher_batch_size` lagging listeners
   - Prioritizes listeners in the same database, but also wakes cross-database listeners if they're far behind

## Testing Considerations

To test this implementation:

1. **Basic Functionality**: Verify LISTEN/NOTIFY still works correctly
2. **Thundering Herd Prevention**: Monitor that only controlled batches of listeners wake up
3. **Queue Tail Advancement**: Ensure the queue tail advances promptly
4. **Cross-Database Notifications**: Test listeners in different databases
5. **Configuration Changes**: Test runtime changes to batch size and wake interval

## Future Work

- Performance benchmarking with varying numbers of listeners
- Tuning default values for batch size and wake interval
- Potential optimization of listener selection algorithm
- Additional monitoring/statistics for dispatcher activity

## Build Commands

```bash
# To build PostgreSQL with these changes:
./configure [your options]
make
make install

# To run tests:
make check
```

## Configuration Example

```sql
-- Set batch size to wake 5 listeners at a time
ALTER SYSTEM SET notify_dispatcher_batch_size = 5;

-- Set wake interval to 5 seconds
ALTER SYSTEM SET notify_dispatcher_wake_interval = 5000;

-- Reload configuration
SELECT pg_reload_conf();
```