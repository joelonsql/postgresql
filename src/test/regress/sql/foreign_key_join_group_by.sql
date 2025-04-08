CREATE TABLE t1
(
    t1_id INT NOT NULL,
    PRIMARY KEY (t1_id)
);

CREATE TABLE t2
(
    t2_id INT NOT NULL,
    t2_t1_id INT NOT NULL,
    PRIMARY KEY (t2_id),
    FOREIGN KEY (t2_t1_id) REFERENCES t1 (t1_id)
);

CREATE TABLE t3
(
    t3_id INT NOT NULL,
    t3_t1_id INT NOT NULL,
    PRIMARY KEY (t3_id),
    FOREIGN KEY (t3_t1_id) REFERENCES t1 (t1_id)
);

INSERT INTO t1 (t1_id) VALUES (1), (2), (3), (4), (5);
INSERT INTO t2 (t2_id, t2_t1_id) VALUES (10, 1), (20, 1), (30, 2), (40, 4), (50, 4), (60, 4);
INSERT INTO t3 (t3_id, t3_t1_id) VALUES (100, 1), (200, 3), (300, 4), (400, 4);

SELECT t3.t3_id, q.t1_id, q.COUNT FROM
(
    SELECT
        t1.t1_id,
        COUNT(*)
    FROM t1
    -- the LEFT JOIN will preserve all rows of t1
    -- but the JOIN with t2 will cause t1 to lose its uniqueness preservation
    LEFT JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    -- however, thanks to the GROUP BY which column list
    -- matches a UNIQUE or PRIMARY KEY constraint,
    -- the uniqueness preservation property is restored
    GROUP BY t1.t1_id
) q
-- so that this foreign key is actually valid,
-- since all rows of t1 are preserved thanks to the LEFT JOIN
-- and the uniqueness property of t1 is preserved thanks to the GROUP BY.
JOIN t3 KEY (t3_t1_id) -> q (t1_id)
ORDER BY t3.t3_id, q.t1_id;

-- the query above is therefore valid and equivalent to the query below

SELECT t3.t3_id, q.t1_id, q.COUNT FROM
(
    SELECT
        t1.t1_id,
        COUNT(*)
    FROM t1
    LEFT JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t1.t1_id
) q
JOIN t3 ON t3.t3_t1_id = q.t1_id
ORDER BY t3.t3_id, q.t1_id;

SELECT t3.t3_id, q.t1_id, q.COUNT FROM
(
    SELECT
        t1.t1_id,
        COUNT(*)
    FROM t2
    RIGHT JOIN t1 KEY (t1_id) <- t2 (t2_t1_id)
    GROUP BY t1.t1_id
) q
JOIN t3 ON t3.t3_t1_id = q.t1_id
ORDER BY t3.t3_id, q.t1_id;

SELECT t3.t3_id, q.t1_id, q.COUNT FROM
(
    SELECT
        t1.t1_id,
        COUNT(*)
    FROM t1
    FULL JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t1.t1_id
) q
JOIN t3 ON t3.t3_t1_id = q.t1_id
ORDER BY t3.t3_id, q.t1_id;

-- error: referenced relation does not preserve all rows
-- The inner join will not preserve all t1 rows
SELECT * FROM
(
    SELECT t1_id
    FROM t1
    JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t1_id
) q JOIN t3 KEY (t3_t1_id) -> q (t1_id);

-- error: foreign key joins not supported for these relations
-- HAVING not allowed on referenced side of foreign key join
SELECT * FROM
(
    SELECT t1_id
    FROM t1
    LEFT JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t1_id
    HAVING COUNT(*) > 1
) q JOIN t3 KEY (t3_t1_id) -> q (t1_id);

-- ok since q is not the referenced side
-- at the referencing side of a foreign key join,
-- it suffices if there is an underlying foreign key constraint on the
-- referencing columns that references the referenced columns,
-- but there is no requirement on set containment or uniqueness
-- on the referencing side, only on the referenced side
SELECT t1.t1_id, q.COUNT FROM
(
    SELECT
        t2.t2_t1_id,
        COUNT(*)
    FROM t1
    JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t2.t2_t1_id
    HAVING COUNT(*) > 1
) q JOIN t1 KEY (t1_id) <- q (t2_t1_id)
ORDER BY t1.t1_id, q.COUNT;

-- error: GROUP BY column 1 is not a simple column reference
SELECT q.expr_result FROM
(
    SELECT t1.t1_id + 1 AS expr_result
    FROM t1
    LEFT JOIN t2 KEY (t2_t1_id) -> t1 (t1_id)
    GROUP BY t1.t1_id + 1
) q JOIN t3 KEY (t3_t1_id) -> q (expr_result);

CREATE TABLE t4
(
    t4_id_1 INT NOT NULL,
    t4_id_2 INT NOT NULL,
    t4_t1_id INT NOT NULL,
    PRIMARY KEY (t4_id_1, t4_id_2),
    FOREIGN KEY (t4_t1_id) REFERENCES t1 (t1_id)
);

INSERT INTO t4 (t4_id_1, t4_id_2, t4_t1_id)
VALUES (1000, 2000, 1), (3000, 4000, 1), (5000, 6000, 2), (7000, 8000, 2);

CREATE TABLE t5
(
    t5_id INT NOT NULL,
    t5_t4_id_1 INT NOT NULL,
    t5_t4_id_2 INT NOT NULL,
    PRIMARY KEY (t5_id),
    FOREIGN KEY (t5_t4_id_1, t5_t4_id_2) REFERENCES t4 (t4_id_1, t4_id_2)
);

INSERT INTO t5 (t5_id, t5_t4_id_1, t5_t4_id_2)
VALUES (10000, 1000, 2000), (20000, 1000, 2000), (30000, 5000, 6000);

-- error: all key columns must belong to the same table
-- This should trigger the error because t4_id_1 comes from t4a and t4_id_2 comes from t4b
SELECT * FROM
(
    SELECT t4a.t4_id_1, t4b.t4_id_2
    FROM t1
    JOIN t4 t4a KEY (t4_t1_id) -> t1 (t1_id)
    JOIN t4 t4b KEY (t4_t1_id) -> t1 (t1_id)
    GROUP BY t4a.t4_id_1, t4b.t4_id_2
) q JOIN t5 KEY (t5_t4_id_1, t5_t4_id_2) -> q (t4_id_1, t4_id_2);

DROP TABLE t1, t2, t3, t4, t5;
