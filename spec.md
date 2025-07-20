# Specification: Enhance PostgreSQL `LISTEN/NOTIFY` to Eliminate Thundering Herd

**Project Goal:**
Modify the PostgreSQL `LISTEN/NOTIFY` mechanism to solve the "thundering herd" problem, where a single `NOTIFY` command wakes up all listening backends simultaneously. The new design should be efficient, minimize notification latency for critical backends, and ensure all listeners eventually receive their notifications in a controlled manner.

**Core Implementation Idea:**
This will be achieved through a dual-strategy approach:

1.  **Immediate, Targeted Wakeup:** The `NOTIFY`-ing backend will immediately signal a *single* listening backend—specifically, one of the backends that is furthest behind in processing the notification queue (i.e., a backend at the `QUEUE_TAIL`). This ensures the queue tail can advance promptly, preventing queue bloat.

2.  **Dedicated Dispatcher Daemon:** A new, permanent background worker, the "notify dispatcher," will be introduced. `NOTIFY`-ing backends will now signal this single daemon instead of all listeners. The dispatcher, in turn, will wake up batches of other lagging listeners in a controlled, throttled manner, ensuring complete notification delivery without overwhelming the system.

This combined approach decouples urgent queue maintenance from general notification, providing both low latency for tail-advancement and high-throughput, controlled wakeups for everyone else.

**Detailed Implementation Steps:**

1.  **Introduce the Notify Dispatcher Daemon:**
    *   Create a new background worker named `notify dispatcher`.
    *   It should register itself at server startup (`BgWorkerStart_RecoveryFinished`).
    *   Its primary logic will be to wake periodically or when its latch is set. Upon waking, it should identify a batch of the most-lagging listener backends (those furthest from `QUEUE_HEAD`) and send them a `PROCSIG_NOTIFY_INTERRUPT` signal.
    *   Implement its main logic in `src/backend/postmaster/notify_dispatcher.c` and its header in `src/include/postmaster/notify_dispatcher.h`.
    *   Register `NotifyDispatcherMain` as a known internal function in `src/backend/postmaster/bgworker.c`.
    *   Add `notify_dispatcher.o` to the build in `src/backend/Makefile`.

2.  **Introduce Configuration Parameters (GUCs):**
    *   Create a new GUC `notify_dispatcher_batch_size` (integer, default `1`). This controls how many listeners the daemon wakes per cycle. This gives DBAs control over the wakeup throttle.
    *   Create a new GUC `notify_dispatcher_wake_interval` (integer, ms, default `10000`). This is the timeout for the dispatcher's `WaitLatch` call, ensuring it wakes periodically even without new `NOTIFY` signals.
    *   Define these GUCs in `src/backend/utils/misc/guc.c` and document them in `doc/src/sgml/config.sgml`.

3.  **Modify `async.c` and Shared Memory:**
    *   **Shared State:** Add a `ProcNumber dispatcherProc` field to the `AsyncQueueControl` struct in `async.c`. The dispatcher daemon will store its `ProcNumber` here on startup.
    *   **Rewrite `SignalBackends()`:** This function is the heart of the change. Its new logic should be:
        a. **Find and Signal the Tail:** Scan the listener list to find one backend whose position is holding up the queue tail. **A backend is considered "at the tail" if its queue page is the same as the global tail page.** This can be checked with the condition: `asyncQueuePageDiff(QUEUE_POS_PAGE(QUEUE_TAIL), QUEUE_POS_PAGE(pos)) == 0`. Send an immediate `PROCSIG_NOTIFY_INTERRUPT` to the first such backend found. This implements the "immediate, targeted wakeup" part of the strategy.
        b. **Signal the Dispatcher:** After signaling the tail backend, set the latch of the notify dispatcher daemon (whose `ProcNumber` is now available in `asyncQueueControl->dispatcherProc`). This replaces the old logic of iterating and signaling all listeners.
