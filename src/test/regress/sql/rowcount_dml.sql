--
-- Test CHECK DIAGNOSTICS (ROW_COUNT = n) functionality for DML statements
--

-- Setup test table
CREATE TEMP TABLE dml_t(id int primary key, val int unique, note text);

-- INSERT initial rows for UPDATE/DELETE tests
INSERT INTO dml_t VALUES (1, 10, 'first'), (2, 20, 'second'), (3, 30, 'third');

-- INSERT Tests

-- Test 1: Pass - insert two VALUES rows → ROW_COUNT = 2
INSERT INTO dml_t VALUES (4, 40, 'fourth'), (5, 50, 'fifth')
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 2: Fail - expected 1, got 2
INSERT INTO dml_t VALUES (6, 60, 'sixth'), (7, 70, 'seventh')
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 3: Pass - DEFAULT VALUES → ROW_COUNT = 1
CREATE TEMP TABLE dml_defaults(id serial primary key, name text default 'default');
INSERT INTO dml_defaults DEFAULT VALUES
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 4: Pass - INSERT ... SELECT producing 2 rows → ROW_COUNT = 2
INSERT INTO dml_t SELECT id + 100, val + 100, note || '_copy' FROM dml_t WHERE id IN (1, 2)
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 5: ON CONFLICT DO NOTHING - conflict occurs → ROW_COUNT = 0
INSERT INTO dml_t VALUES (1, 999, 'conflict') ON CONFLICT (id) DO NOTHING
CHECK DIAGNOSTICS (ROW_COUNT = 0);

-- Test 6: ON CONFLICT DO NOTHING - no conflict → ROW_COUNT = 1
INSERT INTO dml_t VALUES (8, 80, 'eighth') ON CONFLICT (id) DO NOTHING
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 7: ON CONFLICT DO UPDATE - conflict updates exactly one → ROW_COUNT = 1
INSERT INTO dml_t VALUES (1, 999, 'updated') ON CONFLICT (id) DO UPDATE SET note = EXCLUDED.note
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 8: Syntax errors for INSERT
-- Negative literal
INSERT INTO dml_t VALUES (9, 90, 'ninth')
CHECK DIAGNOSTICS (ROW_COUNT = -1);

-- Non-literal (parameter)
PREPARE insert_p(int) AS
  INSERT INTO dml_t VALUES ($1, $1 * 10, 'param')
  CHECK DIAGNOSTICS (ROW_COUNT = $1);

-- Duplicate ROW_COUNT item
INSERT INTO dml_t VALUES (10, 100, 'tenth')
CHECK DIAGNOSTICS (ROW_COUNT = 1, ROW_COUNT = 1);

-- UPDATE Tests

-- Test 9: Pass - update exactly one row
UPDATE dml_t SET note = 'updated_first' WHERE id = 1
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 10: Pass - update zero rows (no match) with ROW_COUNT = 0
UPDATE dml_t SET note = 'no_match' WHERE id = 999
CHECK DIAGNOSTICS (ROW_COUNT = 0);

-- Test 11: Fail - updated many rows vs. ROW_COUNT = 1
UPDATE dml_t SET note = 'updated_all' WHERE id < 100
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 12: With RETURNING present (still asserting affected rows)
UPDATE dml_t SET note = 'updated_with_returning' WHERE id = 2
RETURNING id, note
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- DELETE Tests

-- Test 13: Pass - delete exactly one row
DELETE FROM dml_t WHERE id = 3
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 14: Pass - delete zero rows with ROW_COUNT = 0
DELETE FROM dml_t WHERE id = 999
CHECK DIAGNOSTICS (ROW_COUNT = 0);

-- Test 15: Fail - delete two rows vs. ROW_COUNT = 1
DELETE FROM dml_t WHERE id IN (4, 5)
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 16: With RETURNING present
DELETE FROM dml_t WHERE id = 6
RETURNING id, note
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- WITH (CTE) Tests

-- Test 17: WITH CTE + UPDATE
WITH updated_ids AS (SELECT id FROM dml_t WHERE val > 100)
UPDATE dml_t SET note = 'cte_updated' WHERE id IN (SELECT id FROM updated_ids)
RETURNING id, note
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 18: WITH CTE + DELETE
WITH to_delete AS (SELECT id FROM dml_t WHERE val > 100)
DELETE FROM dml_t WHERE id IN (SELECT id FROM to_delete)
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- Test 19: WITH CTE + INSERT ... SELECT
WITH source_data AS (VALUES (11, 110, 'cte_insert'), (12, 120, 'cte_insert2'))
INSERT INTO dml_t SELECT * FROM source_data
CHECK DIAGNOSTICS (ROW_COUNT = 2);

-- EXPLAIN Tests

-- Test 20: EXPLAIN (ANALYZE FALSE) - no error, no execution
EXPLAIN (ANALYZE FALSE)
INSERT INTO dml_t VALUES (13, 130, 'explain_test')
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- Test 21: EXPLAIN (ANALYZE TRUE) - executes and should raise error for mismatch
EXPLAIN (ANALYZE TRUE)
INSERT INTO dml_t VALUES (14, 140, 'explain_analyzed'), (15, 150, 'explain_analyzed2')
CHECK DIAGNOSTICS (ROW_COUNT = 1);

-- SQL-standard function body Tests

-- Test 22: Function with INSERT that passes
CREATE FUNCTION test_dml_insert_func(x int) RETURNS int
BEGIN ATOMIC
  INSERT INTO dml_t VALUES (x, x * 10, 'func_insert')
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT x;
END;

-- Test the function - should pass
SELECT test_dml_insert_func(20);

-- Test 23: Function with INSERT that fails
CREATE FUNCTION test_dml_insert_fail_func() RETURNS int
BEGIN ATOMIC
  INSERT INTO dml_t VALUES (21, 210, 'fail1'), (22, 220, 'fail2')
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT 1;
END;

-- Test the function - should fail
SELECT test_dml_insert_fail_func();

-- Test 24: Function with UPDATE that passes
CREATE FUNCTION test_dml_update_func(target_id int) RETURNS int
BEGIN ATOMIC
  UPDATE dml_t SET note = 'func_updated' WHERE id = target_id
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT target_id;
END;

-- Test the function - should pass
SELECT test_dml_update_func(20);

-- Test 25: Function with UPDATE that fails
CREATE FUNCTION test_dml_update_fail_func() RETURNS int
BEGIN ATOMIC
  UPDATE dml_t SET note = 'func_fail_update' WHERE id < 100
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT 1;
END;

-- Test the function - should fail
SELECT test_dml_update_fail_func();

-- Test 26: Function with DELETE that passes
CREATE FUNCTION test_dml_delete_func(target_id int) RETURNS int
BEGIN ATOMIC
  DELETE FROM dml_t WHERE id = target_id
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT target_id;
END;

-- Test the function - should pass
SELECT test_dml_delete_func(20);

-- Test 27: Function with DELETE that fails
CREATE FUNCTION test_dml_delete_fail_func() RETURNS int
BEGIN ATOMIC
  DELETE FROM dml_t WHERE id IN (11, 12)
  CHECK DIAGNOSTICS (ROW_COUNT = 1);
  SELECT 1;
END;

-- Test the function - should fail
SELECT test_dml_delete_fail_func();