# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build and Test Commands

```bash
# Build (from repo root)
make -j33

# Copy built postgres binary and restart server
cp ./src/backend/postgres /Users/joel/pg/bin/postgres
/Users/joel/pg/bin/pg_ctl -D /Users/joel/pg-data -l /Users/joel/pg.log restart

# Run regression tests
make check

# Run a single regression test
./src/test/regress/pg_regress --inputdir=src/test/regress --bindir=/Users/joel/pg/bin --schedule=src/test/regress/parallel_schedule numeric
# Or for a quick single-file test:
./src/test/regress/pg_regress --inputdir=src/test/regress --bindir=/Users/joel/pg/bin numeric

# Full test suite
make check-world
```

## Git Commit Messages

Use PostgreSQL commit style. Do not mention Claude or add Co-Authored-By lines.

Format:
```
Short summary in imperative mood (e.g., "Fix crash in...")

Detailed explanation of what and why. Can be multiple paragraphs.

Discussion: https://www.postgresql.org/message-id/...
```

## Code Formatting

Run pgindent before commits:
```bash
src/tools/pgindent/pgindent .
```

Requires `pg_bsd_indent` in PATH (build from `src/tools/pg_bsd_indent`).

To protect comment blocks from reformatting, use dashes:
```c
/*----------
 * Text here will not be touched by pgindent.
 */
```

## Architecture Overview

### Query Processing Path

1. **Parser** (`src/backend/parser/`) - SQL text → parse tree (uses flex/bison: `scan.l`, `gram.y`)
2. **Rewrite** (`src/backend/rewrite/`) - applies rules and view expansion
3. **Planner/Optimizer** (`src/backend/optimizer/`) - parse tree → executable plan (cost-based)
4. **Executor** (`src/backend/executor/`) - runs the plan, returns results

### Key Backend Directories

- `src/backend/access/` - storage access methods (heap, btree, gin, gist, brin, hash, spgist)
- `src/backend/catalog/` - system catalog management
- `src/backend/commands/` - SQL command implementations (DDL/DML)
- `src/backend/storage/` - buffer manager, lock manager, storage primitives
- `src/backend/utils/adt/` - built-in data type implementations (numeric, text, date, etc.)
- `src/backend/tcop/` - "traffic cop" - query dispatch and main processing loop

### Important Files

- `src/include/catalog/pg_*.h` - system catalog definitions
- `src/backend/utils/adt/numeric.c` - numeric type implementation
- `src/backend/access/nbtree/` - B-tree index implementation
- `src/backend/access/transam/xlog.c` - WAL (write-ahead log) core

### Regression Tests

- Test SQL: `src/test/regress/sql/*.sql`
- Expected output: `src/test/regress/expected/*.out`
- Test results appear in: `src/test/regress/results/`
- Parallel test schedule: `src/test/regress/parallel_schedule`

### Developer Resources

- Wiki: http://wiki.postgresql.org/wiki/Development_information
- Tools: `src/tools/` (pgindent, copyright, typedef management, etc.)
- Module READMEs: scattered throughout `src/backend/*/README`
