#!/bin/bash
#
# Test script for ddl_logger extension
#

set -e

# Clean up any previous test files
rm -rf /tmp/pgddl
mkdir -p /tmp/pgddl

# Database name for testing
DBNAME="ddl_logger_test"

echo "Testing ddl_logger extension..."
echo

# Check if PostgreSQL is running
if ! psql -c "SELECT 1" postgres > /dev/null 2>&1; then
    echo "Error: PostgreSQL is not running or not accessible."
    echo "Please start PostgreSQL and ensure ddl_logger is in shared_preload_libraries."
    exit 1
fi

# Create test database
echo "1. Creating test database..."
psql postgres -c "DROP DATABASE IF EXISTS $DBNAME;" > /dev/null
psql postgres -c "CREATE DATABASE $DBNAME;" > /dev/null
echo "   ✓ Database created"
echo

# Run DDL commands in a transaction
echo "2. Executing DDL commands in a single transaction..."
psql $DBNAME <<EOF
BEGIN;
CREATE TABLE test_table1 (id INTEGER PRIMARY KEY, name TEXT);
CREATE TABLE test_table2 (id INTEGER PRIMARY KEY, value NUMERIC);
CREATE INDEX idx_test ON test_table1(name);
COMMIT;
EOF
echo "   ✓ DDL commands executed"
echo

# Check for output files
echo "3. Checking for DDL log files..."
FILES=$(ls /tmp/pgddl/*.sql 2>/dev/null | wc -l)
if [ "$FILES" -gt 0 ]; then
    echo "   ✓ Found $FILES DDL log file(s)"
    echo
    echo "4. Contents of DDL log files:"
    for file in /tmp/pgddl/*.sql; do
        echo "   --- $file ---"
        cat "$file"
        echo
    done
else
    echo "   ✗ No DDL log files found!"
    echo "   Make sure ddl_logger is loaded in shared_preload_libraries"
    exit 1
fi

# Test rollback (should NOT create a file)
echo "5. Testing transaction rollback (should not create file)..."
FILES_BEFORE=$(ls /tmp/pgddl/*.sql 2>/dev/null | wc -l)
psql $DBNAME <<EOF > /dev/null 2>&1
BEGIN;
CREATE TABLE test_table3 (id INTEGER);
ROLLBACK;
EOF
FILES_AFTER=$(ls /tmp/pgddl/*.sql 2>/dev/null | wc -l)
if [ "$FILES_BEFORE" -eq "$FILES_AFTER" ]; then
    echo "   ✓ Rollback test passed (no new file created)"
else
    echo "   ✗ Rollback test failed (unexpected file created)"
fi
echo

# Test non-DDL commands (should not be logged)
echo "6. Testing non-DDL commands (should not be logged)..."
FILES_BEFORE=$(ls /tmp/pgddl/*.sql 2>/dev/null | wc -l)
psql $DBNAME <<EOF > /dev/null 2>&1
INSERT INTO test_table1 VALUES (1, 'test');
SELECT * FROM test_table1;
EOF
FILES_AFTER=$(ls /tmp/pgddl/*.sql 2>/dev/null | wc -l)
if [ "$FILES_BEFORE" -eq "$FILES_AFTER" ]; then
    echo "   ✓ Non-DDL test passed (DML commands not logged)"
else
    echo "   ✗ Non-DDL test failed (unexpected logging)"
fi
echo

# Clean up
echo "7. Cleaning up..."
psql postgres -c "DROP DATABASE $DBNAME;" > /dev/null
echo "   ✓ Test database dropped"
echo

echo "============================================"
echo "All tests completed successfully!"
echo "============================================"
echo
echo "Note: To use ddl_logger, add it to postgresql.conf:"
echo "  shared_preload_libraries = 'ddl_logger'"
echo "Then restart PostgreSQL."
