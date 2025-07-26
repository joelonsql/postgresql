-- Test GPU sort correctness
SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '256MB';
SET client_min_messages = NOTICE;

-- Create a smaller test table for verification
DROP TABLE IF EXISTS gpu_sort_verify;
CREATE TABLE gpu_sort_verify (value BIGINT NOT NULL);

-- Insert 300k rows to trigger GPU sort
INSERT INTO gpu_sort_verify (value)
SELECT (random() * 1000000)::bigint 
FROM generate_series(1, 300000);

-- Test 1: Get first 10 sorted values
\echo 'First 10 values (should be ascending):'
SELECT value FROM gpu_sort_verify ORDER BY value LIMIT 10;

-- Test 2: Get last 10 sorted values  
\echo 'Last 10 values (should be near 1000000):'
SELECT value FROM gpu_sort_verify ORDER BY value DESC LIMIT 10;

-- Test 3: Verify count
\echo 'Total count (should be 300000):'
SELECT COUNT(*) FROM gpu_sort_verify;

-- Test 4: Compare GPU sort with a known correct sort on smaller dataset
DROP TABLE IF EXISTS gpu_sort_small;
CREATE TABLE gpu_sort_small AS 
SELECT generate_series AS value FROM generate_series(10, 1, -1);

\echo 'Small test - unsorted:'
SELECT * FROM gpu_sort_small;

\echo 'Small test - sorted (should be 1-10):'
SELECT * FROM gpu_sort_small ORDER BY value;

-- Cleanup
DROP TABLE gpu_sort_verify;
DROP TABLE gpu_sort_small; 