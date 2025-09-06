--
-- JSON Schema Generation Tests
--

-- Test setup: Create sample tables and functions
CREATE TABLE orders (
    id          bigserial PRIMARY KEY,
    customer_id bigint      NOT NULL,
    status      text        NOT NULL,
    created_at  timestamptz NOT NULL DEFAULT now()
);

CREATE TABLE order_items (
    id        bigserial PRIMARY KEY,
    order_id  bigint      NOT NULL REFERENCES orders(id),
    sku       text        NOT NULL,
    qty       int         NOT NULL CHECK (qty > 0),
    price     numeric(12,2) NOT NULL
);

-- SQL-body function for deep introspection testing
CREATE FUNCTION get_order(_order_id bigint)
RETURNS TABLE (
    id          bigint,
    customer_id bigint,
    status      text,
    created_at  timestamptz,
    items       jsonb
)
RETURN (
    SELECT
        o.id,
        o.customer_id,
        o.status,
        o.created_at,
        COALESCE(
            jsonb_agg(
                jsonb_build_object(
                    'id',    i.id,
                    'sku',   i.sku,
                    'qty',   i.qty,
                    'price', i.price
                )
            ) FILTER (WHERE i.id IS NOT NULL),
            '[]'::jsonb
        ) AS items
    FROM orders o
    LEFT JOIN order_items i ON i.order_id = o.id
    WHERE o.id = _order_id
    GROUP BY o.id, o.customer_id, o.status, o.created_at
);

-- Simple SQL-body function returning scalar
CREATE FUNCTION add_numbers(a int, b int)
RETURNS int
RETURN a + b;

-- SQL-body function with JSON construction
CREATE FUNCTION get_status_json()
RETURNS jsonb
RETURN jsonb_build_object(
    'timestamp', now(),
    'server_version', version(),
    'active', true,
    'connections', 42
);

-- SQL-body function with array construction
CREATE FUNCTION get_sample_array()
RETURNS jsonb
RETURN jsonb_build_array(1, 'two', true, null, jsonb_build_object('nested', 'value'));

-- Non-SQL-body (PL/pgSQL) function for shallow introspection
CREATE FUNCTION plpgsql_json_func(input_text text)
RETURNS jsonb
LANGUAGE plpgsql
AS $$
BEGIN
    RETURN jsonb_build_object('input', input_text, 'processed', true);
END;
$$;

-- Function returning composite type
CREATE TYPE order_summary AS (
    total_orders bigint,
    total_amount numeric,
    avg_amount numeric
);

CREATE FUNCTION get_order_summary()
RETURNS order_summary
LANGUAGE sql
AS $$
    SELECT 
        COUNT(*)::bigint,
        SUM(100.00)::numeric,
        AVG(100.00)::numeric
$$;

-- Test json_schema_generate with different function types

-- Test 1: Deep introspection of SQL-body function with complex JSON
SELECT jsonb_pretty(json_schema_generate('get_order(bigint)'::regprocedure));

-- Test 2: Simple scalar SQL-body function
SELECT jsonb_pretty(json_schema_generate('add_numbers(int,int)'::regprocedure));

-- Test 3: SQL-body function with jsonb_build_object
SELECT jsonb_pretty(json_schema_generate('get_status_json()'::regprocedure));

-- Test 4: SQL-body function with jsonb_build_array
SELECT jsonb_pretty(json_schema_generate('get_sample_array()'::regprocedure));

-- Test 5: Shallow introspection of non-SQL-body function
SELECT jsonb_pretty(json_schema_generate('plpgsql_json_func(text)'::regprocedure));

-- Test 6: Function returning composite type
SELECT jsonb_pretty(json_schema_generate('get_order_summary()'::regprocedure));

-- Test 7: Using OID directly
SELECT json_schema_generate('get_order(bigint)'::regprocedure::oid) IS NOT NULL AS has_schema;

-- Test 8: Using regproc (less specific)
SELECT json_schema_generate('add_numbers'::regproc) IS NOT NULL AS has_schema;

-- Test metadata fields
SELECT 
    result->>'$schema' AS schema_version,
    result->>'x-pg-introspection' AS introspection_type,
    result->>'x-pg-depth' AS depth
FROM (
    SELECT json_schema_generate('get_order(bigint)'::regprocedure) AS result
) t;

-- Test with SETOF returns
CREATE FUNCTION get_all_orders()
RETURNS SETOF orders
LANGUAGE sql
AS $$
    SELECT * FROM orders;
$$;

SELECT 
    result->>'type' AS type,
    result->'items'->>'type' AS item_type,
    result->>'x-pg-returns' AS returns_type
FROM (
    SELECT json_schema_generate('get_all_orders()'::regprocedure) AS result
) t;

-- Cleanup
DROP FUNCTION get_all_orders();
DROP FUNCTION get_order_summary();
DROP TYPE order_summary;
DROP FUNCTION plpgsql_json_func(text);
DROP FUNCTION get_sample_array();
DROP FUNCTION get_status_json();
DROP FUNCTION add_numbers(int, int);
DROP FUNCTION get_order(bigint);
DROP TABLE order_items;
DROP TABLE orders;