/*-------------------------------------------------------------------------
 *
 * pg_backup_split.c
 *
 *	A split format dump is a directory containing:
 *	- A "toc.dat" file for the TOC (pure metadata without SQL)
 *	- Individual .sql files for each database object
 *	- Files organized as: [type]/[schema]/[name]-[hash].sql
 *	- Hash is first 32 chars of SHA256 of object's canonical address
 *
 *	Each .sql file contains:
 *	- For schema objects: Complete CREATE statements
 *	- For tables: CREATE TABLE + COPY data (or split based on -a/-s flags)
 *	- For data-only dumps: Only COPY statements
 *	- For schema-only dumps: Only CREATE statements
 *
 *	This format supports parallel dumping and restoration via pg_restore.
 *
 *	Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 *	Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *		src/bin/pg_dump/pg_backup_split.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <sys/stat.h>

#include "common/file_utils.h"
#include "common/file_perm.h"
#include "common/cryptohash.h"
#include "common/sha2.h"
#include "compress_io.h"
#include "dumputils.h"
#include "parallel.h"
#include "pg_backup_utils.h"

typedef struct
{
	char	   *directory;		/* Root output directory */
	CompressFileHandle *dataFH; /* Currently open data file */
	CompressFileHandle *LOsTocFH;	/* File handle for blobs TOC */
	ParallelState *pstate;		/* For parallel backup/restore */
} lclContext;

typedef struct
{
	char	   *filename;		/* Relative path to the .sql file */
} lclTocEntry;

/* prototypes for private functions */
static void _ArchiveEntry(ArchiveHandle *AH, TocEntry *te);
static void _StartData(ArchiveHandle *AH, TocEntry *te);
static void _EndData(ArchiveHandle *AH, TocEntry *te);
static void _WriteData(ArchiveHandle *AH, const void *data, size_t dLen);
static int	_WriteByte(ArchiveHandle *AH, const int i);
static int	_ReadByte(ArchiveHandle *AH);
static void _WriteBuf(ArchiveHandle *AH, const void *buf, size_t len);
static void _ReadBuf(ArchiveHandle *AH, void *buf, size_t len);
static void _CloseArchive(ArchiveHandle *AH);
static void _ReopenArchive(ArchiveHandle *AH);
static void _PrintTocData(ArchiveHandle *AH, TocEntry *te);

static void _WriteExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _ReadExtraToc(ArchiveHandle *AH, TocEntry *te);
static void _PrintExtraToc(ArchiveHandle *AH, TocEntry *te);

static void _StartLOs(ArchiveHandle *AH, TocEntry *te);
static void _StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid);
static void _EndLOs(ArchiveHandle *AH, TocEntry *te);

static void _PrepParallelRestore(ArchiveHandle *AH);
static void _Clone(ArchiveHandle *AH);
static void _DeClone(ArchiveHandle *AH);

static int	_WorkerJobRestoreSplit(ArchiveHandle *AH, TocEntry *te);
static int	_WorkerJobDumpSplit(ArchiveHandle *AH, TocEntry *te);

static void setFilePath(ArchiveHandle *AH, char *buf, const char *relativeFilename);
static char *createSplitFilePath(ArchiveHandle *AH, TocEntry *te);
static char *sanitizePathSegment(const char *str);
static char *computeObjectHash(ArchiveHandle *AH, TocEntry *te);

/*
 * Initialize the split format
 */
