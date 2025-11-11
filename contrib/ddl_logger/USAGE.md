# DDL Logger - Quick Start Guide

## Installation

The extension has been built and installed to `/Users/joel/pg19/lib/postgresql/ddl_logger.dylib`

## Configuration

To enable DDL logging, add the following to your `postgresql.conf`:

```
shared_preload_libraries = 'ddl_logger'
```

Then restart PostgreSQL.

## Testing

Here's a quick manual test you can run:

### 1. Start PostgreSQL with ddl_logger loaded

Make sure your `postgresql.conf` has:
```
shared_preload_libraries = 'ddl_logger'
```

Then restart PostgreSQL.

### 2. Create the output directory

```bash
mkdir -p /tmp/pgddl
```

### 3. Run some DDL commands

Connect to your database and execute:

```sql
-- Start a transaction
BEGIN;

-- Execute some DDL
CREATE TABLE test_users (
    id SERIAL PRIMARY KEY,
    username TEXT NOT NULL,
    email TEXT
);

CREATE INDEX idx_username ON test_users(username);

ALTER TABLE test_users ADD COLUMN created_at TIMESTAMP DEFAULT NOW();

-- Commit the transaction
COMMIT;
```

### 4. Check the output

Look in `/tmp/pgddl/` for a file named `[xid].sql` where `[xid]` is the transaction ID:

```bash
ls -la /tmp/pgddl/
cat /tmp/pgddl/*.sql
```

You should see all three DDL commands in the file.

### 5. Verify rollback behavior

```sql
BEGIN;
CREATE TABLE test_rollback (id INTEGER);
ROLLBACK;
```

Check `/tmp/pgddl/` again - there should be NO new file because the transaction was rolled back.

## Example Output

A typical DDL log file (`/tmp/pgddl/12345.sql`) might look like:

```sql
CREATE TABLE test_users (
    id SERIAL PRIMARY KEY,
    username TEXT NOT NULL,
    email TEXT
);

CREATE INDEX idx_username ON test_users(username);

ALTER TABLE test_users ADD COLUMN created_at TIMESTAMP DEFAULT NOW();

```

## Verification Script

You can also check if the extension is loaded:

```sql
-- Check loaded libraries
SHOW shared_preload_libraries;

-- Check server log for ddl_logger initialization messages
-- (if you have logging enabled)
```

## Troubleshooting

**Extension not loading:**
- Verify `shared_preload_libraries = 'ddl_logger'` is in postgresql.conf
- Restart PostgreSQL (reload is not enough for shared_preload_libraries)
- Check PostgreSQL logs for errors

**No files being created:**
- Ensure `/tmp/pgddl/` directory exists and is writable
- The extension creates the directory automatically, but check permissions
- Verify you're actually executing DDL commands (CREATE, ALTER, DROP, etc.)
- DML commands (INSERT, UPDATE, DELETE, SELECT) are NOT logged

**Files created but empty:**
- This shouldn't happen, but if it does, check disk space
- Verify the queries are executing successfully before commit

## What Gets Logged

✓ **Logged (DDL commands):**
- CREATE TABLE, CREATE INDEX, CREATE VIEW, etc.
- ALTER TABLE, ALTER INDEX, etc.
- DROP TABLE, DROP INDEX, etc.
- CREATE/DROP DATABASE, SCHEMA, FUNCTION, etc.
- GRANT, REVOKE
- And all other DDL commands

✗ **Not logged:**
- SELECT, INSERT, UPDATE, DELETE (DML)
- BEGIN, COMMIT, ROLLBACK (transaction control)
- SET statements
- VACUUM, ANALYZE (maintenance)
