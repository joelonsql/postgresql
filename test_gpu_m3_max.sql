-- GPU Sort Performance Test for M3 Max (128GB RAM, 32GB shared_buffers)
-- Testing zero-copy GPU-accelerated sorting at scale

SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '4GB';  -- Larger work_mem for your system

\echo '=== PostgreSQL GPU Sort on Apple M3 Max ==='
\echo 'System: 128GB RAM, 32GB shared_buffers'
\echo 'GPU: M3 Max with 400GB/s unified memory bandwidth'
\echo ''

-- Test different dataset sizes to find the sweet spot
CREATE OR REPLACE FUNCTION test_gpu_sort(num_rows int) 
RETURNS TABLE(rows int, sort_time_ms numeric) AS $$
DECLARE
    start_time timestamp;
    end_time timestamp;
BEGIN
    -- Create test data
    DROP TABLE IF EXISTS gpu_test;
    CREATE TEMP TABLE gpu_test AS
    SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as value
    FROM generate_series(1, num_rows);
    
    -- Measure sort time
    start_time := clock_timestamp();
    PERFORM COUNT(*) FROM (SELECT value FROM gpu_test ORDER BY value) s;
    end_time := clock_timestamp();
    
    RETURN QUERY SELECT num_rows, 
        EXTRACT(EPOCH FROM (end_time - start_time)) * 1000;
END;
$$ LANGUAGE plpgsql;

\echo 'Testing various dataset sizes...'
\echo ''

-- Run tests from 100K to 50M rows
WITH test_sizes AS (
    SELECT unnest(ARRAY[
        100000,     -- Below GPU threshold (CPU)
        262144,     -- Exactly at GPU threshold
        500000,     -- 0.5M
        1000000,    -- 1M
        2000000,    -- 2M  
        5000000,    -- 5M
        10000000,   -- 10M
        20000000,   -- 20M
        50000000    -- 50M
    ]) as size
),
results AS (
    SELECT size, (test_gpu_sort(size)).*
    FROM test_sizes
)
SELECT 
    size as "Rows",
    CASE 
        WHEN size < 262144 THEN 'CPU'
        ELSE 'GPU'
    END as "Method",
    round(sort_time_ms::numeric, 2) as "Time (ms)",
    round((size / (sort_time_ms / 1000.0))::numeric, 0) as "Rows/sec"
FROM results
ORDER BY size;

\echo ''
\echo 'Large dataset test (100M rows - ~1.5GB of int64 data):'
\timing on

DROP TABLE IF EXISTS gpu_large_test;
CREATE TABLE gpu_large_test AS
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904 as id,
       random() * 1000000 as score
FROM generate_series(1, 100000000);

\echo ''
\echo 'Sorting 100 million rows...'
EXPLAIN (ANALYZE, BUFFERS, SETTINGS)
SELECT COUNT(*) FROM (
    SELECT id FROM gpu_large_test ORDER BY id
) s;

-- Test with index for comparison
\echo ''
\echo 'Creating index for comparison...'
CREATE INDEX idx_gpu_large_test_id ON gpu_large_test(id);
ANALYZE gpu_large_test;

\echo ''
\echo 'Index scan (for comparison):'
EXPLAIN (ANALYZE, BUFFERS)
SELECT id FROM gpu_large_test ORDER BY id LIMIT 1000;

\timing off

-- Memory usage info
\echo ''
\echo 'Memory usage:'
SELECT 
    pg_size_pretty(pg_database_size(current_database())) as database_size,
    pg_size_pretty(pg_total_relation_size('gpu_large_test')) as table_size;

-- Cleanup
DROP FUNCTION test_gpu_sort(int);
DROP TABLE gpu_large_test;

\echo ''
\echo 'M3 Max GPU Advantages:'
\echo '- 400GB/s unified memory bandwidth (no PCIe bottleneck)'
\echo '- Zero-copy access to PostgreSQL shared_buffers'
\echo '- 40-core GPU with massive parallelism'
\echo '- Hardware-optimized for data-parallel workloads' 