--
-- Test Foreign Key Joins.
--

CREATE TABLE t1
(
    c1 int not null,
    c2 int not null,
    CONSTRAINT t1_pkey PRIMARY KEY (c1)
);

CREATE TABLE t2
(
    c3 int not null,
    c4 int not null,
    CONSTRAINT t2_pkey PRIMARY KEY (c3),
    CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1)
);

INSERT INTO t1 (c1, c2) VALUES (1, 10);
INSERT INTO t1 (c1, c2) VALUES (2, 20);
INSERT INTO t1 (c1, c2) VALUES (3, 30);
INSERT INTO t2 (c3, c4) VALUES (1, 10);
INSERT INTO t2 (c3, c4) VALUES (3, 30);

--
-- Test renaming tables and columns.
--
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 KEY (c3) -> t1 (c1);
\d+ v1
SELECT * FROM v1; -- ok

ALTER TABLE t1 RENAME COLUMN c1 TO c1_renamed;
ALTER TABLE t2 RENAME COLUMN c3 TO c3_renamed;
ALTER TABLE t1 RENAME TO t1_renamed;
ALTER TABLE t2 RENAME TO t2_renamed;
\d+ v1

SELECT * FROM v1; -- ok

-- Undo the effect of the renames
ALTER TABLE t2_renamed RENAME TO t2;
ALTER TABLE t1_renamed RENAME TO t1;
ALTER TABLE t2 RENAME COLUMN c3_renamed TO c3;
ALTER TABLE t1 RENAME COLUMN c1_renamed TO c1;
\d+ v1

-- Test so we didn't break the parser
SELECT 1<-2; -- ok, false

SELECT * FROM v1; -- ok

SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3) ->/*comment*/ t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3) /*comment*/-> t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3) /*comment*/->/*comment*/ t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3) - > t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) -/*comment*/> t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) -> t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) -> t1 (c1,c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c1) <- t1 (c3); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) <- t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) <- t1 (c1,c2); -- error
SELECT * FROM t1 AS a JOIN t2 AS b KEY (c3) -> a (c2); -- error

SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) <-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) < - t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) </*comment*/- t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) <- t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) <- t2 (c3,c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) -> t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) -> t2 (c3,c4); -- error
SELECT * FROM t2 AS a JOIN t1 AS b KEY (c1) <- a (c4); -- error

ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey; -- error

DROP VIEW v1;
ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey;

/* Recreate contraint and view to test DROP CASCADE */
ALTER TABLE t2 ADD CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1);
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 KEY (c3) -> t1 (c1);
ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey CASCADE; -- ok

SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c3); -- error

ALTER TABLE t1 ADD UNIQUE (c1,c2);
ALTER TABLE t2 ADD CONSTRAINT t2_c3_c4_fkey FOREIGN KEY (c3,c4) REFERENCES t1 (c1,c2);

CREATE VIEW v2 AS
SELECT * FROM t1 JOIN t2 KEY (c3,c4) -> t1 (c1,c2); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3,c4) -> t1 (c1,c2); -- ok
SELECT * FROM v2; -- ok
\d+ v2

CREATE VIEW v3 AS
SELECT * FROM t2 JOIN t1 KEY (c1,c2) <- t2 (c3,c4); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1,c2) <- t2 (c3,c4); -- ok
\d+ v3

SELECT * FROM v3; -- ok

SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c3); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) <- t1 (c1,c2); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) -> t2 (c3,c4); -- error

--
-- Test nulls and multiple tables
--

CREATE TABLE t3
(
    c5 int,
    c6 int,
    CONSTRAINT t3_c5_c6_fkey FOREIGN KEY (c5, c6) REFERENCES t1 (c1, c2)
);
INSERT INTO t3 (c5, c6) VALUES (1, 10); -- ok
INSERT INTO t3 (c5, c6) VALUES (3, 30); -- ok
INSERT INTO t3 (c5, c6) VALUES (3, NULL); -- ok
INSERT INTO t3 (c5, c6) VALUES (NULL, 30); -- ok
INSERT INTO t3 (c5, c6) VALUES (1234, NULL); -- ok
INSERT INTO t3 (c5, c6) VALUES (NULL, 5678); -- ok
INSERT INTO t3 (c5, c6) VALUES (NULL, NULL); -- ok

