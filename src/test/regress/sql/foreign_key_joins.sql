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
JOIN t2 KEY (c3) TO t1 (c1);
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

SELECT * FROM v1; -- ok

SELECT * FROM t1 JOIN t2 KEY (c3) TO t1 (c1); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3) TO t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) TO t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) TO t1 (c1,c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) FROM t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c1) FROM t1 (c3); -- error
SELECT * FROM t1 JOIN t2 KEY (c3) FROM t1 (c2); -- error
SELECT * FROM t1 JOIN t2 KEY (c4) FROM t1 (c1); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) FROM t1 (c1,c2); -- error

SELECT * FROM t2 JOIN t1 KEY (c1) FROM t2 (c3); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1) FROM t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) FROM t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) FROM t2 (c3,c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) TO t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) TO t2 (c4); -- error
SELECT * FROM t2 JOIN t1 KEY (c2) TO t2 (c3); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) TO t2 (c3,c4); -- error

ALTER TABLE t2 DROP CONSTRAINT t2_c3_fkey;

SELECT * FROM t1 JOIN t2 KEY (c3) TO t1 (c1); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) FROM t2 (c3); -- error

ALTER TABLE t1 ADD UNIQUE (c1,c2);
ALTER TABLE t2 ADD CONSTRAINT t2_c3_c4_fkey FOREIGN KEY (c3,c4) REFERENCES t1 (c1,c2);

CREATE VIEW v2 AS
SELECT * FROM t1 JOIN t2 KEY (c3,c4) TO t1 (c1,c2); -- ok
SELECT * FROM t1 JOIN t2 KEY (c3,c4) TO t1 (c1,c2); -- ok
SELECT * FROM v2; -- ok
\d+ v2

CREATE VIEW v3 AS
SELECT * FROM t2 JOIN t1 KEY (c1,c2) FROM t2 (c3,c4); -- ok
SELECT * FROM t2 JOIN t1 KEY (c1,c2) FROM t2 (c3,c4); -- ok
\d+ v3

SELECT * FROM v3; -- ok

SELECT * FROM t1 JOIN t2 KEY (c3) TO t1 (c1); -- error
SELECT * FROM t2 JOIN t1 KEY (c1) FROM t2 (c3); -- error
SELECT * FROM t1 JOIN t2 KEY (c3,c4) FROM t1 (c1,c2); -- error
SELECT * FROM t2 JOIN t1 KEY (c1,c2) TO t2 (c3,c4); -- error

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
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
JOIN t3 KEY (c5,c6) TO t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
LEFT JOIN t3 KEY (c5,c6) TO t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
RIGHT JOIN t3 KEY (c5,c6) TO t1 (c1,c2);

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
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
JOIN t4 KEY (c7,c8) TO t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
LEFT JOIN t4 KEY (c7,c8) TO t1 (c1,c2);

SELECT *
FROM t1
JOIN t2 KEY (c3,c4) TO t1 (c1,c2)
RIGHT JOIN t4 KEY (c7,c8) TO t1 (c1,c2);