void
InitArchiveFmt_Split(ArchiveHandle *AH)
{
	lclContext *ctx;

	/* Set function pointers */
	AH->ArchiveEntryPtr = _ArchiveEntry;
	AH->StartDataPtr = _StartData;
	AH->WriteDataPtr = _WriteData;
	AH->EndDataPtr = _EndData;
	AH->WriteBytePtr = _WriteByte;
	AH->ReadBytePtr = _ReadByte;
	AH->WriteBufPtr = _WriteBuf;
	AH->ReadBufPtr = _ReadBuf;
	AH->ClosePtr = _CloseArchive;
	AH->ReopenPtr = _ReopenArchive;
	AH->PrintTocDataPtr = _PrintTocData;
	AH->ReadExtraTocPtr = _ReadExtraToc;
	AH->WriteExtraTocPtr = _WriteExtraToc;
	AH->PrintExtraTocPtr = _PrintExtraToc;

	AH->StartLOsPtr = _StartLOs;
	AH->StartLOPtr = _StartLO;
	AH->EndLOPtr = _EndLO;
	AH->EndLOsPtr = _EndLOs;

	AH->PrepParallelRestorePtr = _PrepParallelRestore;
	AH->ClonePtr = _Clone;
	AH->DeClonePtr = _DeClone;

	AH->WorkerJobRestorePtr = _WorkerJobRestoreSplit;
	AH->WorkerJobDumpPtr = _WorkerJobDumpSplit;

	/* Set up our private context */
	ctx = (lclContext *) pg_malloc0(sizeof(lclContext));
	AH->formatData = ctx;

	ctx->dataFH = NULL;
	ctx->LOsTocFH = NULL;

	/* Open the output directory */
	if (AH->mode == archModeWrite)
	{
		struct stat st;
		bool		is_empty = false;

		/* If no directory was specified, error out */
		if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
			pg_fatal("no output directory specified");

		ctx->directory = AH->fSpec;

		/* Check if directory exists */
		if (stat(ctx->directory, &st) == 0)
		{
			if (!S_ISDIR(st.st_mode))
				pg_fatal("output directory \"%s\" exists but is not a directory",
						ctx->directory);

			/* Check if directory is empty */
			{
				DIR		   *dir;
				struct dirent *entry;
				
				dir = opendir(ctx->directory);
			if (!dir)
				pg_fatal("could not open directory \"%s\": %m", ctx->directory);

			is_empty = true;
			while ((entry = readdir(dir)) != NULL)
			{
				if (strcmp(entry->d_name, ".") != 0 &&
					strcmp(entry->d_name, "..") != 0)
				{
					is_empty = false;
					break;
				}
			}
			closedir(dir);

			if (!is_empty)
				pg_fatal("output directory \"%s\" is not empty", ctx->directory);
			}
		}
		else
		{
			/* Directory doesn't exist, create it */
			if (pg_mkdir_p(ctx->directory, pg_dir_create_mode) != 0)
				pg_fatal("could not create directory \"%s\": %m", ctx->directory);
		}

		{
			/* Create the TOC file */
			char		fname[MAXPGPATH];

			setFilePath(AH, fname, "toc.dat");

			ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

			if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
				pg_fatal("could not open output file \"%s\": %m", fname);

			/* The TOC is always created uncompressed */
			WriteHead(AH);
			WriteToc(AH);
			if (!EndCompressFileHandle(ctx->dataFH))
				pg_fatal("could not close TOC file: %m");
			ctx->dataFH = NULL;
		}
	}
	else
	{
		/* Read mode */
		char		fname[MAXPGPATH];

		if (!AH->fSpec || strcmp(AH->fSpec, "") == 0)
			pg_fatal("no input directory specified");

		ctx->directory = AH->fSpec;

		setFilePath(AH, fname, "toc.dat");

		ctx->dataFH = InitDiscoverCompressFileHandle(fname, PG_BINARY_R);
		if (ctx->dataFH == NULL)
			pg_fatal("could not open input file \"%s\": %m", fname);

		ReadHead(AH);
		ReadToc(AH);

		/* Close the TOC file, we'll open data files as needed */
		if (!EndCompressFileHandle(ctx->dataFH))
			pg_fatal("could not close TOC file: %m");
		ctx->dataFH = NULL;
	}
}

/*
 * Create the file path for a split format object
 * Returns: [type]/[schema]/[name]-[hash].sql or [type]/[name]-[hash].sql
 */
static char *
createSplitFilePath(ArchiveHandle *AH, TocEntry *te)
{
	char	   *type;
	char	   *schema = NULL;
	char	   *name;
	char	   *hash;
	char	   *filepath;
	char		dirpath[MAXPGPATH];
	lclContext *ctx = (lclContext *) AH->formatData;

	/* Get object type as lowercase string */
	type = sanitizePathSegment(te->desc);

	/* Get schema name if it exists */
	if (te->namespace && strlen(te->namespace) > 0)
		schema = sanitizePathSegment(te->namespace);
	else
		schema = NULL;

	/* Get object name - error if missing */
	if (te->tag && strlen(te->tag) > 0)
		name = sanitizePathSegment(te->tag);
	else
		pg_fatal("missing object name for %s", te->desc);

	/* Compute hash */
	hash = computeObjectHash(AH, te);

	/* Build the directory path based on whether we have a schema */
	if (schema)
	{
		snprintf(dirpath, sizeof(dirpath), "%s/%s/%s",
				ctx->directory, type, schema);
	}
	else
	{
		snprintf(dirpath, sizeof(dirpath), "%s/%s",
				ctx->directory, type);
	}

	/* Create directories if they don't exist */
	if (pg_mkdir_p(dirpath, pg_dir_create_mode) != 0)
		pg_fatal("could not create directory \"%s\": %m", dirpath);

	/* Build the full file path based on whether we have a schema */
	filepath = (char *) pg_malloc(MAXPGPATH);
	if (schema)
	{
		snprintf(filepath, MAXPGPATH, "%s/%s/%s-%s.sql",
				type, schema, name, hash);
	}
	else
	{
		snprintf(filepath, MAXPGPATH, "%s/%s-%s.sql",
				type, name, hash);
	}

	pg_free(type);
	if (schema)
		pg_free(schema);
	pg_free(name);
	pg_free(hash);

	return filepath;
}

