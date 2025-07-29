/*-------------------------------------------------------------------------
 *
 * buf_table_lockfree.h
 *	  Lock-free hash table implementation for buffer manager
 *
 * This file provides a lock-free replacement for the buffer mapping table,
 * eliminating the need for BufMappingPartitionLock array and enabling
 * wait-free reads and lock-free writes.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/buf_table_lockfree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BUF_TABLE_LOCKFREE_H
#define BUF_TABLE_LOCKFREE_H

#include "storage/buf_internals.h"
#include "port/atomics.h"

/*
 * Number of partitions for the buffer table sizing calculations.
 * Even though we don't have partition locks in the lock-free implementation,
 * we still size the hash table to accommodate concurrent insertions.
 */
#define NUM_BUFFER_PARTITIONS  128

/*
 * Lock-free buffer lookup entry
 * 
 * Similar to BufferLookupEnt but designed for lock-free operations.
 * Uses regular pointers instead of tagged pointers for ARM64 compatibility.
 */
typedef struct LFBufferLookupEnt
{
	BufferTag	tag;			/* Tag of a disk page */
	int			buf_id;			/* Associated buffer ID */
	struct LFBufferLookupEnt *next;	/* Pointer to next entry */
	pg_atomic_uint32 aba_counter;	/* ABA prevention counter */
} LFBufferLookupEnt;

/*
 * Epoch-based Safe Memory Reclamation (SMR) structures
 */
#define LF_MAX_BACKENDS 1024	/* Maximum supported backends */
#define LF_RECLAIM_BATCH 64		/* Entries to batch before reclaiming */
#define LF_EPOCH_INVALID UINT64_MAX

typedef struct LFEpochEntry
{
	pg_atomic_uint64 epoch;		/* Current epoch for this backend */
	char padding[64 - sizeof(pg_atomic_uint64)]; /* Avoid false sharing */
} LFEpochEntry;

typedef struct LFReclaimNode
{
	LFBufferLookupEnt *entry;	/* Entry to reclaim */
	uint64		epoch;			/* Epoch when deleted */
	struct LFReclaimNode *next;	/* Next in reclaim list */
} LFReclaimNode;

/*
 * Bucket head structure for lock-free operations
 * Contains both pointer and ABA counter
 */
typedef struct LFBucketHead
{
	pg_atomic_uint64 head_ptr;		/* Atomic pointer to first entry */
	pg_atomic_uint32 aba_counter;	/* ABA prevention counter */
} LFBucketHead;

/*
 * Main lock-free buffer table structure
 */
typedef struct LFBufTable
{
	/* Hash table buckets */
	LFBucketHead *buckets;		/* Array of bucket heads */
	uint32		nbuckets;		/* Number of buckets (power of 2) */
	uint32		mask;			/* nbuckets - 1 for fast modulo */
	
	/* Memory management */
	LFBufferLookupEnt *entry_pool;		/* Pre-allocated entry pool */
	pg_atomic_uint32 pool_next;			/* Next free entry index */
	uint32		pool_size;				/* Total pool size */
	pg_atomic_uint64 free_list;			/* Head of free entry list */
	
	/* Epoch-based SMR */
	pg_atomic_uint64 global_epoch;		/* Global epoch counter */
	LFEpochEntry *thread_epochs;		/* Per-backend epoch tracking */
	
	/* Reclamation lists */
	LFReclaimNode **reclaim_lists;		/* Per-backend reclaim lists */
	LFReclaimNode *reclaim_pool;		/* Pool of reclaim nodes */
	pg_atomic_uint32 reclaim_pool_next;	/* Next free reclaim node */
	
	/* Statistics removed for performance */
} LFBufTable;

/*
 * Global lock-free buffer table instance
 */
extern LFBufTable *LFSharedBufTable;

/*
 * Function prototypes
 */

/* Table initialization and management */
extern Size LFBufTableShmemSize(int size);
extern void LFInitBufTable(int size);

/* Core operations (lock-free) */
extern uint32 LFBufTableHashCode(BufferTag *tagPtr);
extern int LFBufTableLookup(BufferTag *tagPtr, uint32 hashcode) pg_attribute_hot;
extern int LFBufTableInsert(BufferTag *tagPtr, uint32 hashcode, int buf_id) pg_attribute_hot;
extern void LFBufTableDelete(BufferTag *tagPtr, uint32 hashcode);

/* Epoch-based SMR operations */
extern void LFEnterEpoch(void);
extern void LFExitEpoch(void);
extern void LFAdvanceEpoch(void);
extern void LFReclaimMemory(void);

/* Inline versions for hot paths */
static inline void
LFEnterEpochInline(void)
{
	/* Simplified inline version without checks */
	extern int MyProcNumber;
	static __thread int cached_backend_id = -1;
	static __thread bool cached = false;
	uint64 epoch;
	
	if (!cached)
	{
		cached_backend_id = MyProcNumber;
		cached = true;
	}
	
	epoch = pg_atomic_read_u64(&LFSharedBufTable->global_epoch);
	pg_atomic_write_u64(&LFSharedBufTable->thread_epochs[cached_backend_id].epoch, epoch);
	pg_write_barrier();  /* Only need write barrier here */
}

static inline void
LFExitEpochInline(void)
{
	/* Simplified inline version */
	extern int MyProcNumber;
	static __thread int cached_backend_id = -1;
	static __thread bool cached = false;
	
	if (!cached)
	{
		cached_backend_id = MyProcNumber;
		cached = true;
	}
	
	pg_read_barrier();  /* Ensure reads complete before epoch exit */
	pg_atomic_write_u64(&LFSharedBufTable->thread_epochs[cached_backend_id].epoch, LF_EPOCH_INVALID);
}

/* Memory management */
extern LFBufferLookupEnt *LFAllocateEntry(void);
extern void LFQueueForReclaim(LFBufferLookupEnt *entry);

/* Debugging and statistics */
extern void LFBufTableStats(void);

#endif							/* BUF_TABLE_LOCKFREE_H */