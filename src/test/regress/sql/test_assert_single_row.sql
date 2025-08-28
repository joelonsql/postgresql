--
-- Test assert_single_row
--

CREATE SCHEMA test_assert_single_row;

SET search_path = test_assert_single_row, public;

CREATE TABLE foo (a int, b int);

--
-- Positive tests
--

-- Test INSERT ok

CREATE FUNCTION test_insert(_a int, _b int)
RETURNS BOOLEAN
SET assert_single_row = true
BEGIN ATOMIC
    INSERT INTO foo (a, b) VALUES (_a, _b)
    RETURNING TRUE;
END;

SELECT test_insert(1, 10);
SELECT test_insert(2, 20);
SELECT test_insert(3, 20);

-- Test SELECT ok

CREATE FUNCTION test_select(_b int)
RETURNS INT
SET assert_single_row = true
BEGIN ATOMIC
    SELECT a FROM foo WHERE b = _b;
END;

SELECT test_select(10);

-- Test UPDATE ok

CREATE FUNCTION test_update(_a int, _b int)
RETURNS BOOLEAN
SET assert_single_row = true
BEGIN ATOMIC
    UPDATE foo SET a = _a WHERE b = _b
    RETURNING TRUE;
END;

SELECT test_update(4, 10);

-- Test DELETE ok

CREATE FUNCTION test_delete(_a int)
RETURNS BOOLEAN
SET assert_single_row = true
BEGIN ATOMIC
    DELETE FROM foo WHERE a = _a
    RETURNING TRUE;
END;

SELECT test_delete(4);

--
-- Negative tests
--

-- Test SELECT fail

SELECT test_select(100);

-- Test INSERT fail

CREATE OR REPLACE FUNCTION prevent_insert()
RETURNS trigger
LANGUAGE plpgsql AS $$
BEGIN
    RETURN NULL;
END;
$$;

CREATE TRIGGER foo_no_inserts
BEFORE INSERT ON foo
FOR EACH ROW
EXECUTE FUNCTION prevent_insert();

SELECT test_insert(5, 50);

DROP TRIGGER foo_no_inserts ON foo;

-- Test UPDATE fail

SELECT test_update(6, 60);

-- Test DELETE fail

SELECT test_delete(4);

RESET search_path;