/*
 * Sanitize a string for use as a path segment
 * Only allow lowercase letters, numbers, and the characters _, ., -
 */
static char *
sanitizePathSegment(const char *str)
{
	char	   *result;
	int			i, j;

	if (!str)
		return pg_strdup("unknown");

	result = pg_malloc(strlen(str) + 1);

	for (i = 0, j = 0; str[i] != '\0'; i++)
	{
		char		c = str[i];

		/* Convert to lowercase */
		if (c >= 'A' && c <= 'Z')
			c = c - 'A' + 'a';

		/* Keep only allowed characters */
		if ((c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') ||
			c == '_' || c == '.' || c == '-')
		{
			result[j++] = c;
		}
		else
		{
			result[j++] = '_';
		}
	}

	result[j] = '\0';

	/* Don't return empty string */
	if (j == 0)
	{
		pg_free(result);
		return pg_strdup("unnamed");
	}

	return result;
}

/*
 * Compute SHA256 hash for an object
 * Returns first 32 characters (128 bits) of the hash
 */
static char *
computeObjectHash(ArchiveHandle *AH, TocEntry *te)
{
	pg_cryptohash_ctx *ctx;
	unsigned char hash[PG_SHA256_DIGEST_LENGTH];
	char	   *result;
	int			i;

	/* Initialize SHA256 context */
	ctx = pg_cryptohash_create(PG_SHA256);
	if (!ctx)
		pg_fatal("could not create hash context");
	
	if (pg_cryptohash_init(ctx) < 0)
		pg_fatal("could not initialize hash context: %s", pg_cryptohash_error(ctx));

	/* Hash the object's canonical address components */
	if (te->catalogId.tableoid != 0)
	{
		if (pg_cryptohash_update(ctx, (uint8 *) &te->catalogId.tableoid, 
								sizeof(te->catalogId.tableoid)) < 0)
			pg_fatal("could not update hash: %s", pg_cryptohash_error(ctx));
	}
	if (te->catalogId.oid != 0)
	{
		if (pg_cryptohash_update(ctx, (uint8 *) &te->catalogId.oid,
								sizeof(te->catalogId.oid)) < 0)
			pg_fatal("could not update hash: %s", pg_cryptohash_error(ctx));
	}
	
	/* Also hash the type, namespace, and tag for uniqueness */
	if (te->desc)
	{
		if (pg_cryptohash_update(ctx, (uint8 *) te->desc, strlen(te->desc)) < 0)
			pg_fatal("could not update hash: %s", pg_cryptohash_error(ctx));
	}
	if (te->namespace)
	{
		if (pg_cryptohash_update(ctx, (uint8 *) te->namespace, strlen(te->namespace)) < 0)
			pg_fatal("could not update hash: %s", pg_cryptohash_error(ctx));
	}
	if (te->tag)
	{
		if (pg_cryptohash_update(ctx, (uint8 *) te->tag, strlen(te->tag)) < 0)
			pg_fatal("could not update hash: %s", pg_cryptohash_error(ctx));
	}

	/* Finalize the hash */
	if (pg_cryptohash_final(ctx, hash, PG_SHA256_DIGEST_LENGTH) < 0)
		pg_fatal("could not finalize hash: %s", pg_cryptohash_error(ctx));
	
	pg_cryptohash_free(ctx);

	/* Convert first 16 bytes (32 hex chars) to hex string */
	result = pg_malloc(33);
	for (i = 0; i < 16; i++)
		sprintf(result + i * 2, "%02x", hash[i]);
	result[32] = '\0';

	return result;
}

/*
 * Called for each TOC entry during dump
 */
static void
_ArchiveEntry(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx;
	char	   *filename;
	char		fullpath[MAXPGPATH];
	FILE	   *fp;
	const char *defn = NULL;

	tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	te->formatData = tctx;

	/* Generate the file path for this object */
	filename = createSplitFilePath(AH, te);
	tctx->filename = filename;

	/* For split format, write SQL definitions to individual files immediately */
	if (AH->mode == archModeWrite)
	{
		/* Get the SQL definition */
		if (te->defnDumper)
		{
			/* Call the defnDumper function to get the SQL */
			defn = te->defnDumper((Archive *) AH, te->defnDumperArg, te);
		}
		else if (te->defn && strlen(te->defn) > 0)
		{
			defn = te->defn;
		}

		/* Write definition to file if we have one and it's not just data */
		if (defn && strlen(defn) > 0 && strcmp(te->desc, "TABLE DATA") != 0)
		{
			setFilePath(AH, fullpath, tctx->filename);
			
			fp = fopen(fullpath, PG_BINARY_W);
			if (!fp)
				pg_fatal("could not open output file \"%s\": %m", fullpath);
			
			/* Write the SQL definition */
			if (fwrite(defn, 1, strlen(defn), fp) != strlen(defn))
				pg_fatal("could not write to output file \"%s\": %m", fullpath);
			
			/* Add newlines for readability */
			if (fwrite("\n\n", 1, 2, fp) != 2)
				pg_fatal("could not write to output file \"%s\": %m", fullpath);
			
			fclose(fp);
		}

		/* Free allocated definition if from defnDumper */
		if (te->defnDumper && defn)
			pg_free((void *)defn);
	}
}

/*
 * Start writing data for a TOC entry
 */
static void
_StartData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	char		fname[MAXPGPATH];
	FILE	   *fp;
	const char *defn = NULL;
	const char *mode;
	
	pg_log_debug("_StartData called for: desc='%s', tag='%s', reqs=%d", 
	             te->desc ? te->desc : "(null)", 
	             te->tag ? te->tag : "(null)",
	             te->reqs);

	/* Build full path to the file */
	setFilePath(AH, fname, tctx->filename);

	/* For TABLE DATA, we need to write CREATE TABLE first if not data-only mode */
	if (strcmp(te->desc, "TABLE DATA") == 0)
	{
		/* Check if we need to dump schema (not data-only mode) */
		if ((te->reqs & REQ_SCHEMA) != 0)
		{
			/* Get the CREATE TABLE statement */
			if (te->defnDumper)
			{
				defn = te->defnDumper((Archive *) AH, te->defnDumperArg, te);
			}
			else if (te->defn && strlen(te->defn) > 0)
			{
				defn = te->defn;
			}

			/* Write CREATE TABLE to file first */
			if (defn && strlen(defn) > 0)
			{
				fp = fopen(fname, PG_BINARY_W);
				if (!fp)
					pg_fatal("could not open output file \"%s\": %m", fname);
				
				/* Write the CREATE TABLE statement */
				if (fwrite(defn, 1, strlen(defn), fp) != strlen(defn))
					pg_fatal("could not write to output file \"%s\": %m", fname);
				
				/* Add newlines before COPY data */
				if (fwrite("\n\n", 1, 2, fp) != 2)
					pg_fatal("could not write to output file \"%s\": %m", fname);
				
				fclose(fp);

				/* Free allocated definition if from defnDumper */
				if (te->defnDumper && defn)
					pg_free((void *)defn);
			}
		}

		/* Open file for appending COPY data */
		ctx->dataFH = InitCompressFileHandle(AH->compression_spec);
		
		/* Open in append mode if we wrote CREATE TABLE */
		mode = (((te->reqs & REQ_SCHEMA) != 0) && defn) ? "ab" : PG_BINARY_W;
		if (!ctx->dataFH->open_write_func(fname, mode, ctx->dataFH))
			pg_fatal("could not open output file \"%s\": %m", fname);

		/* Write the COPY statement if we have one */
		if (te->copyStmt && strlen(te->copyStmt) > 0)
		{
			if (!ctx->dataFH->write_func(te->copyStmt, strlen(te->copyStmt), ctx->dataFH))
				pg_fatal("could not write to output file: %m");
			if (!ctx->dataFH->write_func("\n", 1, ctx->dataFH))
				pg_fatal("could not write to output file: %m");
		}
	}
	else
	{
		/* For non-TABLE DATA entries, just open normally */
		ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

		if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
			pg_fatal("could not open output file \"%s\": %m", fname);
	}
}

