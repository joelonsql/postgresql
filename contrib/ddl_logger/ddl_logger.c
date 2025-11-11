/*-------------------------------------------------------------------------
 *
 * ddl_logger.c
 *		Logs successfully executed DDL commands to /tmp/pgddl/[xid].sql
 *
 * This module captures all DDL commands executed in a transaction and
 * writes them to a file named by the transaction ID when the transaction
 * commits. If the transaction rolls back, no file is created.
 *
 * Copyright (c) 2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  contrib/ddl_logger/ddl_logger.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <sys/stat.h>
#include <unistd.h>

#include "access/xact.h"
#include "commands/event_trigger.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "tcop/cmdtag.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

PG_MODULE_MAGIC_EXT(
					.name = "ddl_logger",
					.version = PG_VERSION
);

/*
 * Structure to hold a DDL command that needs to be logged
 */
typedef struct DDLCommand
{
	char	   *query_string;		/* The SQL text of the DDL command */
	struct DDLCommand *next;		/* Next command in the list */
} DDLCommand;

/*
 * Transaction-local list of DDL commands pending write
 * This will be automatically freed when the transaction ends
 */
static DDLCommand *pending_ddl_commands = NULL;

/* Saved hook values in case of hook stacking */
static ProcessUtility_hook_type prev_ProcessUtility_hook = NULL;

/*
 * ensure_ddl_directory
 *
 * Ensure the /tmp/pgddl directory exists, creating it if necessary.
 * Raises an error if directory cannot be created.
 */
static void
ensure_ddl_directory(void)
{
	struct stat st;
	const char *dir_path = "/tmp/pgddl";

	/* Check if directory exists */
	if (stat(dir_path, &st) == 0)
	{
		/* Path exists, verify it's a directory */
		if (!S_ISDIR(st.st_mode))
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("path \"%s\" exists but is not a directory", dir_path)));
		return;
	}

	/* Directory doesn't exist, create it */
	if (mkdir(dir_path, 0755) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create directory \"%s\": %m", dir_path)));
}

/*
 * write_ddl_file
 *
 * Write all pending DDL commands to a file named by the transaction ID.
 * Raises an error if the write fails, which will cause the transaction to abort.
 */
static void
write_ddl_file(TransactionId xid, DDLCommand *commands)
{
	char		filename[MAXPGPATH];
	FILE	   *file;
	DDLCommand *cmd;

	/* Construct the filename */
	snprintf(filename, MAXPGPATH, "/tmp/pgddl/%u.sql", xid);

	/* Open the file for writing */
	file = fopen(filename, "w");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open file \"%s\" for writing: %m", filename)));

	/* Write each DDL command */
	for (cmd = commands; cmd != NULL; cmd = cmd->next)
	{
		if (fprintf(file, "%s\n\n", cmd->query_string) < 0)
		{
			int			save_errno = errno;

			fclose(file);
			errno = save_errno;
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not write to file \"%s\": %m", filename)));
		}
	}

	/* Close the file */
	if (fclose(file) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not close file \"%s\": %m", filename)));
}

/*
 * ddl_logger_xact_callback
 *
 * Transaction callback to write DDL commands on commit or clean up on abort.
 */
static void
ddl_logger_xact_callback(XactEvent event, void *arg)
{
	if (event == XACT_EVENT_PRE_COMMIT)
	{
		/*
		 * If we have pending DDL commands, write them to a file.
		 * Do this at PRE_COMMIT so that any errors will abort the transaction.
		 */
		if (pending_ddl_commands != NULL)
		{
			TransactionId xid = GetCurrentTransactionId();

			/* Ensure the output directory exists */
			ensure_ddl_directory();

			/* Write the DDL commands to the file */
			write_ddl_file(xid, pending_ddl_commands);

			/*
			 * Clear the list. The memory will be freed automatically
			 * when CurTransactionContext is destroyed.
			 */
			pending_ddl_commands = NULL;
		}
	}
	else if (event == XACT_EVENT_ABORT)
	{
		/*
		 * Transaction aborted, clear the pending list.
		 * Memory will be freed automatically by context destruction.
		 */
		pending_ddl_commands = NULL;
	}
}

/*
 * ddl_logger_ProcessUtility
 *
 * Hook for ProcessUtility to capture DDL commands.
 */
static void
ddl_logger_ProcessUtility(PlannedStmt *pstmt,
						  const char *queryString,
						  bool readOnlyTree,
						  ProcessUtilityContext context,
						  ParamListInfo params,
						  QueryEnvironment *queryEnv,
						  DestReceiver *dest,
						  QueryCompletion *qc)
{
	Node	   *parsetree = pstmt->utilityStmt;
	CommandTag	tag;

	/*
	 * First, call the standard ProcessUtility or any previously-installed hook.
	 * We want to capture the command only if it executes successfully.
	 */
	PG_TRY();
	{
		if (prev_ProcessUtility_hook)
			prev_ProcessUtility_hook(pstmt, queryString, readOnlyTree,
									 context, params, queryEnv,
									 dest, qc);
		else
			standard_ProcessUtility(pstmt, queryString, readOnlyTree,
									context, params, queryEnv,
									dest, qc);
	}
	PG_CATCH();
	{
		/* Command failed, re-throw the error without logging */
		PG_RE_THROW();
	}
	PG_END_TRY();

	/*
	 * Command executed successfully. Check if it's a DDL command
	 * that should be logged.
	 */
	tag = CreateCommandTag(parsetree);

	if (command_tag_event_trigger_ok(tag))
	{
		/*
		 * This is a DDL command. Store it for later writing.
		 * Skip subcommands to avoid duplicate logging (e.g., constraints
		 * within CREATE TABLE trigger ProcessUtility recursively).
		 * Use CurTransactionContext so it's automatically freed on commit/abort.
		 */
		if (context != PROCESS_UTILITY_SUBCOMMAND &&
			queryString != NULL && queryString[0] != '\0')
		{
			MemoryContext oldcontext;
			DDLCommand *new_cmd;

			oldcontext = MemoryContextSwitchTo(CurTransactionContext);

			new_cmd = (DDLCommand *) palloc(sizeof(DDLCommand));
			new_cmd->query_string = pstrdup(queryString);
			new_cmd->next = pending_ddl_commands;
			pending_ddl_commands = new_cmd;

			MemoryContextSwitchTo(oldcontext);
		}
	}
}

/*
 * Module initialization function
 */
void
_PG_init(void)
{
	/* Register the transaction callback */
	RegisterXactCallback(ddl_logger_xact_callback, NULL);

	/* Install the ProcessUtility hook */
	prev_ProcessUtility_hook = ProcessUtility_hook;
	ProcessUtility_hook = ddl_logger_ProcessUtility;
}