--
-- Test composite foreign key joins with columns in matching order
--
SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
JOIN t3 KEY (c5,c6) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
LEFT JOIN t3 KEY (c5,c6) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
RIGHT JOIN t3 KEY (c5,c6) -> t1 (c1,c2);

--
-- Test composite foreign key joins with swapped column orders
--
SELECT *
FROM t1
JOIN t2 KEY (c4,c3) -> t1 (c2,c1)
JOIN t3 KEY (c6,c5) -> t1 (c2,c1);

--
-- Test mismatched column orders between referencing and referenced sides
--
SELECT *
FROM t1
JOIN t2 KEY (c4,c3) -> t1 (c2,c1)
JOIN t3 KEY (c6,c5) -> t1 (c1,c2); -- error

--
-- Test defining foreign key constraints with MATCH FULL
--

CREATE TABLE t4
(
    c7 int,
    c8 int,
    CONSTRAINT t4_c7_c8_fkey FOREIGN KEY (c7, c8) REFERENCES t1 (c1, c2) MATCH FULL
);
INSERT INTO t4 (c7, c8) VALUES (1, 10); -- ok
INSERT INTO t4 (c7, c8) VALUES (3, 30); -- ok
INSERT INTO t4 (c7, c8) VALUES (3, NULL); -- error
INSERT INTO t4 (c7, c8) VALUES (NULL, 30); -- error
INSERT INTO t4 (c7, c8) VALUES (1234, NULL); -- error
INSERT INTO t4 (c7, c8) VALUES (NULL, 5678); -- error
INSERT INTO t4 (c7, c8) VALUES (NULL, NULL); -- ok

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
JOIN t4 KEY (c7,c8) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
LEFT JOIN t4 KEY (c7,c8) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) -> t1 (c1,c2)
RIGHT JOIN t4 KEY (c7,c8) -> t1 (c1,c2);

-- Recreate stuff for pg_dump tests
ALTER TABLE t2
    ADD CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1);
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 KEY (c3) -> t1 (c1);

CREATE TABLE t5
(
    c9 int not null,
    c10 int not null,
    c11 int not null,
    c12 int not null,
    CONSTRAINT t5_pkey PRIMARY KEY (c9, c10),
    CONSTRAINT t5_c11_c12_fkey FOREIGN KEY (c11, c12) REFERENCES t1 (c1, c2)
);

INSERT INTO t5 (c9, c10, c11, c12) VALUES (1, 2, 1, 10);
INSERT INTO t5 (c9, c10, c11, c12) VALUES (3, 4, 3, 30);

CREATE TABLE t6
(
    c13 int not null,
    c14 int not null,
    CONSTRAINT t6_c13_c14_fkey FOREIGN KEY (c13, c14) REFERENCES t5 (c9, c10)
);

INSERT INTO t6 (c13, c14) VALUES (1, 2);
INSERT INTO t6 (c13, c14) VALUES (3, 4);
INSERT INTO t6 (c13, c14) VALUES (3, 4);

CREATE TABLE t7
(
    c15 int not null,
    c16 int not null,
    CONSTRAINT t7_c15_c16_fkey FOREIGN KEY (c15, c16) REFERENCES t5 (c9, c10)
);

INSERT INTO t7 (c15, c16) VALUES (1, 2);
INSERT INTO t7 (c15, c16) VALUES (1, 2);
INSERT INTO t7 (c15, c16) VALUES (3, 4);

CREATE TABLE t8
(
    c17 int not null,
    c18 int not null,
    c19 int,
    c20 int,
    CONSTRAINT t8_pkey PRIMARY KEY (c17, c18),
    CONSTRAINT t8_c19_c20_fkey FOREIGN KEY (c19, c20) REFERENCES t1 (c1, c2)
);

INSERT INTO t8 (c17, c18, c19, c20) VALUES (1, 2, 1, 10);
INSERT INTO t8 (c17, c18, c19, c20) VALUES (3, 4, 3, 30);

CREATE TABLE t9
(
    c21 int not null,
    c22 int not null,
    CONSTRAINT t9_c21_c22_fkey FOREIGN KEY (c21, c22) REFERENCES t8 (c17, c18)
);

INSERT INTO t9 (c21, c22) VALUES (1, 2);
INSERT INTO t9 (c21, c22) VALUES (3, 4);
INSERT INTO t9 (c21, c22) VALUES (3, 4);