/*
 * Write data to the current file
 */
static void
_WriteData(ArchiveHandle *AH, const void *data, size_t dLen)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;
	
	pg_log_debug("_WriteData called with %zu bytes", dLen);
	if (CFH == NULL)
		pg_log_debug("WARNING: dataFH is NULL in _WriteData");
	
	errno = 0;
	if (dLen > 0 && !CFH->write_func(data, dLen, CFH))
	{
		if (errno == 0)
			errno = ENOSPC;
		pg_fatal("could not write to output file: %m");
	}
}

/*
 * End writing data for a TOC entry
 */
static void
_EndData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	/* For TABLE DATA, write the COPY terminator */
	if (strcmp(te->desc, "TABLE DATA") == 0 && (te->reqs & REQ_DATA) != 0)
	{
		/* Write COPY terminator */
		if (!CFH->write_func("\\.\n", 3, CFH))
			pg_fatal("could not write to output file: %m");
	}

	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close data file: %m");

	ctx->dataFH = NULL;
}

/*
 * Print data for a TOC entry during restore
 */
static void
_PrintTocData(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;
	char		fname[MAXPGPATH];
	char		buf[4096];
	size_t		cnt;

	if (!tctx || !tctx->filename)
		return;

	/* Build full path to the file */
	setFilePath(AH, fname, tctx->filename);

	/* Open and read the file */
	ctx->dataFH = InitDiscoverCompressFileHandle(fname, PG_BINARY_R);
	if (ctx->dataFH == NULL)
		pg_fatal("could not open input file \"%s\": %m", fname);

	/* Copy the file contents to output */
	while (ctx->dataFH->read_func(buf, sizeof(buf), &cnt, ctx->dataFH) && cnt > 0)
		ahwrite(buf, 1, cnt, AH);

	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close data file \"%s\": %m", fname);

	ctx->dataFH = NULL;
}

