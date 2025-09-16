--
-- Test CHECK DIAGNOSTICS (ROW_COUNT = n) functionality
--

-- Setup test table
CREATE TEMP TABLE test_rowcount(a int);
INSERT INTO test_rowcount VALUES (1), (2), (3);

-- Test 1: Pass - exactly one row
SELECT a FROM test_rowcount WHERE a = 1
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 2: Pass - exactly zero rows
SELECT a FROM test_rowcount WHERE a = 999
CHECK DIAGNOSTICS (ROW_COUNT = 0);

-- Test 3: Pass - exactly three rows
SELECT a FROM test_rowcount
CHECK DIAGNOSTICS (ROW_COUNT = 3);

-- Test 4: Pass with LIMIT (post-LIMIT value)
SELECT a FROM test_rowcount ORDER BY a LIMIT 2
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 5: Fail - expected 1, got 0
SELECT a FROM test_rowcount WHERE a = 999
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 6: Fail - expected 1, got 2
SELECT a FROM test_rowcount WHERE a IN (1,2)
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 7: Syntax error - negative literal
SELECT a FROM test_rowcount
CHECK DIAGNOSTICS (ROW_COUNT = -1);

-- Test 8: Syntax error - non-literal
PREPARE p(int) AS
  SELECT a FROM test_rowcount WHERE a = $1
  CHECK DIAGNOSTICS (ROW_COUNT = $1);

-- Test 9: Feature not supported - VALUES clause
VALUES (1), (2)
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 10: Feature not supported - set operations
SELECT a FROM test_rowcount WHERE a = 1
UNION
SELECT a FROM test_rowcount WHERE a = 2
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 11: Syntax error - duplicate ROW_COUNT
SELECT a FROM test_rowcount WHERE a = 1
CHECK DIAGNOSTICS (ROW_COUNT = 1, ROW_COUNT = 1);

-- Test 12: Syntax error - empty clause
SELECT a FROM test_rowcount WHERE a = 1
CHECK DIAGNOSTICS ();

-- Test 13: Pass - within SQL-standard function body
CREATE FUNCTION test_func(x int) RETURNS int
BEGIN ATOMIC
  SELECT a FROM test_rowcount WHERE a = x
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
END;

-- Test the function - should pass
SELECT test_func(1);

-- Test the function - should fail
SELECT test_func(999);
