CREATE TABLE t1_1_a (c1 text);
CREATE TABLE t1_10_a (c1 text);
CREATE TABLE t1_100_a (c1 text);

CREATE TABLE t2_1_a (c1 text, c2 text);
CREATE TABLE t2_10_a (c1 text, c2 text);
CREATE TABLE t2_100_a (c1 text, c2 text);

CREATE TABLE t3_1_a (c1 text, c2 text, c3 text);
CREATE TABLE t3_10_a (c1 text, c2 text, c3 text);
CREATE TABLE t3_100_a (c1 text, c2 text, c3 text);

INSERT INTO t1_1_a (c1) SELECT 'a' FROM generate_series(1, 1e6);
INSERT INTO t1_10_a (c1) SELECT repeat('a',10) FROM generate_series(1, 1e6);
INSERT INTO t1_100_a (c1) SELECT repeat('a',100) FROM generate_series(1, 1e6);

INSERT INTO t2_1_a (c1, c2) SELECT 'a', 'a' FROM generate_series(1, 1e6);
INSERT INTO t2_10_a (c1, c2) SELECT repeat('a',10), repeat('a',10) FROM generate_series(1, 1e6);
INSERT INTO t2_100_a (c1, c2) SELECT repeat('a',100), repeat('a',100) FROM generate_series(1, 1e6);

INSERT INTO t3_1_a (c1, c2, c3) SELECT 'a', 'a', 'a' FROM generate_series(1, 1e6);
INSERT INTO t3_10_a (c1, c2, c3) SELECT repeat('a',10), repeat('a',10), repeat('a',10) FROM generate_series(1, 1e6);
INSERT INTO t3_100_a (c1, c2, c3) SELECT repeat('a',100), repeat('a',100), repeat('a',100) FROM generate_series(1, 1e6);

CREATE TABLE t1_1_mix (c1 text);
CREATE TABLE t1_10_mix (c1 text);
CREATE TABLE t1_100_mix (c1 text);

CREATE TABLE t2_1_mix (c1 text, c2 text);
CREATE TABLE t2_10_mix (c1 text, c2 text);
CREATE TABLE t2_100_mix (c1 text, c2 text);

CREATE TABLE t3_1_mix (c1 text, c2 text, c3 text);
CREATE TABLE t3_10_mix (c1 text, c2 text, c3 text);
CREATE TABLE t3_100_mix (c1 text, c2 text, c3 text);

INSERT INTO t1_1_mix (c1) SELECT ',"\.' FROM generate_series(1, 1e6);
INSERT INTO t1_10_mix (c1) SELECT repeat(',"\.',10) FROM generate_series(1, 1e6);
INSERT INTO t1_100_mix (c1) SELECT repeat(',"\.',100) FROM generate_series(1, 1e6);

INSERT INTO t2_1_mix (c1, c2) SELECT ',"\.', ',"\.' FROM generate_series(1, 1e6);
INSERT INTO t2_10_mix (c1, c2) SELECT repeat(',"\.',10), repeat(',"\.',10) FROM generate_series(1, 1e6);
INSERT INTO t2_100_mix (c1, c2) SELECT repeat(',"\.',100), repeat(',"\.',100) FROM generate_series(1, 1e6);

INSERT INTO t3_1_mix (c1, c2, c3) SELECT ',"\.', ',"\.', ',"\.' FROM generate_series(1, 1e6);
INSERT INTO t3_10_mix (c1, c2, c3) SELECT repeat(',"\.',10), repeat(',"\.',10), repeat(',"\.',10) FROM generate_series(1, 1e6);
INSERT INTO t3_100_mix (c1, c2, c3) SELECT repeat(',"\.',100), repeat(',"\.',100), repeat(',"\.',100) FROM generate_series(1, 1e6);

\timing on

-- COPY TO ... FORMAT text
COPY t1_1_a TO '/tmp/t1_1_a.txt' (FORMAT text);
COPY t1_10_a TO '/tmp/t1_10_a.txt' (FORMAT text);
COPY t1_100_a TO '/tmp/t1_100_a.txt' (FORMAT text);

