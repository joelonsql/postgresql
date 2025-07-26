-- GPU vs CPU Sort Performance Comparison
SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '256MB';

-- Test with different data sizes
\echo '=== Performance Comparison: GPU vs CPU Sort ==='
\echo ''

-- Test 1: Just below GPU threshold (250k rows)
\echo 'Test 1: 250,000 rows (below GPU threshold)'
DROP TABLE IF EXISTS sort_test;
CREATE TABLE sort_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 250000);

\timing on
SET client_min_messages = ERROR; -- Suppress notices for cleaner output
SELECT COUNT(*) FROM (SELECT value FROM sort_test ORDER BY value) s;
SET client_min_messages = NOTICE;

-- Test 2: Just above GPU threshold (300k rows)
\echo ''
\echo 'Test 2: 300,000 rows (above GPU threshold - GPU enabled)'
DROP TABLE IF EXISTS sort_test;
CREATE TABLE sort_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 300000);

SET client_min_messages = WARNING; -- Show only key messages
SELECT COUNT(*) FROM (SELECT value FROM sort_test ORDER BY value) s;

-- Test 3: Larger dataset (500k rows)
\echo ''
\echo 'Test 3: 500,000 rows (GPU enabled)'
DROP TABLE IF EXISTS sort_test;
CREATE TABLE sort_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 500000);

SELECT COUNT(*) FROM (SELECT value FROM sort_test ORDER BY value) s;

-- Test 4: 1 million rows
\echo ''
\echo 'Test 4: 1,000,000 rows (GPU enabled)'
DROP TABLE IF EXISTS sort_test;
CREATE TABLE sort_test AS 
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
FROM generate_series(1, 1000000);

SELECT COUNT(*) FROM (SELECT value FROM sort_test ORDER BY value) s;

-- Final cleanup
DROP TABLE sort_test;
\timing off

\echo ''
\echo 'Note: GPU sort is used for datasets >= 262,144 rows' 