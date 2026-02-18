--
-- foreign_key_join_outer_rows
--
-- Demonstrates that a FULL JOIN inside a derived relation can introduce
-- NULL-valued rows in the referenced column list (breaking null-preservation)
-- from the other side of the join.  The FK join validation rejects these
-- queries because the referenced side may contain NULL key values.
--

-- ============================================================
-- Section A: Schema & data
-- ============================================================

CREATE TABLE fko_pktab (
    id integer PRIMARY KEY
);

CREATE TABLE fko_fk_nullable (
    id integer PRIMARY KEY,
    fk_id integer UNIQUE REFERENCES fko_pktab(id)
    -- fk_id is nullable
);

CREATE TABLE fko_fk_notnull (
    id integer PRIMARY KEY,
    fk_id integer NOT NULL REFERENCES fko_pktab(id)
);

-- One-to-one: NOT NULL + UNIQUE FK for no-false-positive tests
CREATE TABLE fko_fk_unique (
    id integer PRIMARY KEY,
    fk_id integer NOT NULL UNIQUE REFERENCES fko_pktab(id)
);

INSERT INTO fko_pktab VALUES (1), (2), (3);
INSERT INTO fko_fk_nullable VALUES (10, 1), (20, NULL);
INSERT INTO fko_fk_notnull VALUES (100, 1), (200, 2), (300, 3);
INSERT INTO fko_fk_unique VALUES (1, 1), (2, 2), (3, 3);

-- ============================================================
-- Section B: Baseline (correct behaviour)
-- ============================================================

-- Direct FK join: pktab LEFT JOIN fk_notnull.
-- Every referenced row matches exactly one referencing row -> COUNT(*) = 3.
SELECT COUNT(*)
FROM fko_pktab
LEFT JOIN fko_fk_notnull FOR KEY (fk_id) -> fko_pktab (id);

-- ============================================================
-- Section C: Outer rows rejected
-- ============================================================

-- The FULL JOIN between fko_pktab and fko_fk_nullable can introduce NULLs
-- in fko_pktab.id (breaking null-preservation).  Using this as the referenced side
-- of a FK join must be rejected because the referenced column may contain
-- NULLs, violating the PK-like invariant (unique + not null).

-- LEFT FK join variant: should error
SELECT q.ref_id, fko_fk_notnull.id AS fk_id
FROM (
    SELECT fko_pktab.id AS ref_id
    FROM fko_pktab
    FULL JOIN fko_fk_nullable FOR KEY (fk_id) -> fko_pktab (id)
) q
LEFT JOIN fko_fk_notnull FOR KEY (fk_id) -> q (ref_id);

-- COUNT variant: should also error
SELECT COUNT(*)
FROM (
    SELECT fko_pktab.id AS ref_id
    FROM fko_pktab
    FULL JOIN fko_fk_nullable FOR KEY (fk_id) -> fko_pktab (id)
) q
LEFT JOIN fko_fk_notnull FOR KEY (fk_id) -> q (ref_id);

-- INNER FK join variant: should also error (even though NULLs happen
-- to be filtered by equi-join, the referenced side is still invalid)
SELECT COUNT(*)
FROM (
    SELECT fko_pktab.id AS ref_id
    FROM fko_pktab
    FULL JOIN fko_fk_nullable FOR KEY (fk_id) -> fko_pktab (id)
) q
JOIN fko_fk_notnull FOR KEY (fk_id) -> q (ref_id);

-- ============================================================
-- Section C2: No false positives — referenced on preserved side
-- ============================================================

-- When the referenced side is on the preserved (outer) side of a
-- LEFT/RIGHT join, it is NOT subject to NULL introduction.  The inner
-- (NULLed) side is the referencing table, which doesn't affect the
-- referenced side's validity.

-- fko_pktab LEFT JOIN fko_fk_unique: referenced (fko_pktab) is on the
-- left (preserved) side, so no NULLs are introduced in fko_pktab.id.
-- Uniqueness is preserved because fko_fk_unique.fk_id is UNIQUE.
-- This should succeed.
SELECT COUNT(*)
FROM (
    SELECT fko_pktab.id AS ref_id
    FROM fko_pktab
    LEFT JOIN fko_fk_unique FOR KEY (fk_id) -> fko_pktab (id)
) q
JOIN fko_fk_notnull i FOR KEY (fk_id) -> q (ref_id);

-- ============================================================
-- Section E: Outer rows — simplified algorithm rejects all
-- ============================================================

-- The simplified algorithm tracks only a single "preserved" base table
-- through the join tree.  Once a LEFT JOIN introduces a second table,
-- the preserved value can only track one side.  All three cases below
-- fail because the subquery's preserved table is fko_clear_a (the LEFT
-- JOIN preserves the left/outer side), not fko_clear_b.

CREATE TABLE fko_clear_a (id integer PRIMARY KEY);
CREATE TABLE fko_clear_c (id integer PRIMARY KEY);
CREATE TABLE fko_clear_b (
    id integer PRIMARY KEY,
    a_id integer NOT NULL REFERENCES fko_clear_a(id),
    c_id integer NOT NULL REFERENCES fko_clear_c(id)
);
CREATE TABLE fko_clear_d (
    id integer PRIMARY KEY,
    b_id integer NOT NULL REFERENCES fko_clear_b(id)
);

INSERT INTO fko_clear_a VALUES (1), (2), (3), (4);
INSERT INTO fko_clear_c VALUES (10), (20), (30);
INSERT INTO fko_clear_b VALUES (100, 1, 10), (200, 2, 20), (300, 3, 30);
INSERT INTO fko_clear_d VALUES (1000, 100), (2000, 200);

-- E1: The simplified algorithm cannot track ghost-row clearing via
-- INNER JOIN, so this now correctly errors.
SELECT COUNT(*)
FROM (
    SELECT a.id AS a_id, b.id AS b_id, c.id AS c_id
    FROM fko_clear_a a
    LEFT JOIN fko_clear_b b FOR KEY (a_id) -> a (id)
    JOIN fko_clear_c c FOR KEY (id) <- b (c_id)
) sub
JOIN fko_clear_d d FOR KEY (b_id) -> sub (b_id);

-- E2: Without the clearing INNER JOIN, b stays unpreserved → error.
SELECT COUNT(*)
FROM (
    SELECT a.id AS a_id, b.id AS b_id
    FROM fko_clear_a a
    LEFT JOIN fko_clear_b b FOR KEY (a_id) -> a (id)
) sub
JOIN fko_clear_d d FOR KEY (b_id) -> sub (b_id);

-- E3: LEFT JOIN followed by LEFT JOIN — preserved tracks a, not b.
SELECT COUNT(*)
FROM (
    SELECT a.id AS a_id, b.id AS b_id, c.id AS c_id
    FROM fko_clear_a a
    LEFT JOIN fko_clear_b b FOR KEY (a_id) -> a (id)
    LEFT JOIN fko_clear_c c FOR KEY (id) <- b (c_id)
) sub
JOIN fko_clear_d d FOR KEY (b_id) -> sub (b_id);

DROP TABLE fko_clear_d;
DROP TABLE fko_clear_b;
DROP TABLE fko_clear_c;
DROP TABLE fko_clear_a;

-- ============================================================
-- Section D: Cleanup
-- ============================================================

DROP TABLE fko_fk_unique;
DROP TABLE fko_fk_notnull;
DROP TABLE fko_fk_nullable;
DROP TABLE fko_pktab;
