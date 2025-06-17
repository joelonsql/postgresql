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

DROP TABLE t1, t2, t3;
