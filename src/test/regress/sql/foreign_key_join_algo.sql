CREATE SCHEMA fkjoins_test;

SET search_path TO fkjoins_test;

CREATE TABLE t1
(
    t1_id INT PRIMARY KEY
);

INSERT INTO t1 VALUES (1), (2), (3), (4), (5), (6), (7), (8), (9);

CREATE TABLE t2
(
    t2_id INT PRIMARY KEY,
    t2_t1_id INT REFERENCES t1,
    t2_t1_id_nn INT NOT NULL REFERENCES t1,
    t2_t1_id_u INT UNIQUE REFERENCES t1,
    t2_t1_id_nn_u INT NOT NULL UNIQUE REFERENCES t1
);

INSERT INTO t2
(t2_id, t2_t1_id, t2_t1_id_nn, t2_t1_id_u, t2_t1_id_nn_u) VALUES
(   10,     NULL,           1,       NULL,             2),
(   20,        3,           1,       NULL,             4),
(   30,     NULL,           5,          6,             7),
(   40,        3,           5,          8,             9),
(   50,        1,           6,          1,             3),
(   60,        1,           2,          3,             5),
(   70,        2,           3,          4,             1),
(   80,        5,           6,          2,             8);

CREATE TABLE t3a
(
    t3a_id INT PRIMARY KEY,
    t3a_t2_id INT REFERENCES t2,
    t3a_t2_id_nn INT NOT NULL REFERENCES t2,
    t3a_t2_id_u INT UNIQUE REFERENCES t2,
    t3a_t2_id_nn_u INT NOT NULL UNIQUE REFERENCES t2
);

INSERT INTO t3a
(t3a_id, t3a_t2_id, t3a_t2_id_nn, t3a_t2_id_u, t3a_t2_id_nn_u) VALUES
(   100,      NULL,           10,        NULL,            20),
(   200,        30,           10,        NULL,            40),
(   300,      NULL,           50,          60,            70),
(   400,        30,           50,          80,            10),
(   500,        10,           60,          10,            30),
(   600,        10,           20,          30,            50),
(   700,        20,           30,          40,            60),
(   800,        50,           60,          20,            80);

CREATE TABLE t3b
(
    t3b_id INT PRIMARY KEY,
    t3b_t2_id INT REFERENCES t2,
    t3b_t2_id_nn INT NOT NULL REFERENCES t2,
    t3b_t2_id_u INT UNIQUE REFERENCES t2,
    t3b_t2_id_nn_u INT NOT NULL UNIQUE REFERENCES t2
);

INSERT INTO t3b
(t3b_id, t3b_t2_id, t3b_t2_id_nn, t3b_t2_id_u, t3b_t2_id_nn_u) VALUES
(   100,      NULL,           20,        NULL,             30),
(   200,        40,           20,        NULL,             50),
(   300,      NULL,           60,          70,             80),
(   400,        40,           60,          10,             20),
(   500,        20,           70,          20,             40),
(   600,        20,           30,          50,             60),
(   700,        30,           40,          60,             70),
(   800,        60,           70,          30,             10);

CREATE TABLE t4a
(
    t4a_id INT PRIMARY KEY,
    t4a_t3a_id INT REFERENCES t3a,
    t4a_t3a_id_nn INT NOT NULL REFERENCES t3a,
    t4a_t3a_id_u INT UNIQUE REFERENCES t3a,
    t4a_t3a_id_nn_u INT NOT NULL UNIQUE REFERENCES t3a
);

INSERT INTO t4a
(t4a_id, t4a_t3a_id, t4a_t3a_id_nn, t4a_t3a_id_u, t4a_t3a_id_nn_u) VALUES
(  1000,       NULL,           100,         NULL,             200),
(  2000,        300,           100,         NULL,             400),
(  3000,       NULL,           500,          600,             700),
(  4000,        300,           500,          800,             100),
(  5000,        100,           600,          100,             300),
(  6000,        100,           200,          300,             500),
(  7000,        200,           300,          400,             600),
(  8000,        500,           600,          200,             800);

CREATE TABLE t4b
(
    t4b_id INT PRIMARY KEY,
    t4b_t3b_id INT REFERENCES t3b,
    t4b_t3b_id_nn INT NOT NULL REFERENCES t3b,
    t4b_t3b_id_u INT UNIQUE REFERENCES t3b,
    t4b_t3b_id_nn_u INT NOT NULL UNIQUE REFERENCES t3b
);

INSERT INTO t4b
(t4b_id, t4b_t3b_id, t4b_t3b_id_nn, t4b_t3b_id_u, t4b_t3b_id_nn_u) VALUES
( 1000,       NULL,           100,        NULL,            200),
( 2000,        300,           100,        NULL,            400),
( 3000,       NULL,           500,         600,            700),
( 4000,        300,           500,         800,            100),
( 5000,        100,           600,         100,            300),
( 6000,        100,           200,         300,            500),
( 7000,        200,           300,         400,            600),
( 8000,        500,           600,         200,            800);

CREATE TABLE t5a
(
    t5a_id INT PRIMARY KEY,
    t5a_t4a_id INT REFERENCES t4a,
    t5a_t4a_id_nn INT NOT NULL REFERENCES t4a,
    t5a_t4a_id_u INT UNIQUE REFERENCES t4a,
    t5a_t4a_id_nn_u INT NOT NULL UNIQUE REFERENCES t4a
);