COPY t2_1_a TO '/tmp/t2_1_a.txt' (FORMAT text);
COPY t2_10_a TO '/tmp/t2_10_a.txt' (FORMAT text);
COPY t2_100_a TO '/tmp/t2_100_a.txt' (FORMAT text);

COPY t3_1_a TO '/tmp/t3_1_a.txt' (FORMAT text);
COPY t3_10_a TO '/tmp/t3_10_a.txt' (FORMAT text);
COPY t3_100_a TO '/tmp/t3_100_a.txt' (FORMAT text);

COPY t1_1_mix TO '/tmp/t1_1_mix.txt' (FORMAT text);
COPY t1_10_mix TO '/tmp/t1_10_mix.txt' (FORMAT text);
COPY t1_100_mix TO '/tmp/t1_100_mix.txt' (FORMAT text);

COPY t2_1_mix TO '/tmp/t2_1_mix.txt' (FORMAT text);
COPY t2_10_mix TO '/tmp/t2_10_mix.txt' (FORMAT text);
COPY t2_100_mix TO '/tmp/t2_100_mix.txt' (FORMAT text);

COPY t3_1_mix TO '/tmp/t3_1_mix.txt' (FORMAT text);
COPY t3_10_mix TO '/tmp/t3_10_mix.txt' (FORMAT text);
COPY t3_100_mix TO '/tmp/t3_100_mix.txt' (FORMAT text);

\timing off

TRUNCATE TABLE t1_1_a, t1_10_a, t1_100_a;
TRUNCATE TABLE t2_1_a, t2_10_a, t2_100_a;
TRUNCATE TABLE t3_1_a, t3_10_a, t3_100_a;
TRUNCATE TABLE t1_1_mix, t1_10_mix, t1_100_mix;
TRUNCATE TABLE t2_1_mix, t2_10_mix, t2_100_mix;
TRUNCATE TABLE t3_1_mix, t3_10_mix, t3_100_mix;

CHECKPOINT;

\timing on

-- COPY FROM ... FORMAT text
COPY t1_1_a FROM '/tmp/t1_1_a.txt' (FORMAT text);
COPY t1_10_a FROM '/tmp/t1_10_a.txt' (FORMAT text);
COPY t1_100_a FROM '/tmp/t1_100_a.txt' (FORMAT text);

COPY t2_1_a FROM '/tmp/t2_1_a.txt' (FORMAT text);
COPY t2_10_a FROM '/tmp/t2_10_a.txt' (FORMAT text);
COPY t2_100_a FROM '/tmp/t2_100_a.txt' (FORMAT text);

COPY t3_1_a FROM '/tmp/t3_1_a.txt' (FORMAT text);
COPY t3_10_a FROM '/tmp/t3_10_a.txt' (FORMAT text);
COPY t3_100_a FROM '/tmp/t3_100_a.txt' (FORMAT text);

COPY t1_1_mix FROM '/tmp/t1_1_mix.txt' (FORMAT text);
COPY t1_10_mix FROM '/tmp/t1_10_mix.txt' (FORMAT text);
COPY t1_100_mix FROM '/tmp/t1_100_mix.txt' (FORMAT text);

COPY t2_1_mix FROM '/tmp/t2_1_mix.txt' (FORMAT text);
COPY t2_10_mix FROM '/tmp/t2_10_mix.txt' (FORMAT text);
COPY t2_100_mix FROM '/tmp/t2_100_mix.txt' (FORMAT text);

COPY t3_1_mix FROM '/tmp/t3_1_mix.txt' (FORMAT text);
COPY t3_10_mix FROM '/tmp/t3_10_mix.txt' (FORMAT text);
COPY t3_100_mix FROM '/tmp/t3_100_mix.txt' (FORMAT text);

\timing off

CHECKPOINT;

\timing on

