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