INSERT INTO t5a
(t5a_id, t5a_t4a_id, t5a_t4a_id_nn, t5a_t4a_id_u, t5a_t4a_id_nn_u) VALUES
( 10000,       NULL,          1000,         NULL,            2000),
( 20000,       3000,          1000,         NULL,            4000),
( 30000,       NULL,          5000,         6000,            7000),
( 40000,       3000,          5000,         8000,            1000),
( 50000,       1000,          6000,         1000,            3000),
( 60000,       1000,          2000,         3000,            5000),
( 70000,       2000,          3000,         4000,            6000),
( 80000,       5000,          6000,         2000,            8000);

SET client_min_messages TO DEBUG2;

-- The purpose of these examples is just to reason about the
-- resulting uniqueness preservation set (U) and the functional
-- dependencies set (F), as a result of each join.

-- In the first section, there is just a single namespace,
-- and all relations are base tables, so the U and F data structures
-- are not needed to determine if the foreign key joins are valid or
-- not, since base tables trivially preserve rows/uniqueness.

SELECT *
FROM t1
JOIN t2 FOR KEY (t2_t1_id) -> t1 (t1_id);
-- The uniqueness preservation set of the referencing relation always
-- remains unchanged in the relation that is the result of a foreign key
-- join.  In this example, t2 is the referencing relation, and since its
-- a base table, it trivially preserve all its rows and uniqueness.
-- There is no UNIQUE constraint on the foreign key column.
-- The resulting U is therefore just t2.
--
-- Since there isn't any NOT NULL constraint on the foreign key column,
-- and the join type is INNER, the functional dependencies as a result
-- of this foreign key join, is the empty set.
-- 

SELECT *
FROM t1
JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id);
-- The NOT NULL constraint guarantee all rows in t2 will find a match
-- in t1, so the functional dependencies from t2, i.e. {(t2,t2)},
-- meaning t2 preserve all its rows, are inherited, and the new
-- functional dependency (t2,t1) is added to the set.

SELECT *
FROM t1
JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id);
-- We here also introduce t3a via an INNER join on a NOT NULL fk, which
-- references t2, that happens to preserve all its rows already, which
-- means that t3a will find a match for all its rows, resulting in
-- inheriting the functional dependencies from t3a, i.e. {(t3a,t3a)}, as
-- well as adding transitive functional dependencies from the left side
-- of the join, i.e. {(t2,t2), (t2,t1)} becomes {(t3a,t2), (t3a,t1)}.

SELECT *
FROM t1
JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id);
-- The LEFT join will preserve everything from before, and also add
-- transitive functional dependnecies, which we will next see why we
-- need. Note how (t3a,t3a) and (t4a,t4a) are both in F, meaning
-- that are the two base table instances that preserve all their rows.

SELECT *
FROM t1
JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn);
-- Here we introduce another instance of t1 under the alias t1_2,
-- by joining with t2 following its NOT NULL foreign key column.
-- Note how all functional dependencies are inherited and two new
-- are added, due to the transitive functional depdenencies.

-- In the next section, we will do foreign key joins that uses
-- derived tables on the referenced side, which then require
-- the referenced columns to recursively resolve to a base table
-- that preserve all rows and uniqueness of its keys.

SELECT *
FROM t5a
JOIN
(
    SELECT *
    FROM t1
    JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
    JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
    LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
    JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
) AS q FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn);
-- In the INNER join, all rows in t5a will find a match in q,
-- since t4a_id resolve to t4a that is both in U and (t4a,t4a) is in F.

SELECT *
FROM t4a
LEFT JOIN
(
    SELECT *
    FROM t5a
    JOIN
    (
        SELECT *
        FROM t1
        JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
        JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
        LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
        JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
    ) AS q FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn)
) AS q2 FOR KEY (t5a_t4a_id_nn_u) -> t4a (t4a_id);
-- Since we do a LEFT JOIN we preserve all rows of t4a and in additon
-- we join on a fk col for which there is both a NOT NULL and UNIQUE
-- constraint, so since t5a preserved all its rows, the join will
-- find a match for all rows, and the UNIQUE constraint on the fk cols
-- preserve the uniqueness of t4a, in additon to all rows preserved
-- due to the LEFT JOIN.


SELECT *
FROM t4a
LEFT JOIN
(
    WITH q AS
    (
        SELECT *
        FROM t1
        JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
        JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
        LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
        JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
    )
    SELECT *
    FROM t5a
    JOIN q FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn)
) AS q2 FOR KEY (t5a_t4a_id_nn_u) -> t4a (t4a_id);

CREATE VIEW q AS
SELECT
    t4a.t4a_id
FROM t1
JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
;

SELECT *
FROM t4a
LEFT JOIN
(
    SELECT *
    FROM t5a
    JOIN q FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn)
) AS q2 FOR KEY (t5a_t4a_id_nn_u) -> t4a (t4a_id);

SELECT *
FROM t4a
LEFT JOIN
(
    SELECT *
    FROM t5a
    JOIN (
        t1
        JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
        JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
        LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
        JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
    ) AS q FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn)
) AS q2 FOR KEY (t5a_t4a_id_nn_u) -> t4a (t4a_id);

SELECT *
FROM t4a
LEFT JOIN
(
    SELECT *
    FROM t5a
    JOIN (
        t1
        JOIN t2 FOR KEY (t2_t1_id_nn) -> t1 (t1_id)
        JOIN t3a FOR KEY (t3a_t2_id_nn) -> t2 (t2_id)
        LEFT JOIN t4a FOR KEY (t4a_t3a_id_nn) -> t3a (t3a_id)
        JOIN t1 AS t1_2 FOR KEY (t1_id) <- t2 (t2_t1_id_nn)
    ) FOR KEY (t4a_id) <- t5a (t5a_t4a_id_nn)
) AS q2 FOR KEY (t5a_t4a_id_nn_u) -> t4a (t4a_id);

RESET client_min_messages;

DROP SCHEMA fkjoins_test CASCADE;

RESET search_path;
