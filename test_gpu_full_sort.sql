-- Force GPU sort by sorting without LIMIT
SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;
SET work_mem = '256MB';
SET client_min_messages = NOTICE;

-- Create test table
DROP TABLE IF EXISTS gpu_sort_test;
CREATE TABLE gpu_sort_test (value BIGINT NOT NULL);

-- Insert 300k rows to trigger GPU sort
INSERT INTO gpu_sort_test (value)
SELECT (random() * 1000000)::bigint 
FROM generate_series(1, 300000);

-- This should trigger GPU sort
\echo 'Forcing full GPU sort (counting sorted results):'
\timing on
SELECT COUNT(*) FROM (
    SELECT value FROM gpu_sort_test ORDER BY value
) sorted;

-- For comparison, let's also time without ORDER BY
\echo 'Count without sorting (for comparison):'
SELECT COUNT(*) FROM gpu_sort_test;

-- Cleanup
DROP TABLE gpu_sort_test; 