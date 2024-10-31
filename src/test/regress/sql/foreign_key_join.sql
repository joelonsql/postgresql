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
SELECT * FROM t1 JOIN t2 KEY (c3) -> t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) -> t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) -> t1 (c1,c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c1) <- t1 (c3); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) <- t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) <- t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) <- t1 (c1,c2); -- error

SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) <-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<- t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) /*comment*/<-/*comment*/ t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) < - t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) <- t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) <- t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) <- t2 (c3,c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) -> t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) -> t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) -> t2 (c3,c4); -- error

ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey; -- error

DROP VIEW v1;
ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey;

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

-- Recrate stuff for pg_dump tests
ALTER TABLE t2
    ADD CONSTRAINT t2_c3_fkey FOREIGN KEY (c3) REFERENCES t1 (c1);
CREATE VIEW v1 AS
SELECT *
FROM t1
JOIN t2 KEY (c3) -> t1 (c1);


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

--
-- Test CTEs
--

WITH
q1 AS
(
    SELECT c1 AS q1_c1, c2 AS q1_c2 FROM t1
),
q2 AS
(
    SELECT q1_c1 AS q2_c1, q1_c2 AS q2_c2 FROM q1
),
q3 AS
(
    SELECT c3 AS q3_c3, c4 AS q3_c4 FROM t2
),
q4 AS
(
    SELECT q3_c3 AS q4_c3, q3_c4 AS q4_c4 FROM q3
)
SELECT
    q2_c1,
    q2_c2,
    q4_c3,
    q4_c4
FROM q2 JOIN q4 KEY (q4_c3, q4_c4) -> q2 (q2_c1, q2_c2);

--
-- Test VIEWs
--

DROP VIEW v1;
DROP VIEW v2;
DROP VIEW v3;

CREATE VIEW v1 AS
SELECT c1 AS v1_c1, c2 AS v1_c2 FROM t1;

CREATE VIEW v2 AS
SELECT v1_c1 AS v2_c1, v1_c2 AS v2_c2 FROM v1;

CREATE VIEW v3 AS
SELECT c3 AS v3_c3, c4 AS v3_c4 FROM t2;

CREATE VIEW v4 AS
SELECT v3_c3 AS v4_c3, v3_c4 AS v4_c4 FROM v3;

SELECT
    v2_c1,
    v2_c2,
    v4_c3,
    v4_c4
FROM v2 JOIN v4 KEY (v4_c3, v4_c4) -> v2 (v2_c1, v2_c2);
