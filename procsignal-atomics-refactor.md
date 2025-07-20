# **Project Plan: Modernizing `procsignal.c` with Atomics**

## **1. Goal**

Refactor the `procsignal.c` facility to use the modern `atomics.h`
framework. The objective is to improve correctness by eliminating potential
race conditions, and to improve performance by removing redundant `kill()`
system calls for high-frequency signals.

## **2. Motivation**

The `procsignal.c` facility dates to 2009, predating the introduction
of the `atomics.h` API in 2014. Its design reflects the limitations
of the tools available at the time and has notable deficiencies.

The use of an array of `volatile sig_atomic_t` flags is suboptimal.
This type only guarantees atomicity for simple loads and stores, not
for read-modify-write operations, forcing the implementation to rely
on spinlocks.

Furthermore, the current implementation cannot safely coalesce signals
at the application level. This leads to spurious `kill()` syscalls in
high-frequency scenarios. Empirical evidence shows the OS already
coalesces delivery of standard signals, so senders are not currently
guaranteed exactly-N delivery anyway. Modifying the implementation
to coalesce signals at the application layer would therefore not
violate the existing effective contract of `SendProcSignal()`.

## **3. Proposed Design**

The core of the change is to replace the array of boolean-like flags
with a single atomic bitmask. This allows for both a correctness fix
and a significant performance optimization.

### **3.1. Primitives Modernization**

First, the `ProcSignalSlot` data structure will be changed to use
a single atomic integer for all signal flags:

-   From: `volatile sig_atomic_t pss_signalFlags[NUM_PROCSIGNALS];`
-   To: `pg_atomic_uint32 pss_signalFlags;`

The sender logic in `SendProcSignal` will be updated to use an atomic
OR operation, `pg_atomic_fetch_or_u32()`, to set the appropriate bit in
the `pss_signalFlags` mask, removing the need for an explicit spinlock.

The receiver logic in the `procsignal_sigusr1_handler` will be updated
to atomically consume all pending signal bits at once using
`pg_atomic_exchange_u32()`. The handler will then dispatch to the
appropriate `Handle*()` functions based on the bits set in the value
returned by the exchange. This single atomic consumption point makes the
entire mechanism race-free.

### **3.2. Redundant Signal Elimination**

With the robust atomic foundation in place, the `kill()` syscall can be
safely elided. The `pg_atomic_fetch_or_u32()` function returns the
previous value of the bitmask. `SendProcSignal` will be modified to
inspect this return value.

If the bit corresponding to the requested signal was *already set* in the
old value, it implies another sender has already guaranteed that a `SIGUSR1`
is pending delivery. In this case, the function can safely return without
issuing another `kill()`. If the bit was not previously set, it is this
caller's responsibility to emit the `kill()` syscall to trigger the handler.

## **4. Research**

### **4.1. Experiment to prove OS signal coalescing**

```
joel@Joels-MacBook-Pro postgresql % git diff
diff --git a/src/backend/storage/ipc/procsignal.c b/src/backend/storage/ipc/procsignal.c
index a9bb540b55a..3cf4d544328 100644
--- a/src/backend/storage/ipc/procsignal.c
+++ b/src/backend/storage/ipc/procsignal.c
@@ -297,6 +297,10 @@ SendProcSignal(pid_t pid, ProcSignalReason reason, ProcNumber procNumber)
            slot->pss_signalFlags[reason] = true;
            SpinLockRelease(&slot->pss_mutex);
            /* Send signal */
+           if (reason == PROCSIG_NOTIFY_INTERRUPT)
+           {
+               elog(LOG, "SIGNAL_SIGUSR1_SENT");
+           }
            return kill(pid, SIGUSR1);
        }
        SpinLockRelease(&slot->pss_mutex);
@@ -677,7 +681,10 @@ procsignal_sigusr1_handler(SIGNAL_ARGS)
        HandleCatchupInterrupt();

    if (CheckProcSignal(PROCSIG_NOTIFY_INTERRUPT))
+   {
+       elog(LOG, "SIGNAL_SIGUSR1_RECEIVED");
        HandleNotifyInterrupt();
+   }

    if (CheckProcSignal(PROCSIG_PARALLEL_MESSAGE))
        HandleParallelMessageInterrupt();

% pgbench -f listen_notify.sql -p 5433 -j 32 -c 32 -T 3 -n
% grep SIGNAL_SIGUSR1_SENT /tmp/pg-patch-v1.log | wc -l
  202544
% grep SIGNAL_SIGUSR1_RECEIVED /tmp/pg-patch-v1.log | wc -l
   87440
```

This is evidence that not all of the SIGUSR1 signal that the application tried
to send were received, which is OK and didn't cause any problems.

### *4.2. Files to read carefully before thinking**

Read these files in full before starting to think about this:
./src/backend/storage/lmgr/README.barrier
./src/include/port/atomics.h
./src/backend/storage/ipc/procsignal.c
