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
JOIN t2 FOR KEY (c3) -> t1 (c1);
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

SELECT * FROM t1 JOIN t2 FOR KEY (c3) -> t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 FOR KEY (c3) ->/*comment*/ t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 FOR KEY (c3) /*comment*/-> t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 FOR KEY (c3) /*comment*/->/*comment*/ t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 FOR KEY (c3) - > t1 (c2); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3) -/*comment*/> t1 (c2); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3) -> t1 (c2); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c4) -> t1 (c1); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c1) <- t1 (c3); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3) <- t1 (c2); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c4) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3,c4) <- t1 (c1,c2); -- error
SELECT * FROM t1 AS a JOIN t2 AS b FOR KEY (c3) -> a (c2); -- error

SELECT * FROM t2 JOIN t1 FOR KEY (c1) <- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 FOR KEY (c1) <-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 FOR KEY (c1) /*comment*/<- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 FOR KEY (c1) /*comment*/<-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 FOR KEY (c1) < - t2 (c3); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) </*comment*/- t2 (c3); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) <- t2 (c4); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c2) <- t2 (c3); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1,c2) <- t2 (c3,c4); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) -> t2 (c4); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c2) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1,c2) -> t2 (c3,c4); -- error
SELECT * FROM t2 AS a JOIN t1 AS b FOR KEY (c1) <- a (c4); -- error

ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey; -- error

DROP VIEW v1;
ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey;

/* Recreate contraint and view to test DROP CASCADE */
ALTER TABLE t2 ADD CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1);
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 FOR KEY (c3) -> t1 (c1);
ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey CASCADE; -- ok

SELECT * FROM t1 JOIN t2 FOR KEY (c3) -> t1 (c1); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) <- t2 (c3); -- error

ALTER TABLE t1 ADD UNIQUE (c1,c2);
ALTER TABLE t2 ADD CONSTRAINT t2_c3_c4_fkey FOREIGN KEY (c3,c4) REFERENCES t1 (c1,c2);

CREATE VIEW v2 AS
SELECT * FROM t1 JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2); -- ok
SELECT * FROM t1 JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2); -- ok
SELECT * FROM v2; -- ok
\d+ v2

CREATE VIEW v3 AS
SELECT * FROM t2 JOIN t1 FOR KEY (c1,c2) <- t2 (c3,c4); -- ok
SELECT * FROM t2 JOIN t1 FOR KEY (c1,c2) <- t2 (c3,c4); -- ok
\d+ v3

SELECT * FROM v3; -- ok

SELECT * FROM t1 JOIN t2 FOR KEY (c3) -> t1 (c1); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1) <- t2 (c3); -- error
SELECT * FROM t1 JOIN t2 FOR KEY (c3,c4) <- t1 (c1,c2); -- error
SELECT * FROM t2 JOIN t1 FOR KEY (c1,c2) -> t2 (c3,c4); -- error

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
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
JOIN t3 FOR KEY (c5,c6) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
LEFT JOIN t3 FOR KEY (c5,c6) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
RIGHT JOIN t3 FOR KEY (c5,c6) -> t1 (c1,c2);

--
-- Test composite foreign key joins with swapped column orders
--
SELECT *
FROM t1
JOIN t2 FOR KEY (c4,c3) -> t1 (c2,c1)
JOIN t3 FOR KEY (c6,c5) -> t1 (c2,c1);

--
-- Test mismatched column orders between referencing and referenced sides
--
SELECT *
FROM t1
JOIN t2 FOR KEY (c4,c3) -> t1 (c2,c1)
JOIN t3 FOR KEY (c6,c5) -> t1 (c1,c2); -- error

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
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
JOIN t4 FOR KEY (c7,c8) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
LEFT JOIN t4 FOR KEY (c7,c8) -> t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 FOR KEY (c3,c4) -> t1 (c1,c2)
RIGHT JOIN t4 FOR KEY (c7,c8) -> t1 (c1,c2);

