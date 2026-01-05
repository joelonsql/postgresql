/*-------------------------------------------------------------------------
 *
 * posix_shmem.c
 *	  Implement shared memory using POSIX and anonymous mmap facilities
 *
 * This implementation uses anonymous mmap() for the main shared memory
 * segment in non-EXEC_BACKEND mode, and POSIX shared memory (shm_open)
 * for EXEC_BACKEND mode where memory must survive exec().
 *
 * Unlike the former sysv_shmem.c, we do not use System V shared memory
 * at all.  Instead of relying on shm_nattch to detect attached processes,
 * we check if the postmaster process is still alive using kill(pid, 0).
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/port/posix_shmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <signal.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef EXEC_BACKEND
#include <sys/types.h>
#endif

#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "portability/mem.h"
#include "storage/dsm.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/pg_shmem.h"
#include "utils/guc.h"
#include "utils/guc_hooks.h"
#include "utils/pidfile.h"


/*
 * In non-EXEC_BACKEND mode, we use anonymous mmap for the entire shared
 * memory segment.  This is simple and efficient, and avoids all System V
 * shared memory limits.
 *
 * In EXEC_BACKEND mode, we must use named POSIX shared memory (shm_open)
 * because anonymous mmap does not survive exec().  The segment name is
 * derived from the data directory's inode to ensure uniqueness.
 */

unsigned long UsedShmemSegID = 0;
void	   *UsedShmemSegAddr = NULL;

static Size AnonymousShmemSize;
static void *AnonymousShmem = NULL;

#ifdef EXEC_BACKEND
/* Name of POSIX shared memory segment, for reattaching after exec */
static char PosixShmemName[64];
static Size PosixShmemSize;
#endif


/*
 * Generate a name for the POSIX shared memory segment based on the data
 * directory's inode.  This ensures different data directories use different
 * segment names.
 */
static void
GeneratePosixShmemName(char *name, size_t namelen, ino_t inode)
{
	/*
	 * POSIX shared memory names must start with a slash and should not
	 * contain any other slashes.  We use the inode to make the name unique
	 * per data directory.
	 */
	snprintf(name, namelen, "/PostgreSQL.%llu", (unsigned long long) inode);
}


/*
 * Identify the huge page size to use, and compute the related mmap flags.
 *
 * Some Linux kernel versions have a bug causing mmap() to fail on requests
 * that are not a multiple of the hugepage size.  Versions without that bug
 * instead silently round the request up to the next hugepage multiple ---
 * and then munmap() fails when we give it a size different from that.
 * So we have to round our request up to a multiple of the actual hugepage
 * size to avoid trouble.
 *
 * Doing the round-up ourselves also lets us make use of the extra memory,
 * rather than just wasting it.  Currently, we just increase the available
 * space recorded in the shmem header, which will make the extra usable for
 * purposes such as additional locktable entries.  Someday, for very large
 * hugepage sizes, we might want to think about more invasive strategies,
 * such as increasing shared_buffers to absorb the extra space.
 *
 * Returns the (real, assumed or config provided) page size into
 * *hugepagesize, and the hugepage-related mmap flags to use into
 * *mmap_flags if requested by the caller.  If huge pages are not supported,
 * *hugepagesize and *mmap_flags are set to 0.
 */