-- COPY TO ... FORMAT csv
COPY t1_1_a TO '/tmp/t1_1_a.csv' (FORMAT csv);
COPY t1_10_a TO '/tmp/t1_10_a.csv' (FORMAT csv);
COPY t1_100_a TO '/tmp/t1_100_a.csv' (FORMAT csv);

COPY t2_1_a TO '/tmp/t2_1_a.csv' (FORMAT csv);
COPY t2_10_a TO '/tmp/t2_10_a.csv' (FORMAT csv);
COPY t2_100_a TO '/tmp/t2_100_a.csv' (FORMAT csv);

COPY t3_1_a TO '/tmp/t3_1_a.csv' (FORMAT csv);
COPY t3_10_a TO '/tmp/t3_10_a.csv' (FORMAT csv);
COPY t3_100_a TO '/tmp/t3_100_a.csv' (FORMAT csv);

COPY t1_1_mix TO '/tmp/t1_1_mix.csv' (FORMAT csv);
COPY t1_10_mix TO '/tmp/t1_10_mix.csv' (FORMAT csv);
COPY t1_100_mix TO '/tmp/t1_100_mix.csv' (FORMAT csv);

COPY t2_1_mix TO '/tmp/t2_1_mix.csv' (FORMAT csv);
COPY t2_10_mix TO '/tmp/t2_10_mix.csv' (FORMAT csv);
COPY t2_100_mix TO '/tmp/t2_100_mix.csv' (FORMAT csv);

COPY t3_1_mix TO '/tmp/t3_1_mix.csv' (FORMAT csv);
COPY t3_10_mix TO '/tmp/t3_10_mix.csv' (FORMAT csv);
COPY t3_100_mix TO '/tmp/t3_100_mix.csv' (FORMAT csv);

\timing off

TRUNCATE TABLE t1_1_a, t1_10_a, t1_100_a;
TRUNCATE TABLE t2_1_a, t2_10_a, t2_100_a;
TRUNCATE TABLE t3_1_a, t3_10_a, t3_100_a;
TRUNCATE TABLE t1_1_mix, t1_10_mix, t1_100_mix;
TRUNCATE TABLE t2_1_mix, t2_10_mix, t2_100_mix;
TRUNCATE TABLE t3_1_mix, t3_10_mix, t3_100_mix;

CHECKPOINT;

\timing on

-- COPY FROM ... FORMAT csv
COPY t1_1_a FROM '/tmp/t1_1_a.csv' (FORMAT csv);
COPY t1_10_a FROM '/tmp/t1_10_a.csv' (FORMAT csv);
COPY t1_100_a FROM '/tmp/t1_100_a.csv' (FORMAT csv);

COPY t2_1_a FROM '/tmp/t2_1_a.csv' (FORMAT csv);
COPY t2_10_a FROM '/tmp/t2_10_a.csv' (FORMAT csv);
COPY t2_100_a FROM '/tmp/t2_100_a.csv' (FORMAT csv);

COPY t3_1_a FROM '/tmp/t3_1_a.csv' (FORMAT csv);
COPY t3_10_a FROM '/tmp/t3_10_a.csv' (FORMAT csv);
COPY t3_100_a FROM '/tmp/t3_100_a.csv' (FORMAT csv);

COPY t1_1_mix FROM '/tmp/t1_1_mix.csv' (FORMAT csv);
COPY t1_10_mix FROM '/tmp/t1_10_mix.csv' (FORMAT csv);
COPY t1_100_mix FROM '/tmp/t1_100_mix.csv' (FORMAT csv);

COPY t2_1_mix FROM '/tmp/t2_1_mix.csv' (FORMAT csv);
COPY t2_10_mix FROM '/tmp/t2_10_mix.csv' (FORMAT csv);
COPY t2_100_mix FROM '/tmp/t2_100_mix.csv' (FORMAT csv);

COPY t3_1_mix FROM '/tmp/t3_1_mix.csv' (FORMAT csv);
COPY t3_10_mix FROM '/tmp/t3_10_mix.csv' (FORMAT csv);
COPY t3_100_mix FROM '/tmp/t3_100_mix.csv' (FORMAT csv);

\timing off