CREATE TABLE t10
(
    c23 INT NOT NULL,
    c24 INT NOT NULL,
    c25 INT NOT NULL,
    c26 INT NOT NULL,
    CONSTRAINT t10_pkey PRIMARY KEY (c23, c24),
    CONSTRAINT t10_c23_c24_fkey FOREIGN KEY (c23, c24) REFERENCES t1 (c1, c2),
    CONSTRAINT t10_c25_c26_fkey FOREIGN KEY (c25, c26) REFERENCES t10 (c23, c24)
);

INSERT INTO t10 (c23, c24, c25, c26) VALUES (1, 10, 1, 10);

CREATE TABLE t11
(
    c27 INT NOT NULL,
    c28 INT NOT NULL,
    CONSTRAINT t11_pkey PRIMARY KEY (c27, c28),
    CONSTRAINT t11_c27_c28_fkey FOREIGN KEY (c27, c28) REFERENCES t10 (c23, c24)
);

INSERT INTO t11 (c27, c28) VALUES (1, 10);

--
-- Test subqueries
--

SELECT
    a.c1,
    a.c2,
    b.c3,
    b.c4
FROM t1 AS a
JOIN
(
    SELECT * FROM t2
) AS b KEY (c3) -> a (c1);

SELECT
    a.c1,
    a.c2,
    b.c3,
    b.c4
FROM
(
    SELECT * FROM t1
) AS a
JOIN
(
    SELECT * FROM t2
) AS b KEY (c3) -> a (c1);

SELECT
    a.t1_c1,
    a.t1_c2,
    b.t2_c3,
    b.t2_c4
FROM
(
    SELECT c1 AS t1_c1, c2 AS t1_c2 FROM t1
) AS a
JOIN
(
    SELECT c3 AS t2_c3, c4 AS t2_c4 FROM t2
) AS b KEY (t2_c3) -> a (t1_c1);

SELECT
    a.outer_c1,
    a.outer_c2,
    b.outer_c3,
    b.outer_c4
FROM
(
    SELECT mid_c1 AS outer_c1, mid_c2 AS outer_c2 FROM
    (
        SELECT c1 AS mid_c1, c2 AS mid_c2 FROM t1
    ) sub1
) AS a
JOIN
(
    SELECT mid_c3 AS outer_c3, mid_c4 AS outer_c4 FROM
    (
        SELECT c3 AS mid_c3, c4 AS mid_c4 FROM t2
    ) sub2
) AS b KEY (outer_c3) -> a (outer_c1);

SELECT *
FROM t1
JOIN
(
    SELECT
        t10.c23,
        t10.c24,
        t10_2.c25,
        t10_2.c26
    FROM t10
    JOIN t10 AS t10_2 KEY (c23, c24) <- t10 (c25, c26)
) AS q1 KEY (c23, c24) -> t1 (c1, c2);

SELECT *
FROM t1
JOIN LATERAL (
    SELECT c3, c4 FROM t2 WHERE c4 = c1 + 9
) AS q1 KEY (c3) -> t1 (c1);

--
-- Test CTEs
--

WITH
q1 (q1_c1, q1_c2) AS
(
    SELECT c1, c2 FROM t1
),
q2 (q2_c1, q2_c2) AS
(
    SELECT q1_c1, q1_c2 FROM q1
),
q3 (q3_c3, q3_c4) AS
(
    SELECT c3, c4 FROM t2
),
q4 (q4_c3, q4_c4) AS
(
    SELECT q3_c3, q3_c4 FROM q3
)
SELECT
    q2_c1,
    q2_c2,
    q4_c3,
    q4_c4
FROM q2 JOIN q4 KEY (q4_c3, q4_c4) -> q2 (q2_c1, q2_c2);

WITH RECURSIVE q1 AS (SELECT c1 FROM t1 UNION SELECT c1 FROM q1)
SELECT * FROM q1 JOIN t2 KEY (c3) -> q1 (c1);

--
-- Test VIEWs
--

DROP VIEW v1, v2, v3;

CREATE VIEW v1 AS
SELECT c1 AS v1_c1, c2 AS v1_c2 FROM t1;

CREATE VIEW v2 AS
SELECT v1_c1 AS v2_c1, v1_c2 AS v2_c2 FROM v1;

CREATE VIEW v3 AS
SELECT c3 AS v3_c3, c4 AS v3_c4 FROM t2;

CREATE VIEW v4 AS
SELECT v3_c3 AS v4_c3, v3_c4 AS v4_c4 FROM v3;