void
GetHugePageSize(Size *hugepagesize, int *mmap_flags)
{
#ifdef MAP_HUGETLB

	Size		default_hugepagesize = 0;
	Size		hugepagesize_local = 0;
	int			mmap_flags_local = 0;

	/*
	 * System-dependent code to find out the default huge page size.
	 *
	 * On Linux, read /proc/meminfo looking for a line like "Hugepagesize:
	 * nnnn kB".  Ignore any failures, falling back to the preset default.
	 */
#ifdef __linux__

	{
		FILE	   *fp = AllocateFile("/proc/meminfo", "r");
		char		buf[128];
		unsigned int sz;
		char		ch;

		if (fp)
		{
			while (fgets(buf, sizeof(buf), fp))
			{
				if (sscanf(buf, "Hugepagesize: %u %c", &sz, &ch) == 2)
				{
					if (ch == 'k')
					{
						default_hugepagesize = sz * (Size) 1024;
						break;
					}
					/* We could accept other units besides kB, if needed */
				}
			}
			FreeFile(fp);
		}
	}
#endif							/* __linux__ */

	if (huge_page_size != 0)
	{
		/* If huge page size is requested explicitly, use that. */
		hugepagesize_local = (Size) huge_page_size * 1024;
	}
	else if (default_hugepagesize != 0)
	{
		/* Otherwise use the system default, if we have it. */
		hugepagesize_local = default_hugepagesize;
	}
	else
	{
		/*
		 * If we fail to find out the system's default huge page size, or no
		 * huge page size is requested explicitly, assume it is 2MB. This will
		 * work fine when the actual size is less.  If it's more, we might get
		 * mmap() or munmap() failures due to unaligned requests; but at this
		 * writing, there are no reports of any non-Linux systems being picky
		 * about that.
		 */
		hugepagesize_local = 2 * 1024 * 1024;
	}

	mmap_flags_local = MAP_HUGETLB;

	/*
	 * On recent enough Linux, also include the explicit page size, if
	 * necessary.
	 */
#if defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT)
	if (hugepagesize_local != default_hugepagesize)
	{
		int			shift = pg_ceil_log2_64(hugepagesize_local);

		mmap_flags_local |= (shift & MAP_HUGE_MASK) << MAP_HUGE_SHIFT;
	}
#endif

	/* assign the results found */
	if (mmap_flags)
		*mmap_flags = mmap_flags_local;
	if (hugepagesize)
		*hugepagesize = hugepagesize_local;

#else

	if (hugepagesize)
		*hugepagesize = 0;
	if (mmap_flags)
		*mmap_flags = 0;

#endif							/* MAP_HUGETLB */
}

/*
 * GUC check_hook for huge_page_size
 */
bool
check_huge_page_size(int *newval, void **extra, GucSource source)
{
#if !(defined(MAP_HUGE_MASK) && defined(MAP_HUGE_SHIFT))
	/* Recent enough Linux only, for now.  See GetHugePageSize(). */
	if (*newval != 0)
	{
		GUC_check_errdetail("\"huge_page_size\" must be 0 on this platform.");
		return false;
	}
#endif
	return true;
}

/*
 * Creates an anonymous mmap()ed shared memory segment.
 *
 * Pass the requested size in *size.  This function will modify *size to the
 * actual size of the allocation, if it ends up allocating a segment that is
 * larger than requested.
 */
static void *
CreateAnonymousSegment(Size *size)
{
	Size		allocsize = *size;
	void	   *ptr = MAP_FAILED;
	int			mmap_errno = 0;

#ifndef MAP_HUGETLB
	/* PGSharedMemoryCreate should have dealt with this case */
	Assert(huge_pages != HUGE_PAGES_ON);
#else
	if (huge_pages == HUGE_PAGES_ON || huge_pages == HUGE_PAGES_TRY)
	{
		/*
		 * Round up the request size to a suitable large value.
		 */
		Size		hugepagesize;
		int			mmap_flags;

		GetHugePageSize(&hugepagesize, &mmap_flags);

		if (allocsize % hugepagesize != 0)
			allocsize += hugepagesize - (allocsize % hugepagesize);

		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS | mmap_flags, -1, 0);
		mmap_errno = errno;
		if (huge_pages == HUGE_PAGES_TRY && ptr == MAP_FAILED)
			elog(DEBUG1, "mmap(%zu) with MAP_HUGETLB failed, huge pages disabled: %m",
				 allocsize);
	}
