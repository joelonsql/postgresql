-- Test Zero-Copy GPU Sort Performance
SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '256MB';

\echo '=== Zero-Copy GPU Sort Test ==='
\echo ''

-- Create larger test dataset
DROP TABLE IF EXISTS zero_copy_test;
CREATE TABLE zero_copy_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 1000000);

\echo 'Testing with 1 million rows...'
\timing on

-- Run the sort multiple times to get consistent timing
\echo 'Run 1:'
SELECT COUNT(*) FROM (SELECT value FROM zero_copy_test ORDER BY value) s;

\echo 'Run 2:'
SELECT COUNT(*) FROM (SELECT value FROM zero_copy_test ORDER BY value) s;

\echo 'Run 3:'
SELECT COUNT(*) FROM (SELECT value FROM zero_copy_test ORDER BY value) s;

-- Test with 2 million rows
\echo ''
\echo 'Testing with 2 million rows...'
DROP TABLE IF EXISTS zero_copy_test;
CREATE TABLE zero_copy_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 2000000);

SELECT COUNT(*) FROM (SELECT value FROM zero_copy_test ORDER BY value) s;

-- Cleanup
DROP TABLE zero_copy_test;
\timing off 