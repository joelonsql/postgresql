CREATE TABLE t_1_a (c1 text, c2 text, c3 text);
CREATE TABLE t_10_a (c1 text, c2 text, c3 text);
CREATE TABLE t_100_a (c1 text, c2 text, c3 text);

INSERT INTO t_1_a (c1, c2, c3) SELECT 'aaaa', 'aaaa', 'aaaa' FROM generate_series(1, 72e6);
INSERT INTO t_10_a (c1, c2, c3) SELECT repeat('aaaa',10), repeat('aaaa',10), repeat('aaaa',10) FROM generate_series(1, 32e6);
INSERT INTO t_100_a (c1, c2, c3) SELECT repeat('aaaa',100), repeat('aaaa',100), repeat('aaaa',100) FROM generate_series(1, 4e6);

CREATE TABLE t_1_mix (c1 text, c2 text, c3 text);
CREATE TABLE t_10_mix (c1 text, c2 text, c3 text);
CREATE TABLE t_100_mix (c1 text, c2 text, c3 text);

INSERT INTO t_1_mix (c1, c2, c3) SELECT ',"\.', ',"\.', ',"\.' FROM generate_series(1, 72e6);
INSERT INTO t_10_mix (c1, c2, c3) SELECT repeat(',"\.',10), repeat(',"\.',10), repeat(',"\.',10) FROM generate_series(1, 32e6);
INSERT INTO t_100_mix (c1, c2, c3) SELECT repeat(',"\.',100), repeat(',"\.',100), repeat(',"\.',100) FROM generate_series(1, 4e6);

CHECKPOINT;
VACUUM;

SELECT now();

\timing on

-- COPY TO ... FORMAT text
COPY t_1_a TO '/data/t_1_a.txt' (FORMAT text);
COPY t_10_a TO '/data/t_10_a.txt' (FORMAT text);
COPY t_100_a TO '/data/t_100_a.txt' (FORMAT text);

COPY t_1_mix TO '/data/t_1_mix.txt' (FORMAT text);
COPY t_10_mix TO '/data/t_10_mix.txt' (FORMAT text);
COPY t_100_mix TO '/data/t_100_mix.txt' (FORMAT text);

\timing off

TRUNCATE TABLE t_1_a, t_10_a, t_100_a;
TRUNCATE TABLE t_1_mix, t_10_mix, t_100_mix;

CHECKPOINT;
VACUUM;

\timing on

-- COPY FROM ... FORMAT text
COPY t_1_a FROM '/data/t_1_a.txt' (FORMAT text);
COPY t_10_a FROM '/data/t_10_a.txt' (FORMAT text);
COPY t_100_a FROM '/data/t_100_a.txt' (FORMAT text);

COPY t_1_mix FROM '/data/t_1_mix.txt' (FORMAT text);
COPY t_10_mix FROM '/data/t_10_mix.txt' (FORMAT text);
COPY t_100_mix FROM '/data/t_100_mix.txt' (FORMAT text);

\timing off

CHECKPOINT;
VACUUM;

\timing on

-- COPY TO ... FORMAT csv
COPY t_1_a TO '/data/t_1_a.csv' (FORMAT csv);
COPY t_10_a TO '/data/t_10_a.csv' (FORMAT csv);
COPY t_100_a TO '/data/t_100_a.csv' (FORMAT csv);

COPY t_1_mix TO '/data/t_1_mix.csv' (FORMAT csv);
COPY t_10_mix TO '/data/t_10_mix.csv' (FORMAT csv);
COPY t_100_mix TO '/data/t_100_mix.csv' (FORMAT csv);

\timing off

TRUNCATE TABLE t_1_a, t_10_a, t_100_a;
TRUNCATE TABLE t_1_mix, t_10_mix, t_100_mix;

CHECKPOINT;
VACUUM;

\timing on

-- COPY FROM ... FORMAT csv
COPY t_1_a FROM '/data/t_1_a.csv' (FORMAT csv);
COPY t_10_a FROM '/data/t_10_a.csv' (FORMAT csv);
COPY t_100_a FROM '/data/t_100_a.csv' (FORMAT csv);

COPY t_1_mix FROM '/data/t_1_mix.csv' (FORMAT csv);
COPY t_10_mix FROM '/data/t_10_mix.csv' (FORMAT csv);
COPY t_100_mix FROM '/data/t_100_mix.csv' (FORMAT csv);

\timing off

SELECT now();