#endif

	/*
	 * Report whether huge pages are in use.  This needs to be tracked before
	 * the second mmap() call if attempting to use huge pages failed
	 * previously.
	 */
	SetConfigOption("huge_pages_status", (ptr == MAP_FAILED) ? "off" : "on",
					PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);

	if (ptr == MAP_FAILED && huge_pages != HUGE_PAGES_ON)
	{
		/*
		 * Use the original size, not the rounded-up value, when falling back
		 * to non-huge pages.
		 */
		allocsize = *size;
		ptr = mmap(NULL, allocsize, PROT_READ | PROT_WRITE,
				   PG_MMAP_FLAGS, -1, 0);
		mmap_errno = errno;
	}

	if (ptr == MAP_FAILED)
	{
		errno = mmap_errno;
		ereport(FATAL,
				(errmsg("could not map anonymous shared memory: %m"),
				 (mmap_errno == ENOMEM) ?
				 errhint("This error usually means that PostgreSQL's request "
						 "for a shared memory segment exceeded available memory, "
						 "swap space, or huge pages. To reduce the request size "
						 "(currently %zu bytes), reduce PostgreSQL's shared "
						 "memory usage, perhaps by reducing \"shared_buffers\" or "
						 "\"max_connections\".",
						 allocsize) : 0));
	}

	*size = allocsize;
	return ptr;
}

/*
 * AnonymousShmemDetach --- detach from an anonymous mmap'd block
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
AnonymousShmemDetach(int status, Datum arg)
{
	/* Release anonymous shared memory block, if any. */
	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
	}
}

#ifdef EXEC_BACKEND
/*
 * Create a POSIX shared memory segment using shm_open().
 * This is used in EXEC_BACKEND mode where we need named shared memory
 * that can be reattached after exec().
 */
static void *
CreatePosixSegment(const char *name, Size size, void *requestedAddress)
{
	int			fd;
	void	   *ptr;

	/* Create the POSIX shared memory object */
	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, PG_FILE_MODE_OWNER);
	if (fd < 0)
	{
		if (errno == EEXIST)
			return NULL;		/* Already exists, caller will handle */

		ereport(FATAL,
				(errmsg("could not create shared memory segment \"%s\": %m",
						name),
				 (errno == ENOSPC) ?
				 errhint("This error may indicate insufficient space in /dev/shm or "
						 "the POSIX shared memory filesystem.") : 0,
				 (errno == EACCES) ?
				 errhint("Check permissions on the POSIX shared memory filesystem.") : 0));
	}

	/* Set the segment size */
	if (ftruncate(fd, size) < 0)
	{
		int			save_errno = errno;

		close(fd);
		shm_unlink(name);
		errno = save_errno;
		ereport(FATAL,
				(errmsg("could not resize shared memory segment \"%s\" to %zu bytes: %m",
						name, size)));
	}

	/* Map the segment into our address space */
	ptr = mmap(requestedAddress, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED)
	{
		int			save_errno = errno;

		close(fd);
		shm_unlink(name);
		errno = save_errno;
		ereport(FATAL,
				(errmsg("could not map shared memory segment \"%s\": %m", name)));
	}

	/* Close the fd; the mapping keeps the segment alive */
	close(fd);

	return ptr;
}

/*
 * PosixShmemDetach --- unlink and unmap the POSIX shared memory segment
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
PosixShmemDetach(int status, Datum arg)
{
	/* Unlink the POSIX shared memory object */
	if (PosixShmemName[0] != '\0')
	{
		shm_unlink(PosixShmemName);
		PosixShmemName[0] = '\0';
	}
}

/*
 * PosixShmemUnmap --- unmap the POSIX shared memory segment
 * (called as an on_shmem_exit callback, hence funny argument list)
 */