/*
 * Write extra TOC information (filename)
 */
static void
_WriteExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (tctx && tctx->filename)
		WriteStr(AH, tctx->filename);
	else
		WriteStr(AH, "");
}

/*
 * Read extra TOC information (filename)
 */
static void
_ReadExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx;

	tctx = (lclTocEntry *) pg_malloc0(sizeof(lclTocEntry));
	tctx->filename = ReadStr(AH);
	te->formatData = tctx;
}

/*
 * Print extra TOC information
 */
static void
_PrintExtraToc(ArchiveHandle *AH, TocEntry *te)
{
	lclTocEntry *tctx = (lclTocEntry *) te->formatData;

	if (AH->public.verbose && tctx && tctx->filename)
		ahprintf(AH, "-- File: %s\n", tctx->filename);
}

/*
 * Start writing Large Objects section
 */
static void
_StartLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	/* Create blobs directory */
	char		blobsDir[MAXPGPATH];
	snprintf(blobsDir, sizeof(blobsDir), "%s/blobs", ctx->directory);
	
	if (pg_mkdir_p(blobsDir, pg_dir_create_mode) != 0)
		pg_fatal("could not create directory \"%s\": %m", blobsDir);

	/* Open the blobs TOC file */
	snprintf(fname, sizeof(fname), "%s/blobs.toc", ctx->directory);

	ctx->LOsTocFH = InitCompressFileHandle(AH->compression_spec);

	if (!ctx->LOsTocFH->open_write_func(fname, PG_BINARY_W, ctx->LOsTocFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * Start writing a specific Large Object
 */
static void
_StartLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char		fname[MAXPGPATH];

	snprintf(fname, sizeof(fname), "%s/blobs/blob_%u.dat", ctx->directory, oid);

	ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

	if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
		pg_fatal("could not open output file \"%s\": %m", fname);
}

/*
 * End writing a specific Large Object
 */
static void
_EndLO(ArchiveHandle *AH, TocEntry *te, Oid oid)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (!EndCompressFileHandle(ctx->dataFH))
		pg_fatal("could not close LO data file: %m");

	ctx->dataFH = NULL;

	/* Write the OID to the blobs TOC */
	if (ctx->LOsTocFH)
	{
		char		buf[50];
		int			len;

		len = snprintf(buf, sizeof(buf), "%u\n", oid);
		if (!ctx->LOsTocFH->write_func(buf, len, ctx->LOsTocFH))
			pg_fatal("could not write to LOs TOC file: %m");
	}
}