CREATE VIEW v5 AS
SELECT
    v2_c1,
    v2_c2,
    v4_c3,
    v4_c4
FROM v2 JOIN v4 KEY (v4_c3, v4_c4) -> v2 (v2_c1, v2_c2);

SELECT * FROM v5;

--
-- Test subqueries, CTEs, and views
--
WITH
q2 (q2_c1, q2_c2) AS
(
    SELECT
        q1_c1,
        q1_c2
    FROM
    (
        SELECT c1 AS q1_c1, c2 AS q1_c2 FROM t1
    ) AS q1
)
SELECT
    q2_c1,
    q2_c2,
    v4_c3,
    v4_c4
FROM q2 JOIN v4 KEY (v4_c3, v4_c4) -> q2 (q2_c1, q2_c2);

DROP VIEW v1, v2, v3, v4, v5;

--
-- Test subqueries, CTEs and VIEWs containing joins
--

SELECT
    q1.c11,
    q1.c12,
    t6.c13,
    t6.c14
FROM
(
    SELECT
        t5.c9,
        t5.c10,
        t5.c11,
        t5.c12
    FROM t5
    JOIN t1 KEY (c1, c2) <- t5 (c11, c12)
    JOIN t1 AS t1_2 KEY (c1, c2) <- t5 (c11, c12)
    JOIN t1 AS t1_3 KEY (c1, c2) <- t5 (c11, c12)
) AS q1
JOIN t6 KEY (c13, c14) -> q1 (c9, c10);

WITH
q1 AS
(
    SELECT
        t5.c9,
        t5.c10,
        t5.c11,
        t5.c12
    FROM t5
    JOIN t1 KEY (c1, c2) <- t5 (c11, c12)
    JOIN t1 AS t1_2 KEY (c1, c2) <- t5 (c11, c12)
    JOIN t1 AS t1_3 KEY (c1, c2) <- t5 (c11, c12)
)
SELECT
    q1.c11,
    q1.c12,
    t6.c13,
    t6.c14
FROM q1
JOIN t6 KEY (c13, c14) -> q1 (c9, c10);

CREATE VIEW v1 AS
SELECT
    t5.c9,
    t5.c10,
    t5.c11,
    t5.c12
FROM t5
JOIN t1 KEY (c1, c2) <- t5 (c11, c12)
JOIN t1 AS t1_2 KEY (c1, c2) <- t5 (c11, c12)
JOIN t1 AS t1_3 KEY (c1, c2) <- t5 (c11, c12);

SELECT
    v1.c11,
    v1.c12,
    t6.c13,
    t6.c14
FROM v1
JOIN t6 KEY (c13, c14) -> v1 (c9, c10);

DROP VIEW v1;

--
-- Test disallowed filtering of referenced table
--

CREATE VIEW v1 AS
SELECT * FROM t1 WHERE c1 > 0;

CREATE VIEW v2 AS
SELECT * FROM t2 WHERE c3 > 0;

-- invalid since v1 is filtered and is the referenced table
SELECT * FROM v1 JOIN t2 KEY (c3) -> v1 (c1);

-- OK, filtering allowed since v2 is the referencing table
SELECT * FROM t1 JOIN v2 KEY (c3) -> t1 (c1);

-- also invalid, since v1 is filtered and is the referenced table
SELECT * FROM v1 JOIN v2 KEY (c3) -> v1 (c1);

-- also invalid, filters uisng a having clause
SELECT * FROM
(
    SELECT c1, count(*) FROM t1 GROUP BY c1 HAVING c2 > 100
) AS u
JOIN t2 KEY (c3) -> u (c1);

-- invalid, since u is filtered and is the referenced table
SELECT * FROM (SELECT c1 FROM t1 LIMIT 1) AS u
JOIN t2 KEY (c3) -> u (c1);

-- invalid, since u is filtered and is the referenced table
SELECT * FROM (SELECT c1 FROM t1 OFFSET 1) AS u
JOIN t2 KEY (c3) -> u (c1);

-- invalid, since referenced table has RLS enabled
ALTER TABLE t1 ENABLE ROW LEVEL SECURITY;

CREATE POLICY t1_policy ON t1 USING (false);

SELECT * FROM (SELECT c1 FROM t1) AS u
JOIN t2 KEY (c3) -> u (c1);

SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1);

ALTER TABLE t1 DISABLE ROW LEVEL SECURITY;