static void
PosixShmemUnmap(int status, Datum shmaddr)
{
	if (DatumGetPointer(shmaddr) != NULL)
	{
		if (munmap(DatumGetPointer(shmaddr), PosixShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 DatumGetPointer(shmaddr), PosixShmemSize);
	}
}
#endif							/* EXEC_BACKEND */

/*
 * PGSharedMemoryIsInUse
 *
 * Is a previously-existing shmem segment still existing and in use?
 *
 * The point of this exercise is to detect the case where a prior postmaster
 * crashed, but it left child backends that are still running.  We check
 * if the postmaster process (stored in the shmem header) is still alive.
 *
 * The id1 and id2 parameters are unused in this implementation but kept
 * for API compatibility.
 */
bool
PGSharedMemoryIsInUse(unsigned long id1, unsigned long id2)
{
#ifdef EXEC_BACKEND
	char		name[64];
	int			fd;
	struct stat statbuf;
	PGShmemHeader header;
	ssize_t		nread;

	/*
	 * Get the data directory inode to construct the POSIX shm name.
	 */
	if (stat(DataDir, &statbuf) < 0)
		return false;			/* Can't stat data dir, assume not in use */

	GeneratePosixShmemName(name, sizeof(name), statbuf.st_ino);

	/* Try to open the existing POSIX shared memory segment */
	fd = shm_open(name, O_RDONLY, 0);
	if (fd < 0)
		return false;			/* Doesn't exist, not in use */

	/* Read the header to get the creator PID */
	nread = read(fd, &header, sizeof(header));
	close(fd);

	if (nread != sizeof(header))
		return false;			/* Can't read header, assume stale */

	/* Verify it's a valid PostgreSQL segment for this data directory */
	if (header.magic != PGShmemMagic ||
		header.device != statbuf.st_dev ||
		header.inode != statbuf.st_ino)
		return false;			/* Not our segment */

	/* Check if the creator process is still alive */
	if (header.creatorPID <= 0)
		return false;			/* Invalid PID */

	if (kill(header.creatorPID, 0) == 0)
		return true;			/* Process exists, segment in use */

	if (errno == ESRCH)
		return false;			/* Process doesn't exist, segment is stale */

	/* Other error (e.g., EPERM), assume in use to be safe */
	return true;
#else
	/*
	 * In non-EXEC_BACKEND mode, we use anonymous mmap which doesn't persist.
	 * The lockfile is the primary mechanism for detecting stale postmasters.
	 * Just check if there's a live process with the PID from the lockfile.
	 */
	return false;				/* Let the lockfile check handle this */
#endif
}

/*
 * PGSharedMemoryCreate
 *
 * Create a shared memory segment of the given size and initialize its
 * standard header.  Also, register an on_shmem_exit callback to release
 * the storage.
 *
 * In non-EXEC_BACKEND mode, we use anonymous mmap.
 * In EXEC_BACKEND mode, we use POSIX shared memory (shm_open).
 */
PGShmemHeader *
PGSharedMemoryCreate(Size size,
					 PGShmemHeader **shim)
{
	void	   *memAddress;
	PGShmemHeader *hdr;
	struct stat statbuf;

	/*
	 * We use the data directory's ID info (inode and device numbers) to
	 * positively identify shmem segments associated with this data dir.
	 */
	if (stat(DataDir, &statbuf) < 0)
		ereport(FATAL,
				(errcode_for_file_access(),
				 errmsg("could not stat data directory \"%s\": %m",
						DataDir)));

	/* Complain if hugepages demanded but we can't possibly support them */
#if !defined(MAP_HUGETLB)
	if (huge_pages == HUGE_PAGES_ON)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("huge pages not supported on this platform")));
#endif

	/* Room for a header? */
	Assert(size > MAXALIGN(sizeof(PGShmemHeader)));

