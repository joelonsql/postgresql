CREATE TABLE t1
(
    c1 INT NOT NULL,
    c2 INT NOT NULL,
    PRIMARY KEY (c1, c2)
);

CREATE TABLE t2
(
    c3 INT NOT NULL,
    PRIMARY KEY (c3)
);

CREATE TABLE t3
(
    c4 INT NOT NULL,
    c5 INT NOT NULL,
    PRIMARY KEY (c4, c5)
);

CREATE TABLE t4
(
    c6 INT NOT NULL,
    c7 INT NOT NULL,
    c8 INT NOT NULL,
    c9 INT NOT NULL,
    c10 INT NOT NULL,
    PRIMARY KEY (c6, c7),
    FOREIGN KEY (c8) REFERENCES t2 (c3),
    FOREIGN KEY (c9, c10) REFERENCES t3 (c4, c5)
);

CREATE TABLE t5
(
    c11 INT NOT NULL,
    c12 INT NOT NULL,
    PRIMARY KEY (c11, c12)
);

CREATE TABLE t6
(
    c13 INT NOT NULL,
    c14 INT NOT NULL,
    PRIMARY KEY (c13, c14)
);

CREATE TABLE t7
(
    c15 INT NOT NULL,
    c16 INT NOT NULL,
    c17 INT NOT NULL,
    c18 INT NOT NULL,
    c19 INT NOT NULL,
    c20 INT NOT NULL,
    c21 INT NOT NULL,
    c22 INT NOT NULL,
    PRIMARY KEY (c15, c16),
    FOREIGN KEY (c17, c18) REFERENCES t4 (c6, c7),
    FOREIGN KEY (c19, c20) REFERENCES t5 (c11, c12),
    FOREIGN KEY (c21, c22) REFERENCES t6 (c13, c14)
);

INSERT INTO t1 (c1, c2) VALUES
(1, 10),
(2, 20),
(3, 30),
(4, 40),
(5, 50);

INSERT INTO t2 (c3) VALUES
(100),
(200),
(300),
(400);

INSERT INTO t3 (c4, c5) VALUES
(1000, 1001),
(2000, 2001),
(3000, 3001);

INSERT INTO t5 (c11, c12) VALUES
(500, 501),
(600, 601),
(700, 701);

INSERT INTO t6 (c13, c14) VALUES
(800, 801),
(900, 901),
(950, 951);

INSERT INTO t4 (c6, c7, c8, c9, c10) VALUES
(50, 51, 100, 1000, 1001),
(60, 61, 200, 2000, 2001),
(70, 71, 300, 3000, 3001),
(80, 81, 100, 1000, 1001),
(90, 91, 200, 2000, 2001);

INSERT INTO t7 (c15, c16, c17, c18, c19, c20, c21, c22) VALUES
(1, 2, 50, 51, 500, 501, 800, 801),
(3, 4, 60, 61, 600, 601, 900, 901),
(5, 6, 70, 71, 700, 701, 950, 951),
(7, 8, 80, 81, 500, 501, 800, 801),
(9, 10, 90, 91, 600, 601, 900, 901),
(11, 12, 50, 51, 700, 701, 950, 951),
(13, 14, 60, 61, 500, 501, 800, 801),
(15, 16, 70, 71, 600, 601, 900, 901),
(17, 18, 80, 81, 700, 701, 950, 951),
(19, 20, 90, 91, 500, 501, 800, 801);

--
-- Example 1
--
WITH topcte AS
(
    SELECT * FROM t4 JOIN t2 KEY (c3) <- t4 (c8)
)
SELECT * FROM
(
    SELECT *
    FROM topcte
    JOIN t7 KEY (c17, c18) -> topcte (c6, c7)
);

