CREATE TABLE t1 (id INT NOT NULL PRIMARY KEY);
CREATE TABLE t2 (id INT PRIMARY KEY, t1_id INT NOT NULL REFERENCES t1(id));
CREATE TABLE t3 (id INT PRIMARY KEY, t2_id INT NOT NULL REFERENCES t2(id));
CREATE TABLE t4 (id INT PRIMARY KEY, t3_id INT NOT NULL REFERENCES t3(id));
CREATE TABLE t5 (id INT PRIMARY KEY, t4_id INT NOT NULL REFERENCES t4(id));

WITH
t1_cte AS (SELECT * FROM t1),
t2_cte AS (SELECT * FROM t2)
SELECT t5.id FROM
(
    WITH
    t3_cte AS (SELECT * FROM t3),
    t4_cte AS (SELECT * FROM t4)
    SELECT t4_cte.id FROM
    (
        SELECT t3_cte.id AS t3_id_in_unnamed_subquery
        FROM t3_cte
        JOIN t2_cte ON t2_cte.id = t3_cte.t2_id
    ) JOIN t4_cte ON t4_cte.t3_id = t3_id_in_unnamed_subquery
) AS named_subquery
JOIN t5 ON t5.t4_id = named_subquery.id;