#ifdef EXEC_BACKEND
	{
		char	   *name;
		void	   *requestedAddress = NULL;
		char	   *pg_shmem_addr = getenv("PG_SHMEM_ADDR");

		if (pg_shmem_addr)
			requestedAddress = (void *) strtoul(pg_shmem_addr, NULL, 0);
		else
		{
#if defined(__darwin__) && SIZEOF_VOID_P == 8
			/*
			 * Provide a default value that is believed to avoid problems with
			 * ASLR on the current macOS release.
			 */
			requestedAddress = (void *) 0x80000000000;
#endif
		}

		/* Generate the POSIX shared memory name */
		GeneratePosixShmemName(PosixShmemName, sizeof(PosixShmemName),
							   statbuf.st_ino);
		name = PosixShmemName;

		/*
		 * First, try to clean up any stale segment from a previous crash.
		 */
		if (PGSharedMemoryIsInUse(0, 0))
		{
			ereport(FATAL,
					(errcode(ERRCODE_LOCK_FILE_EXISTS),
					 errmsg("pre-existing shared memory block is still in use"),
					 errhint("Terminate any old server processes associated with data directory \"%s\".",
							 DataDir)));
		}

		/* Try to unlink any stale segment */
		shm_unlink(name);

		/* Create the new segment */
		memAddress = CreatePosixSegment(name, size, requestedAddress);
		if (memAddress == NULL)
		{
			/* Should not happen after we unlinked, but handle it */
			ereport(FATAL,
					(errcode(ERRCODE_LOCK_FILE_EXISTS),
					 errmsg("could not create shared memory segment")));
		}

		PosixShmemSize = size;

		/* Register on-exit routines */
		on_shmem_exit(PosixShmemDetach, (Datum) 0);
		on_shmem_exit(PosixShmemUnmap, PointerGetDatum(memAddress));

		/*
		 * Store shmem info in data directory lockfile.
		 * Use inode as the "key" for display purposes.
		 */
		{
			char		line[64];

			sprintf(line, "%9lu %9lu",
					(unsigned long) statbuf.st_ino,
					(unsigned long) 0);
			AddToDataDirLockFile(LOCK_FILE_LINE_SHMEM_KEY, line);
		}

		/* No huge pages support in EXEC_BACKEND POSIX shm mode yet */
		SetConfigOption("huge_pages_status", "off",
						PGC_INTERNAL, PGC_S_DYNAMIC_DEFAULT);
	}
#else							/* !EXEC_BACKEND */
	{
		/*
		 * Use anonymous mmap for the shared memory segment.
		 */
		AnonymousShmem = CreateAnonymousSegment(&size);
		AnonymousShmemSize = size;
		memAddress = AnonymousShmem;

		/* Register on-exit routine to unmap the anonymous segment */
		on_shmem_exit(AnonymousShmemDetach, (Datum) 0);

		/*
		 * Store placeholder info in data directory lockfile.
		 */
		{
			char		line[64];

			sprintf(line, "%9lu %9lu",
					(unsigned long) statbuf.st_ino,
					(unsigned long) 0);
			AddToDataDirLockFile(LOCK_FILE_LINE_SHMEM_KEY, line);
		}
	}
#endif							/* EXEC_BACKEND */

	/* Initialize new segment. */
	hdr = (PGShmemHeader *) memAddress;
	hdr->creatorPID = getpid();
	hdr->magic = PGShmemMagic;
	hdr->dsm_control = 0;

	/* Fill in the data directory ID info, too */
	hdr->device = statbuf.st_dev;
	hdr->inode = statbuf.st_ino;

	/*
	 * Initialize space allocation status for segment.
	 */
	hdr->totalsize = size;
	hdr->freeoffset = MAXALIGN(sizeof(PGShmemHeader));
	*shim = hdr;

	/* Save info for possible future use */
	UsedShmemSegAddr = memAddress;
	UsedShmemSegID = (unsigned long) statbuf.st_ino;

	return hdr;
}

#ifdef EXEC_BACKEND

/*
 * PGSharedMemoryReAttach
 *
 * This is called during startup of a postmaster child process to re-attach to
 * an already existing shared memory segment.  This is needed only in the
 * EXEC_BACKEND case; otherwise postmaster children inherit the shared memory
 * segment attachment via fork().
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryReAttach(void)
{
	int			fd;
	void	   *ptr;
	char		name[64];
	struct stat statbuf;
	void	   *origUsedShmemSegAddr = UsedShmemSegAddr;
	PGShmemHeader *hdr;

	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc (currently) appears to not detach on exec. */
	PGSharedMemoryDetach();
	UsedShmemSegAddr = origUsedShmemSegAddr;