--
-- Example 2
--
SELECT * FROM
(
    WITH
    t7_cte (t7_c15, t7_c16, t7_c17, t7_c18, t7_c19, t7_c20, t7_c21, t7_c22) AS
    (
        SELECT c15, c16, c17, c18, c19, c20, c21, c22 FROM t7
    ),
    t2_cte AS
    (
        SELECT * FROM t2
    )
    SELECT
        t7_cte.t7_c15,
        t7_cte.t7_c16,
        t4_q.t4_c8,
        t4_q.t4_c9,
        t4_q.t4_c10
    FROM t7_cte
    JOIN
    (
        WITH t4_cte (t4_c6, t4_c7, t4_c8, t4_c9, t4_c10) AS (SELECT c6, c7, c8, c9, c10 FROM t4)
        SELECT * FROM t4_cte JOIN t2_cte KEY (c3) <- t4_cte (t4_c8)
    ) AS t4_q KEY (t4_c6, t4_c7) <- t7_cte (t7_c17, t7_c18)
    JOIN
    (
        WITH t5_cte (t5_c11, t5_c12) AS (SELECT c11, c12 FROM t5)
        SELECT * FROM t5_cte
    ) AS t5_q KEY (t5_c11, t5_c12) <- t7_cte (t7_c19, t7_c20)
    JOIN
    (
        WITH t6_cte (t6_c13, t6_c14) AS (SELECT c13, c14 FROM t6)
        SELECT * FROM t6_cte
    ) AS t6_q KEY (t6_c13, t6_c14) <- t7_cte (t7_c21, t7_c22)
) t7_q
JOIN
(
    WITH t2_cte (t2_c3) AS (SELECT c3 FROM t2)
    SELECT * FROM t2_cte
) t2_q
KEY (t2_c3) <- t7_q (t4_c8)
JOIN
(
    WITH t3_cte (t3_c4, t3_c5) AS (SELECT c4, c5 FROM t3)
    SELECT * FROM t3_cte
)
KEY (t3_c4, t3_c5) <- t7_q (t4_c9, t4_c10)
ORDER BY 1,2,3,4,5;
-- equivalent to:
SELECT * FROM
(
    SELECT
        t7_cte.t7_c15,
        t7_cte.t7_c16,
        t4_q.t4_c8,
        t4_q.t4_c9,
        t4_q.t4_c10
    FROM (SELECT c15, c16, c17, c18, c19, c20, c21, c22 FROM t7) AS t7_cte (t7_c15, t7_c16, t7_c17, t7_c18, t7_c19, t7_c20, t7_c21, t7_c22)
    JOIN
    (
        SELECT * FROM (SELECT c6, c7, c8, c9, c10 FROM t4) AS t4_cte (t4_c6, t4_c7, t4_c8, t4_c9, t4_c10)
        JOIN (SELECT * FROM t2) AS t2_cte KEY (c3) <- t4_cte (t4_c8)
    ) AS t4_q KEY (t4_c6, t4_c7) <- t7_cte (t7_c17, t7_c18)
    JOIN
    (
        SELECT * FROM (SELECT c11, c12 FROM t5) AS t5_cte (t5_c11, t5_c12)
    ) AS t5_q KEY (t5_c11, t5_c12) <- t7_cte (t7_c19, t7_c20)
    JOIN
    (
        SELECT * FROM (SELECT c13, c14 FROM t6) AS t6_cte (t6_c13, t6_c14)
    ) AS t6_q KEY (t6_c13, t6_c14) <- t7_cte (t7_c21, t7_c22)
) t7_q
JOIN
(
    SELECT * FROM (SELECT c3 FROM t2) AS t2_cte (t2_c3)
) t2_q
KEY (t2_c3) <- t7_q (t4_c8)
JOIN
(
    SELECT * FROM (SELECT c4, c5 FROM t3) AS t3_cte (t3_c4, t3_c5)
)
KEY (t3_c4, t3_c5) <- t7_q (t4_c9, t4_c10)
ORDER BY 1,2,3,4,5;

DROP TABLE t1, t2, t3, t4, t5, t6, t7;

-- Tables
CREATE TABLE t1 (
   t1_a int PRIMARY KEY
);

CREATE TABLE t2 (
   t2_a int PRIMARY KEY,
   FOREIGN KEY (t2_a) REFERENCES t1
);

CREATE TABLE t3 (
   t3_a int PRIMARY KEY,
   FOREIGN KEY (t3_a) REFERENCES t2
);

INSERT INTO t1 (t1_a) VALUES (1), (2), (3);
INSERT INTO t2 (t2_a) VALUES (1), (2);
INSERT INTO t3 (t3_a) VALUES (1);

-- Complex example
WITH c3 AS (SELECT * FROM t3)
SELECT * FROM (
  WITH c2 AS (SELECT * FROM t2)
  SELECT * FROM (
    SELECT * FROM c2
    JOIN c3 KEY (t3_a) -> c2 (t2_a)
  )
) q
JOIN t1 KEY (t1_a) <- q (t2_a);

WITH c3 AS (SELECT * FROM t3)
SELECT * FROM (
  WITH c2 AS (SELECT * FROM t2)
  SELECT * FROM (
    SELECT * FROM c2
    JOIN c3 KEY (t3_a) -> c2 (t2_a)
  )
) q
JOIN t1 ON t1.t1_a = q.t2_a;

-- Minimal example
SELECT * FROM (
  WITH c2 AS (SELECT * FROM t2)
  SELECT * FROM (
    SELECT * FROM c2
  )
) q
JOIN t1 KEY (t1_a) <- q (t2_a);

SELECT * FROM (
  WITH c2 AS (SELECT * FROM t2)
  SELECT * FROM (
    SELECT * FROM c2
  )
) q
JOIN t1 ON t1.t1_a = q.t2_a;

DROP TABLE t1, t2, t3;
