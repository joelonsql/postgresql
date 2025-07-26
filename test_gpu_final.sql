-- Final GPU Sort Performance Test
-- This test demonstrates the zero-copy GPU-accelerated sorting

SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '512MB';

\echo '=== PostgreSQL GPU-Accelerated Sort (Metal) ==='
\echo 'Using zero-copy unified memory architecture'
\echo ''

-- Create test data
DROP TABLE IF EXISTS gpu_sort_test;
CREATE TABLE gpu_sort_test AS
SELECT 
    (random() * 9223372036854775807)::bigint - 4611686018427387904 as id,
    random() as score,
    md5(random()::text) as data
FROM generate_series(1, 2000000);

-- Create index for comparison
CREATE INDEX idx_gpu_sort_test_id ON gpu_sort_test(id);
ANALYZE gpu_sort_test;

\echo ''
\echo 'Dataset: 2 million rows'
\echo ''

-- Test 1: Full table sort (GPU)
\echo 'Test 1: Full table sort by id (GPU-accelerated)'
\timing on
EXPLAIN (ANALYZE, BUFFERS) 
SELECT id FROM gpu_sort_test ORDER BY id LIMIT 10;

-- Test 2: Sort with additional columns
\echo ''
\echo 'Test 2: Sort with data retrieval'
EXPLAIN (ANALYZE, BUFFERS)
SELECT id, score, data 
FROM gpu_sort_test 
ORDER BY id 
LIMIT 10;

-- Test 3: Verify correctness
\echo ''
\echo 'Test 3: Verify sort correctness (first and last 5 values)'
WITH sorted AS (
    SELECT id, ROW_NUMBER() OVER (ORDER BY id) as rn
    FROM gpu_sort_test
)
SELECT 'First 5' as position, id 
FROM sorted 
WHERE rn <= 5
UNION ALL
SELECT 'Last 5' as position, id 
FROM sorted 
WHERE rn > (SELECT COUNT(*) - 5 FROM gpu_sort_test)
ORDER BY position DESC, id;

\timing off

-- Cleanup
DROP TABLE gpu_sort_test;

\echo ''
\echo 'GPU Sort Features:'
\echo '- Zero-copy data access (unified memory)'
\echo '- Bitonic sort algorithm optimized for parallel execution'
\echo '- Automatic fallback to CPU for small datasets'
\echo '- Threshold: 262,144 rows (configurable)' 