#endif

	/*
	 * Get the data directory inode to construct the POSIX shm name.
	 */
	if (stat(DataDir, &statbuf) < 0)
		elog(FATAL, "could not stat data directory \"%s\": %m", DataDir);

	GeneratePosixShmemName(name, sizeof(name), statbuf.st_ino);

	elog(DEBUG3, "attaching to POSIX shared memory \"%s\" at %p", name, UsedShmemSegAddr);

	/* Open the existing POSIX shared memory segment */
	fd = shm_open(name, O_RDWR, 0);
	if (fd < 0)
		elog(FATAL, "could not open shared memory segment \"%s\": %m", name);

	/* Get the size */
	if (fstat(fd, &statbuf) < 0)
	{
		close(fd);
		elog(FATAL, "could not stat shared memory segment \"%s\": %m", name);
	}
	PosixShmemSize = statbuf.st_size;

	/* Map it at the expected address */
	ptr = mmap(UsedShmemSegAddr, PosixShmemSize, PROT_READ | PROT_WRITE,
			   MAP_SHARED, fd, 0);
	close(fd);

	if (ptr == MAP_FAILED)
		elog(FATAL, "could not map shared memory segment \"%s\": %m", name);

	if (ptr != origUsedShmemSegAddr)
		elog(FATAL, "reattaching to shared memory returned unexpected address (got %p, expected %p)",
			 ptr, origUsedShmemSegAddr);

	hdr = (PGShmemHeader *) ptr;
	dsm_set_control_handle(hdr->dsm_control);

	UsedShmemSegAddr = ptr;		/* probably redundant */

	/* Remember the name for cleanup purposes */
	strlcpy(PosixShmemName, name, sizeof(PosixShmemName));
}

/*
 * PGSharedMemoryNoReAttach
 *
 * This is called during startup of a postmaster child process when we choose
 * *not* to re-attach to the existing shared memory segment.  We must clean up
 * to leave things in the appropriate state.  This is not used in the non
 * EXEC_BACKEND case, either.
 *
 * The child process startup logic might or might not call PGSharedMemoryDetach
 * after this; make sure that it will be a no-op if called.
 *
 * UsedShmemSegID and UsedShmemSegAddr are implicit parameters to this
 * routine.  The caller must have already restored them to the postmaster's
 * values.
 */
void
PGSharedMemoryNoReAttach(void)
{
	Assert(UsedShmemSegAddr != NULL);
	Assert(IsUnderPostmaster);

#ifdef __CYGWIN__
	/* cygipc (currently) appears to not detach on exec. */
	PGSharedMemoryDetach();
#endif

	/* For cleanliness, reset UsedShmemSegAddr to show we're not attached. */
	UsedShmemSegAddr = NULL;
	/* And the same for UsedShmemSegID. */
	UsedShmemSegID = 0;
	/* Clear the segment name too. */
	PosixShmemName[0] = '\0';
}

#endif							/* EXEC_BACKEND */

/*
 * PGSharedMemoryDetach
 *
 * Detach from the shared memory segment, if still attached.  This is not
 * intended to be called explicitly by the process that originally created the
 * segment (it will have on_shmem_exit callback(s) registered to do that).
 * Rather, this is for subprocesses that have inherited an attachment and want
 * to get rid of it.
 *
 * UsedShmemSegAddr is an implicit parameter to this routine,
 * also AnonymousShmem and AnonymousShmemSize.
 */
void
PGSharedMemoryDetach(void)
{
#ifdef EXEC_BACKEND
	if (UsedShmemSegAddr != NULL)
	{
		if (munmap(UsedShmemSegAddr, PosixShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 UsedShmemSegAddr, PosixShmemSize);
		UsedShmemSegAddr = NULL;
	}
#else
	if (AnonymousShmem != NULL)
	{
		if (munmap(AnonymousShmem, AnonymousShmemSize) < 0)
			elog(LOG, "munmap(%p, %zu) failed: %m",
				 AnonymousShmem, AnonymousShmemSize);
		AnonymousShmem = NULL;
		UsedShmemSegAddr = NULL;
	}
#endif
}
