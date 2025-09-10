--
-- Tests for Common Subexpression Elimination (CSE)
--

-- Create test tables
CREATE TABLE cse_test (
    id INTEGER,
    val INTEGER,
    txt TEXT
);

INSERT INTO cse_test VALUES (1, 10, 'abc'), (2, 20, 'def'), (3, 30, 'ghi');

-- Test basic CSE functionality
-- First with CSE disabled (default)
SET enable_cse = OFF;

-- Simple repeated expression
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val + 100, (val + 100) * 2, (val + 100) / 3 FROM cse_test;

-- Complex repeated expression
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 
    COALESCE(val * 2 + id, 0),
    COALESCE(val * 2 + id, 0) + 100,
    CASE WHEN COALESCE(val * 2 + id, 0) > 50 THEN 'high' ELSE 'low' END
FROM cse_test;

-- Now enable CSE
SET enable_cse = ON;

-- Simple repeated expression with CSE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val + 100, (val + 100) * 2, (val + 100) / 3 FROM cse_test;

-- Complex repeated expression with CSE
EXPLAIN (VERBOSE, COSTS OFF)
SELECT 
    COALESCE(val * 2 + id, 0),
    COALESCE(val * 2 + id, 0) + 100,
    CASE WHEN COALESCE(val * 2 + id, 0) > 50 THEN 'high' ELSE 'low' END
FROM cse_test;

-- Test the Easter calculation query
PREPARE calc_easter_day_for_year AS
SELECT make_date($1::integer, easter_month, easter_day)
FROM (VALUES ($1::integer % 19, $1::integer / 100)) AS step1(g,c)
CROSS JOIN LATERAL (VALUES ((c - c/4 - (8*c + 13)/25 + 19*g + 15) % 30)) AS step2(h)
CROSS JOIN LATERAL (VALUES (h - (h/28)*(1 - (h/28)*(29/(h + 1))*((21 - g)/11)))) AS step3(i)
CROSS JOIN LATERAL (VALUES (($1::integer + $1::integer/4 + i + 2 - c + c/4) % 7)) AS step4(j)
CROSS JOIN LATERAL (VALUES (i - j)) AS step5(p)
CROSS JOIN LATERAL (VALUES (3 + (p + 26)/30, 1 + (p + 27 + (p + 6)/40) % 31)) AS step6(easter_month, easter_day);

SET plan_cache_mode = 'force_generic_plan';

-- Test with CSE disabled
SET enable_cse = OFF;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE calc_easter_day_for_year(2021);

-- Test with CSE enabled
SET enable_cse = ON;
EXPLAIN (VERBOSE, COSTS OFF) EXECUTE calc_easter_day_for_year(2021);

-- Test CSE threshold
SET cse_min_usage_threshold = 3;

-- This should not trigger CSE (only 2 occurrences)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val * 10, val * 10 + 5 FROM cse_test;

-- This should trigger CSE (3 occurrences)
EXPLAIN (VERBOSE, COSTS OFF)
SELECT val * 10, val * 10 + 5, val * 10 - 3 FROM cse_test;

-- Test that CSE respects volatility
CREATE FUNCTION volatile_func(int) RETURNS int AS 
$$ SELECT $1 + random()::int $$ LANGUAGE SQL VOLATILE;

CREATE FUNCTION immutable_func(int) RETURNS int AS
$$ SELECT $1 * 2 $$ LANGUAGE SQL IMMUTABLE;

-- Volatile functions should not be CSE'd
EXPLAIN (VERBOSE, COSTS OFF)
SELECT volatile_func(val), volatile_func(val) FROM cse_test;

-- Immutable functions should be CSE'd
EXPLAIN (VERBOSE, COSTS OFF)
SELECT immutable_func(val), immutable_func(val), immutable_func(val) + 10 FROM cse_test;

-- Clean up
DROP TABLE cse_test;
DROP FUNCTION volatile_func(int);
DROP FUNCTION immutable_func(int);

-- Reset settings
RESET enable_cse;
RESET cse_min_usage_threshold;
RESET plan_cache_mode;