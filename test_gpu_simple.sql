-- Simple GPU Sort Performance Test
-- Single test that should show clear GPU acceleration

SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '10GB';

\echo '=== GPU Sort Performance Test ==='
\echo 'Testing 100 million random integers'
\echo ''

-- Create test data
CREATE TABLE sort_test AS
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 100000000);

ANALYZE sort_test;

\echo 'Dataset created: 100 million rows'
\echo ''

-- Disable debug output for clean results
-- SET client_min_messages = WARNING;

-- Run the sort test
\timing on
\echo 'Running sort...'

EXPLAIN (ANALYZE, BUFFERS, TIMING)
SELECT COUNT(*) FROM (
    SELECT value FROM sort_test ORDER BY value
) s;

\timing off

-- Show table size
\echo ''
SELECT pg_size_pretty(pg_total_relation_size('sort_test')) as table_size;

\echo ''
\echo 'Note: GPU acceleration engages automatically for datasets >= 262,144 rows'
\echo 'Compare this result with a standard PostgreSQL build to see the performance difference' 