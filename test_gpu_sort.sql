-- Test GPU-accelerated sorting in PostgreSQL
-- This test creates a table with enough rows to trigger GPU sorting

-- Force serial execution for GPU testing
SET max_parallel_workers_per_gather = 0;
SET max_parallel_workers = 0;

-- Increase work_mem to force in-memory sorting
SET work_mem = '256MB';

-- Suppress warnings during setup
SET client_min_messages = WARNING;

-- Drop test table if it exists
DROP TABLE IF EXISTS gpu_sort_test;

-- Create a test table with integer data
CREATE TABLE gpu_sort_test (
    value BIGINT NOT NULL
);

-- Insert exactly 300k rows (just above GPU_SORT_THRESHOLD of 256k)
-- Using a CTE to make it faster
INSERT INTO gpu_sort_test (value)
SELECT (random() * 9223372036854775807)::bigint - 4611686018427387904
FROM generate_series(1, 300000);

-- Re-enable notices for the actual test
SET client_min_messages = NOTICE;

-- Enable timing
\timing on

-- Test: Simple sort that should trigger GPU acceleration
-- Using COUNT(*) to avoid returning all rows
SELECT COUNT(*) FROM (
    SELECT value FROM gpu_sort_test ORDER BY value
) sorted;

-- Show statistics
SELECT pg_size_pretty(pg_relation_size('gpu_sort_test')) as table_size;

-- Clean up
DROP TABLE gpu_sort_test; 