-- Recreate stuff for pg_dump tests
ALTER TABLE t2
    ADD CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1);
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 FOR KEY (c3) -> t1 (c1);

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

DROP VIEW v1, v2, v3;

--
-- Test various error conditions
--

SELECT * FROM t1 JOIN t2 FOR KEY (c3, c4) -> t3 (c1, c2);

SELECT * FROM t1 JOIN t2 FOR KEY (c3, c4) -> t1 (c1);
SELECT * FROM t1 JOIN t2 FOR KEY (c3) -> t1 (c1, c2);
SELECT * FROM t1 JOIN t2 FOR KEY (c3, c4) -> t1 (c1, c2, c3);
SELECT * FROM t1 JOIN t2 FOR KEY (c3, c4, c5) -> t1 (c1, c2);

CREATE FUNCTION t2() RETURNS TABLE (c3 INTEGER, c4 INTEGER)
LANGUAGE sql
RETURN (1, 2);

SELECT * FROM t1 JOIN t2() FOR KEY (c3, c4) -> t1 (c1, c2);

SELECT * FROM t1 JOIN t2 FOR KEY (c3, c4) -> t1 (c1, c5);

--
-- Test materialized views (not supported yet)
--

CREATE MATERIALIZED VIEW mv1 AS
SELECT c1, c2 FROM t1;

SELECT * FROM mv1 JOIN t2 FOR KEY (c3, c4) -> mv1 (c1, c2);

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
    t13 JOIN t14 FOR KEY (b_id) -> t13 (id)
FOR KEY (a_id) -> t12 (id);

SELECT *
FROM t12
JOIN (t13 JOIN t14 FOR KEY (b_id) -> t13 (id)) FOR KEY (a_id) -> t12 (id);

--
-- Test nested foreign key joins with composite foreign keys
--
SELECT *
FROM t15
JOIN
    t16 JOIN t17 FOR KEY (b_id, b_id2) -> t16 (id, id2)
FOR KEY (a_id, a_id2) -> t15 (id, id2);

--
-- Explicit parenthesization:
--
SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 FOR KEY (b_id, b_id2) -> t16 (id, id2)
) FOR KEY (a_id, a_id2) -> t15 (id, id2);

--
-- Test swapping the column order:
--

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 FOR KEY (b_id, b_id2) -> t16 (id, id2)
) FOR KEY (a_id2, a_id) -> t15 (id2, id);

--
-- Test mismatched column orders between referencing and referenced sides:
--

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 FOR KEY (b_id, b_id2) -> t16 (id, id2)
) FOR KEY (a_id, a_id2) -> t15 (id2, id); -- error

SELECT *
FROM t15
JOIN
(
    t16 JOIN t17 FOR KEY (b_id, b_id2) -> t16 (id2, id)
) FOR KEY (a_id2, a_id) -> t15 (id2, id); -- error

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

SELECT * FROM t1 JOIN pt2 FOR KEY (c3) -> t1 (c1) JOIN pt3 FOR KEY (c5) -> pt2 (c3);
SELECT * FROM t1 JOIN pt2_1 FOR KEY (c3) -> t1 (c1);

DROP TABLE pt3;
DROP TABLE pt2;

--
-- Test that derived relations are rejected as FK join operands
--

-- Subquery as operand: error
SELECT * FROM (SELECT c1, c2 FROM t1) AS q
JOIN t2 FOR KEY (c3, c4) -> q (c1, c2);

-- View as operand: error
CREATE VIEW v1 AS SELECT c1, c2 FROM t1;
SELECT * FROM v1 JOIN t2 FOR KEY (c3, c4) -> v1 (c1, c2);
DROP VIEW v1;

-- CTE as operand: error
WITH q AS (SELECT c1, c2 FROM t1)
SELECT * FROM q JOIN t2 FOR KEY (c3, c4) -> q (c1, c2);