WITH q2 AS
(
    SELECT * FROM t5 WHERE t5.c11 > 0
)
SELECT
    q1.c11,
    q1.c12,
    t7.c15,
    t7.c16
FROM
(
    SELECT
        q2.c9,
        q2.c10,
        q2.c11,
        q2.c12
    FROM q2
    JOIN t1 KEY (c1, c2) <- q2 (c11, c12)
) AS q1
JOIN t7 KEY (c15, c16) -> q1 (c9, c10);

--
-- Test allowed joins not affecting uniqueness
--

SELECT
    q1.c11,
    q1.c12,
    t6.c13,
    t6.c14
FROM
(
    SELECT
        t5.c9,
        t5.c10,
        t5.c11,
        t5.c12
    FROM t5
    JOIN t1 KEY (c1, c2) <- t5 (c11, c12)
) AS q1
JOIN t6 KEY (c13, c14) -> q1 (c9, c10);

--
-- Test disallowed non-unique referenced table
--

SELECT
    q1.c11,
    q1.c12,
    t7.c15,
    t7.c16
FROM
(
    SELECT
        t5.c9,
        t5.c10,
        t5.c11,
        t5.c12
    FROM t5
    JOIN t1 KEY (c1, c2) <- t5 (c11, c12)
    JOIN t6 KEY (c13, c14) -> t5 (c9, c10)
) AS q1
JOIN t7 KEY (c15, c16) -> q1 (c9, c10);

SELECT
    q1.c19,
    q1.c20,
    t9.c21,
    t9.c22
FROM
(
    SELECT
        t8.c17,
        t8.c18,
        t8.c19,
        t8.c20
    FROM t8
    JOIN t1 KEY (c1, c2) <- t8 (c19, c20)
) AS q1
JOIN t9 KEY (c21, c22) -> q1 (c17, c18);

--
-- Test revalidation of views
--

CREATE TABLE addresses
(
    id           INTEGER      NOT NULL,
    street       VARCHAR(255) NOT NULL,
    city         VARCHAR(100) NOT NULL,
    state        VARCHAR(100) NOT NULL,
    country_code CHAR(2)      NOT NULL,
    zip_code     VARCHAR(20)  NOT NULL,
    CONSTRAINT addresses_pkey PRIMARY KEY (id)
);

CREATE TABLE customers
(
    id         INTEGER      NOT NULL,
    name       VARCHAR(255) NOT NULL,
    address_id INTEGER      NOT NULL,
    CONSTRAINT customers_pkey            PRIMARY KEY (id),
    CONSTRAINT customers_address_id_fkey FOREIGN KEY (address_id) REFERENCES addresses (id)
);

CREATE TABLE orders
(
    id           BIGINT         NOT NULL,
    order_date   DATE           NOT NULL,
    amount       DECIMAL(10, 2) NOT NULL,
    customer_id  INTEGER        NOT NULL,
    CONSTRAINT orders_pkey             PRIMARY KEY (id),
    CONSTRAINT orders_customer_id_fkey FOREIGN KEY (customer_id) REFERENCES customers (id)
);

CREATE VIEW customer_details AS
SELECT
    c.id AS customer_id,
    c.name AS customer_name,
    a.street,
    a.city,
    a.state,
    a.country_code,
    a.zip_code
FROM customers AS c
JOIN addresses AS a KEY (id) <- c (address_id);

CREATE VIEW orders_by_country AS
SELECT
    cd.country_code,
    COUNT(*) AS order_count,
    SUM(o.amount) AS total_amount
FROM orders AS o
JOIN customer_details AS cd KEY (customer_id) <- o (customer_id)
GROUP BY ROLLUP (cd.country_code);

CREATE TABLE customer_addresses
(
    customer_id INTEGER NOT NULL,
    address_id INTEGER NOT NULL,
    CONSTRAINT customer_addresses_pkey             PRIMARY KEY (customer_id, address_id),
    CONSTRAINT customer_addresses_customer_id_fkey FOREIGN KEY (customer_id) REFERENCES customers (id),
    CONSTRAINT customer_addresses_address_id_fkey  FOREIGN KEY (address_id) REFERENCES addresses (id)
);

-- error, since it would invalidate foreign key join in orders_by_country
-- that uses customer_details
CREATE OR REPLACE VIEW customer_details AS
SELECT
    c.id AS customer_id,
    c.name AS customer_name,
    a.street,
    a.city,
    a.state,
    a.country_code,
    a.zip_code