/*
 * End writing Large Objects section
 */
static void
_EndLOs(ArchiveHandle *AH, TocEntry *te)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (ctx->LOsTocFH)
	{
		if (!EndCompressFileHandle(ctx->LOsTocFH))
			pg_fatal("could not close LOs TOC file: %m");
		ctx->LOsTocFH = NULL;
	}
}

/*
 * Clone for parallel restore
 */
static void
_Clone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	AH->formatData = pg_malloc(sizeof(lclContext));
	memcpy(AH->formatData, ctx, sizeof(lclContext));
	ctx = (lclContext *) AH->formatData;

	/*
	 * Each thread must have its own file handle, so close the parent's
	 * handle if any, and reset ours to NULL.
	 */
	ctx->dataFH = NULL;
	
	/* For safety */
	ctx->LOsTocFH = NULL;
}

/*
 * DeClone for parallel restore
 */
static void
_DeClone(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	ctx->dataFH = NULL;
	ctx->LOsTocFH = NULL;
	
	pg_free(ctx);
}

/*
 * Close the archive
 */
static void
_CloseArchive(ArchiveHandle *AH)
{
	lclContext *ctx = (lclContext *) AH->formatData;

	if (AH->mode == archModeWrite)
	{
		/* In write mode, rewrite the TOC file with updated information */
		char		fname[MAXPGPATH];

		setFilePath(AH, fname, "toc.dat");

		ctx->dataFH = InitCompressFileHandle(AH->compression_spec);

		if (!ctx->dataFH->open_write_func(fname, PG_BINARY_W, ctx->dataFH))
			pg_fatal("could not open output file \"%s\": %m", fname);

		WriteHead(AH);
		WriteToc(AH);
		AH->FH = NULL;
		if (!EndCompressFileHandle(ctx->dataFH))
			pg_fatal("could not close TOC file: %m");
	}

	ctx->dataFH = NULL;
	ctx->LOsTocFH = NULL;
}

/*
 * Reopen the archive
 */
static void
_ReopenArchive(ArchiveHandle *AH)
{
	/* Not needed for split format */
}

/*
 * Prepare for parallel restore
 */
static void
_PrepParallelRestore(ArchiveHandle *AH)
{
	/* Nothing specific needed for split format */
}

/*
 * Worker job for parallel dump
 */
static int
_WorkerJobDumpSplit(ArchiveHandle *AH, TocEntry *te)
{
	/* Write the data for this specific TOC entry */
	WriteDataChunksForTocEntry(AH, te);
	
	return 0;
}

/*
 * Worker job for parallel restore
 */
static int
_WorkerJobRestoreSplit(ArchiveHandle *AH, TocEntry *te)
{
	/* Restore this specific TOC entry */
	return parallel_restore(AH, te);
}

/*
 * Helper functions for byte I/O
 */
static int
_WriteByte(ArchiveHandle *AH, const int i)
{
	unsigned char c = (unsigned char) i;
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;
	
	if (!CFH->write_func(&c, 1, CFH))
		pg_fatal("could not write byte");

	return 1;
}

static int
_ReadByte(ArchiveHandle *AH)
{
	unsigned char c;
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;
	
	if (!CFH->read_func(&c, 1, NULL, CFH))
		pg_fatal("could not read byte");

	return (int) c;
}

static void
_WriteBuf(ArchiveHandle *AH, const void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	if (!CFH->write_func(buf, len, CFH))
		pg_fatal("could not write to output file");
}

static void
_ReadBuf(ArchiveHandle *AH, void *buf, size_t len)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	CompressFileHandle *CFH = ctx->dataFH;

	if (!CFH->read_func(buf, len, NULL, CFH))
		pg_fatal("could not read from input file");
}

/*
 * Helper to build full file paths
 */
static void
setFilePath(ArchiveHandle *AH, char *buf, const char *relativeFilename)
{
	lclContext *ctx = (lclContext *) AH->formatData;
	char	   *dname = ctx->directory;

	if (strlen(dname) + strlen(relativeFilename) + 2 > MAXPGPATH)
		pg_fatal("path name too long");

	strcpy(buf, dname);
	if (buf[strlen(buf) - 1] != '/')
		strcat(buf, "/");
	strcat(buf, relativeFilename);
}