FROM customers AS c
JOIN customer_addresses AS ca KEY (customer_id) -> c (id)
JOIN addresses AS a KEY (id) <- ca (address_id);

--
-- Test various error conditions
--

SELECT * FROM t1 JOIN t2 KEY (c3, c4) -> t3 (c1, c2);

SELECT * FROM t1 JOIN t2 KEY (c3, c4) -> t1 (c1);
SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c1, c2);
SELECT * FROM t1 JOIN t2 KEY (c3, c4) -> t1 (c1, c2, c3);
SELECT * FROM t1 JOIN t2 KEY (c3, c4, c5) -> t1 (c1, c2);

CREATE FUNCTION t2() RETURNS TABLE (c3 INTEGER, c4 INTEGER)
LANGUAGE sql
RETURN (1, 2);

SELECT * FROM t1 JOIN t2() KEY (c3, c4) -> t1 (c1, c2);

SELECT * FROM t1 JOIN t2 KEY (c3, c4) -> t1 (c1, c5);

DROP VIEW v1, v2;
CREATE VIEW v1 AS SELECT c1 AS c1_1, c1 AS c1_2, c2 AS c2_1, c2 AS c2_2 FROM t1;
CREATE VIEW v2 AS SELECT c3 AS c3_1, c3 AS c3_2, c4 AS c4_1, c4 AS c4_2 FROM t2;

SELECT * FROM v1 JOIN t2 KEY (c3, c4) -> v1 (c1_1, c2_1); -- ok

SELECT * FROM v1 JOIN t2 KEY (c3, c4) -> v1 (c1_1, c1_2);

SELECT * FROM v1 JOIN t2 KEY (c3, c4) -> v1 (c1_1, nonexistent);

/*
 * We don't need to check for duplicate columns,
 * since there is already such a check for foreign key constraints.
 * Let's test it anyway.
 */
SELECT * FROM v1 JOIN t2 KEY (c3, c3) -> v1 (c1_1, c1_1);

DROP VIEW v1;
CREATE VIEW v1 AS SELECT c1+0 AS c1_1, c1 AS c1_2, c2 AS c2_1, c2 AS c2_2 FROM t1;
SELECT * FROM v1 JOIN t2 KEY (c3, c4) -> v1 (c1_1, c2_1);

SELECT * FROM t1 JOIN
(
    SELECT c3, c4 FROM t2
    UNION ALL
    SELECT c3, c4 FROM t2
) AS u KEY (c3, c4) -> t1 (c1, c2);

SELECT * FROM
(
    SELECT c1, c2 FROM t1 WHERE c2 > 0
) AS u
JOIN t2 KEY (c3, c4) -> u (c1, c2);

SELECT *
FROM t1
JOIN
(
    SELECT * FROM t10
    JOIN t10 AS t10_2 KEY (c23, c24) <- t10 (c25, c26)
) AS q1 KEY (c23, c24) -> t1 (c1, c2);

SELECT *
FROM t1
JOIN
(
    SELECT
        t10.c23,
        t10.c24,
        t10_2.c25,
        t10_2.c26
    FROM t10
    JOIN t10 AS t10_2 KEY (c23, c24) <- t10 (c25, c26)
) AS q1 KEY (nonexistent, c24) -> t1 (c1, c2);

SELECT *
FROM t1
JOIN
(
    SELECT
        t10.c23,
        t10_2.c24
    FROM t10
    JOIN t10 AS t10_2 KEY (c23, c24) <- t10 (c25, c26)
) AS q1 KEY (c23, c24) -> t1 (c1, c2);

--
-- Test materialized views (not supported yet)
--

CREATE MATERIALIZED VIEW mv1 AS
SELECT c1, c2 FROM t1;

SELECT * FROM mv1 JOIN t2 KEY (c3, c4) -> mv1 (c1, c2);

DROP MATERIALIZED VIEW mv1;

--
-- Test nested foreign keyjoins
--
CREATE TABLE t12 (id integer PRIMARY KEY);
CREATE TABLE t13 (id integer PRIMARY KEY, a_id integer REFERENCES t12(id));
CREATE TABLE t14 (id integer PRIMARY KEY, b_id integer REFERENCES t13(id));

CREATE TABLE t15 (
    id integer,
    id2 integer,
    PRIMARY KEY (id, id2)
);
CREATE TABLE t16 (
    id integer,
    id2 integer,
    a_id integer,
    a_id2 integer,
    PRIMARY KEY (id, id2),
    FOREIGN KEY (a_id, a_id2) REFERENCES t15 (id, id2)
);
CREATE TABLE t17 (
    id integer,
    id2 integer,
    b_id integer,
    b_id2 integer,
    PRIMARY KEY (id, id2),
    FOREIGN KEY (b_id, b_id2) REFERENCES t16 (id, id2)
);

INSERT INTO t12 VALUES (1), (2), (3);
INSERT INTO t13 VALUES (4, 1), (5, 2);
INSERT INTO t14 VALUES (6, 4);
INSERT INTO t15 VALUES (1, 10), (2, 20), (3, 30);
INSERT INTO t16 VALUES (4, 40, 1, 10), (5, 50, 2, 20);
INSERT INTO t17 VALUES (6, 60, 4, 40);

--
-- Test nested foreign key joins
--
SELECT *
FROM t12
JOIN
    t13 JOIN t14 KEY (b_id) -> t13 (id)
KEY (a_id) -> t12 (id);

SELECT *
FROM t12
JOIN (t13 JOIN t14 KEY (b_id) -> t13 (id)) KEY (a_id) -> t12 (id);

--
-- Test nested foreign key joins with composite foreign keys
--
SELECT *
FROM t15
JOIN
    t16 JOIN t17 KEY (b_id, b_id2) -> t16 (id, id2)
KEY (a_id, a_id2) -> t15 (id, id2);

--
-- Explicit parenthesization:
--
SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 KEY (b_id, b_id2) -> t16 (id, id2)
) KEY (a_id, a_id2) -> t15 (id, id2);

--
-- Test swapping the column order:
--

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 KEY (b_id, b_id2) -> t16 (id, id2)
) KEY (a_id2, a_id) -> t15 (id2, id);

--
-- Test mismatched column orders between referencing and referenced sides:
--

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 KEY (b_id, b_id2) -> t16 (id, id2)
) KEY (a_id, a_id2) -> t15 (id2, id); -- error

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 KEY (b_id, b_id2) -> t16 (id2, id)
) KEY (a_id2, a_id) -> t15 (id2, id); -- error

--
-- Test partitioned tables
--

CREATE TABLE pt2
(
    c3 int not null,
    c4 int not null,
    CONSTRAINT pt2_pkey PRIMARY KEY (c3),
    CONSTRAINT pt2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1)
) PARTITION BY RANGE (c3);

CREATE TABLE pt2_1 PARTITION OF pt2 FOR VALUES FROM (1) TO (3);
CREATE TABLE pt2_2 PARTITION OF pt2 FOR VALUES FROM (3) TO (4);

CREATE TABLE pt3
(
    c5 int not null,
    c6 int not null,
    CONSTRAINT pt3_pkey PRIMARY KEY (c5),
    CONSTRAINT pt3_c5_fkey FOREIGN KEY (c5) REFERENCES pt2 (c3)
) PARTITION BY RANGE (c5);

CREATE TABLE pt3_1 PARTITION OF pt3 FOR VALUES FROM (1) TO (3);
CREATE TABLE pt3_2 PARTITION OF pt3 FOR VALUES FROM (3) TO (4);

INSERT INTO pt2 (c3, c4) VALUES (1, 100);
INSERT INTO pt2 (c3, c4) VALUES (3, 300);
INSERT INTO pt3 (c5, c6) VALUES (1, 1000);
INSERT INTO pt3 (c5, c6) VALUES (3, 3000);

SELECT * FROM t1 JOIN pt2 KEY (c3) -> t1 (c1) JOIN pt3 KEY (c5) -> pt2 (c3);
SELECT * FROM t1 JOIN pt2_1 KEY (c3) -> t1 (c1);

DROP TABLE pt3;
DROP TABLE pt2;

SELECT *
FROM (SELECT * FROM (SELECT c1, c2 FROM t1) AS q1(q1_c1, q1_c2)) q
JOIN t2 KEY (c3, c4) -> q (q1_c1, q1_c2);
-- equivalent to:
SELECT *
FROM (WITH q1 (q1_c1, q1_c2) AS (SELECT c1, c2 FROM t1) SELECT * FROM q1) q
JOIN t2 KEY (c3, c4) -> q (q1_c1, q1_c2);
