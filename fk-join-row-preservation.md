# Mathematical Specification of `update_row_preserving`

This document gives a formal specification of the row-preservation and
NOT NULL FK chain computation implemented by `update_row_preserving` in
`src/backend/parser/parse_fkjoin.c`. The intended audience is someone
comfortable with basic set theory and relational algebra who wants to
understand the algorithm without reading C.

## 1. Problem Statement

A foreign key join over derived relations (views, CTEs, subqueries)
is valid only if the referenced side preserves its base table's
uniqueness and complete row set. When multiple FK joins are composed
into a tree, each inner join filters rows: a referencing row survives
only if its FK value matches a row on the referenced side. We need to
track, at every node in the join tree, (a) which base tables still
have all their rows in the result, and (b) which base tables are
transitively "reachable" from which others through NOT NULL FK links.
These two properties together let us decide whether a derived relation
on the referenced side of a new FK join still represents the full,
unfiltered base table.

## 2. Definitions

### 2.1 Join Tree Nodes

A join tree is built from:

- **Leaf nodes**: a single base table `t`. When the table is accessed
  through a derived relation (subquery, CTE, or view) whose query
  applies filtering (WHERE, LIMIT, HAVING), the algorithm traverses
  into the derived relation and reaches the base table — but marks it
  as not row-preserving because the enclosing query may discard rows.
- **Join nodes**: `J = (J_f ⋈[f→p, nn, τ, δ] J_p)` where:
  - `J_f` is the subtree containing the referencing relation
  - `J_p` is the subtree containing the referenced relation
  - `f` is the RTEId of the referencing base table
  - `p` is the RTEId of the referenced base table
  - `nn` is a boolean: true iff the FK columns carry NOT NULL constraints
  - `τ ∈ {INNER, LEFT, RIGHT, FULL}` is the SQL join type
  - `δ ∈ {FROM, TO}` is the FK direction, determining which SQL side
    (left/right arg) is the referencing vs. referenced relation

### 2.2 Properties of a Node

Each node `J` carries four sets computed bottom-up:

| Symbol | Name | Type | Meaning |
|--------|------|------|---------|
| **U** | uniqueness preservation | ⊆ T | Base tables whose primary/unique key uniqueness is preserved through the joins in J's subtree |
| **R** | row-preserving set | ⊆ T | Base tables whose complete row set appears in J's result |
| **C** | NOT NULL FK chains | ⊆ T × T | Binary relation where (A, B) ∈ C means A ∈ R and A reaches B through a directed path of NOT NULL FK joins, each step's referenced table being row-preserving at the time the step was processed |
| **O** | outer rows | ⊆ T | Base tables whose columns may contain NULLs introduced by being on the inner side of an outer join within the subtree |

where T is the universe of base table RTEIds in J's subtree.

C is stored as a flat list of alternating (determinant, dependent)
RTEId pairs in the implementation.

### 2.3 FK Direction and SQL Sides

The FK direction δ maps the abstract referencing/referenced roles to
the concrete left/right arguments of the SQL JOIN:

| δ | Left arg | Right arg |
|---|----------|-----------|
| FROM | referencing (J_f) | referenced (J_p) |
| TO | referenced (J_p) | referencing (J_f) |

This mapping matters for outer joins, where LEFT JOIN preserves the
left arg and RIGHT JOIN preserves the right arg.

## 3. Base Cases (Leaf Nodes)

When the recursive traversal (`analyze_join_tree`) reaches a base
table `t` — either directly or after drilling through derived
relations — it initializes the properties as follows:

**Unfiltered** (the base table is accessed directly, or every
enclosing derived relation's query has no WHERE, LIMIT, OFFSET, or
HAVING):

    U = {t},  R = {t},  C = ∅,  O = ∅

**Filtered** (some enclosing derived relation applies WHERE, LIMIT,
OFFSET, or HAVING):

    U = {t},  R = ∅,  C = ∅,  O = ∅

Uniqueness is always preserved (filtering cannot break a unique
constraint). Row preservation is lost because the enclosing query may
exclude rows from `t`.

## 4. Uniqueness Preservation (U)

Uniqueness is handled by a separate function
(`update_uniqueness_preservation`) but is included here for
completeness, since the entry point checks U before accepting an FK
join.

At a join node `J = (J_f ⋈[f→p, nn, τ, δ] J_p)`:

    U = U_f  ∪  (U_p  if  unique(f) ∧ f ∈ U_f  else  ∅)

where `unique(f)` means the FK columns on the referencing base table
form a unique key (UNIQUE or PRIMARY KEY constraint). The referencing
side's uniqueness set is always inherited. The referenced side's set
is added only when the FK columns are unique *and* the referencing
base table itself preserves uniqueness — in that case the join is
one-to-one and cannot duplicate referenced rows.

## 5. Row Preservation and Chains (R, C) — `update_row_preserving`

This is the core algorithm. Given a join node
`J = (J_f ⋈[f→p, nn, τ, δ] J_p)` with inputs
`(R_f, C_f)` from the referencing subtree and `(R_p, C_p)` from the
referenced subtree, compute the output `(R', C')`.

The computation proceeds in three phases.

### Phase 1 — Outer Join Preservation

Define the predicate `outer_preserves(side, τ, δ)` that is true when
the SQL outer join unconditionally preserves a side's rows (padding
unmatched rows with NULLs):

| | Referencing side preserved | Referenced side preserved |
|---|---|---|
| INNER | no | no |
| LEFT, δ=FROM | yes (referencing is left) | no |
| LEFT, δ=TO | no | yes (referenced is left) |
| RIGHT, δ=FROM | no | yes (referenced is right) |
| RIGHT, δ=TO | yes (referencing is right) | no |
| FULL | yes | yes |

When a side is preserved by the outer join, its R and C are copied
directly into the result:

    R'_outer = (R_f  if outer_preserves(referencing, τ, δ)  else  ∅)
             ∪ (R_p  if outer_preserves(referenced, τ, δ)  else  ∅)
    C'_outer = (C_f  if outer_preserves(referencing, τ, δ)  else  ∅)
             ∪ (C_p  if outer_preserves(referenced, τ, δ)  else  ∅)

The outer join guarantee is unconditional — it does not depend on
NOT NULL or FK properties.

### Phase 1b — Outer Rows (O)

Compute the outer-rows set for the combined node. This tracks which
base tables may have "ghost" rows with NULL-filled columns introduced
by outer joins within the subtree.

**Propagate** from both inputs:

    O' = O_f ∪ O_p

**Clear** — the inner side of the join filters ghost rows for the
specific base table in the FK equi-join condition. Ghost rows have
NULLs in *all* of that table's columns (including the join column),
so they cannot match the equi-join and are eliminated. Only the
table named in the join condition is cleared; other tables in O
may have NULLs in non-join columns and their ghost rows can survive:

    if ¬outer_preserves(referencing, τ, δ):  O' = O' \ {f}
    if ¬outer_preserves(referenced, τ, δ):   O' = O' \ {p}

**Add** — the preserved (outer) side of an outer join introduces new
ghost rows on the inner side. When all FK columns are NOT NULL, the
FK guarantee ensures every referencing row matches a referenced row,
so no ghost rows appear on the referenced side:

    if outer_preserves(referenced, τ, δ):                O' = O' ∪ {f}
    if outer_preserves(referencing, τ, δ) ∧ ¬nn:         O' = O' ∪ {p}

Summary of the net effect on f and p after clear + add:

| τ | f in O'? | p in O'? |
|---|-----------|-----------|
| INNER | cleared | cleared |
| LEFT (referencing preserved) | cleared | added if ¬nn, else cleared |
| LEFT (referenced preserved) | added | cleared |
| RIGHT | mirror of LEFT | mirror of LEFT |
| FULL | added | added if ¬nn, else inherited |

(Plus any entries inherited from O_f ∪ O_p for other base tables.)

### Phase 2 — Guard Condition

If `¬nn ∨ p ∉ R_p`, no new chains can be derived and the result is
`R' = R'_outer, C' = C'_outer`.

**Rationale.** If the FK columns are nullable (`¬nn`), a referencing
row with NULL FK values will not match any referenced row, so it may
be lost in an inner join. If the referenced base table is not
row-preserving (`p ∉ R_p`), some referenced rows may be missing, and
a valid non-null FK value might fail to find its match.

### Phase 3 — Chain Extension

When `nn ∧ p ∈ R_p`, the FK constraint guarantees that *every*
referencing row will match exactly one referenced row (the FK value is
non-null and the referenced table is complete). Therefore, any table
whose rows were all reaching `f` through prior joins will continue to
have all its rows in the combined result.

**Step 3a.** Compute `anchor_set`: the set of tables
that "reach" the referencing base table f:

    anchor_set = { A : (A, f) ∈ C_f }  ∪  ( {f}  if  f ∈ R_f )

Intuitively, `anchor_set` contains every table A such that A is
row-preserving and has a NOT NULL FK chain ending at f. This includes
f itself when it is directly row-preserving.

**Step 3b.** Inherit from the referencing side. Selectively copy R
and C entries rooted at `anchor_set` members:

    R'_inherit = anchor_set
    C'_inherit = { (A, Y) ∈ C_f : A ∈ anchor_set }

When `outer_preserves(referencing, τ, δ)`, R'_outer ⊇ R'_inherit and
C'_outer ⊇ C'_inherit (since anchor_set ⊆ R_f), so the inherit sets
are redundant — the implementation skips this step as an optimization.

The unconditional `R'_inherit = anchor_set` (rather than filtering by
R_f membership) is valid because every member of `anchor_set` is
guaranteed to be in R_f: each (A, f) ∈ C_f implies A ∈ R_f by the
invariant on C (Section 6), and f is only added to `anchor_set` when
f ∈ R_f (Step 3a).

Only `anchor_set` members (and their chains) survive — tables in R_f or
C_f that do not reach f are *not* inherited, because the inner join
may have dropped some of their rows.

**Step 3c.** Extend chains across the FK boundary. Every table in
`anchor_set` now reaches p (and transitively everything p reaches):

    C'_extend = { (A, p) : A ∈ anchor_set }
              ∪ { (A, Y) : A ∈ anchor_set, (p, Y) ∈ C_p }

**Final result:**

    R' = R'_outer ∪ R'_inherit
    C' = C'_outer ∪ C'_inherit ∪ C'_extend

### Complete Formal Rule

Putting it all together for an INNER join (τ = INNER, no outer
preservation, so R'_outer = C'_outer = ∅):

    if ¬nn ∨ p ∉ R_p:
        R' = ∅,  C' = ∅

    if nn ∧ p ∈ R_p:
        anchor_set = { A : (A, f) ∈ C_f } ∪ ( {f} if f ∈ R_f )
        R' = anchor_set
        C' = { (A, Y) ∈ C_f : A ∈ anchor_set }
           ∪ { (A, p) : A ∈ anchor_set }
           ∪ { (A, Y) : A ∈ anchor_set, (p, Y) ∈ C_p }

For outer joins, the outer-preserved sides' R and C are always
included additively, and the chain extension logic applies on top.

## 6. Correctness Argument

### Invariant

After processing node J, the following invariant holds:

> **(A, B) ∈ C** iff there exists a directed path
> `A = t₀ → t₁ → ... → tₖ = B` in the join tree where:
> 1. A ∈ R (A's complete row set appears in J's result)
> 2. Each edge `tᵢ → tᵢ₊₁` corresponds to a NOT NULL FK join
>    where `tᵢ` is the referencing table and `tᵢ₊₁` is the
>    referenced table
> 3. At the time edge `tᵢ → tᵢ₊₁` was processed, `tᵢ₊₁` was
>    row-preserving (i.e., `tᵢ₊₁ ∈ R` of the referenced subtree)

### Base Case

At a leaf node, C = ∅ and the invariant holds vacuously.

### Inductive Step

Assume the invariant holds for both subtrees J_f and J_p. We show it
holds for the combined node J.

**Outer-join-preserved entries.** When an outer join preserves a
side, every row from that side appears in the result (possibly padded
with NULLs). The R and C entries from that side remain valid because
no rows are lost. The invariant is maintained by direct copy.

**Guard condition.** When `¬nn ∨ p ∉ R_p`, no new chain can be
valid: either null FK values could cause row loss (violating condition
1 of the invariant), or the referenced table is incomplete (violating
condition 3). Returning with only outer-preserved sets is correct.

**Chain extension.** When `nn ∧ p ∈ R_p`:

*Key lemma.* If A ∈ `anchor_set`, then every row of A in J_f's result
successfully joins with exactly one row in J_p's result.

*Proof.* A ∈ `anchor_set` means either A = f with f ∈ R_f, or (A, f)
∈ C_f. In both cases, A has a NOT NULL FK chain ending at f, so every
row of A in J_f's result is associated with a specific value of f's FK
columns. Since nn holds, these FK column values are non-null. Since p
∈ R_p, the referenced table is complete. The FK constraint guarantees
that every non-null FK value has a match. Therefore every row of A
survives the join, so A ∈ R' is justified.

*Inherit step.* We copy (A, Y) from C_f only when A ∈ `anchor_set`. By
the lemma, A remains row-preserving. The chain A → ... → Y was valid
in J_f (by the inductive hypothesis), and its rows are preserved, so
the chain remains valid in J.

*Extend step.* Adding (A, p): A ∈ `anchor_set` ensures A ∈ R', the
edge f → p is a NOT NULL FK join, and p ∈ R_p at the time of
processing. So conditions 1-3 of the invariant hold. Adding (A, Y)
for (p, Y) ∈ C_p extends the path through p; by the inductive
hypothesis on J_p, the chain p → ... → Y was valid, and prepending
the path from A through f to p preserves validity.

### Outer Rows (O)

- **Base case.** O = ∅ is trivially correct: a single base table has
  no joins, so no outer join can have introduced ghost rows.

- **Clear step.** When a side is inner (not preserved), the FK
  equi-join filters rows where the join column is NULL. Ghost rows
  for a base table have NULLs in *all* of that table's columns
  (including the join column), so they cannot match the equi-join
  and are eliminated. Removing the table from O is correct.

- **Add step.** When a side is preserved (outer), unmatched rows from
  the other side produce ghost rows with all-NULL columns on the
  inner side. Adding the inner-side base table to O is correct.
  Exception: when the FK columns carry NOT NULL constraints and the
  referencing side is preserved, every referencing row has a non-null
  FK value and the FK constraint guarantees a match exists. Therefore
  no unmatched referencing rows exist and no ghost rows appear on the
  referenced side.

**Selective inheritance.** Tables in R_f that are *not* in `anchor_set`
are correctly excluded from R'. Such a table B has no NOT NULL FK
chain to f. An inner join on f's FK columns may drop rows of B (when
B's rows pair with f-rows that happen to have null FK values or that
don't match). Without a guarantee that all of B's rows survive, B
cannot be in R'.

## 7. Worked Example

Consider three tables with FK constraints:

    orders(id, customer_id NOT NULL)  →FK  customers(id)
    customers(id, address_id NOT NULL)  →FK  addresses(id)

We compute R and C for the query:

```sql
FROM orders
JOIN customers FOR KEY (id) <- orders (customer_id)
JOIN addresses FOR KEY (id) <- customers (address_id)
```

Both joins are INNER with NOT NULL FK columns.

### Leaf Nodes

| Table | U | R | C |
|-------|---|---|---|
| orders | {orders} | {orders} | ∅ |
| customers | {customers} | {customers} | ∅ |
| addresses | {addresses} | {addresses} | ∅ |

### Join 1: orders ⋈ customers

Parameters: f = orders, p = customers, nn = true, τ = INNER

**Inputs:**

    R_f = {orders},  C_f = ∅
    R_p = {customers},  C_p = ∅

**Phase 1:** INNER join — no outer preservation.

**Phase 2:** nn = true, p = customers ∈ R_p — pass.

**Phase 3:**
- anchor_set: scan C_f for (A, orders) → none. orders ∈ R_f → **anchor_set = {orders}**
- Inherit: R' = anchor_set = {orders}. No chains in C_f → C' = ∅
- Extend: add (orders, customers). No (customers, Y) in C_p.

**Output:**

    R = {orders}
    C = {(orders, customers)}

Reading: "orders is row-preserving, and orders reaches customers
through a NOT NULL FK chain."

### Join 2: (orders ⋈ customers) ⋈ addresses

Parameters: f = customers, p = addresses, nn = true, τ = INNER

**Inputs (referencing side = result of Join 1):**

    R_f = {orders},  C_f = {(orders, customers)}
    R_p = {addresses},  C_p = ∅

**Phase 1:** INNER join — no outer preservation.

**Phase 2:** nn = true, p = addresses ∈ R_p — pass.

**Phase 3:**
- anchor_set: scan C_f for (A, customers): (orders, customers) matches →
  {orders}. customers ∈ R_f = {orders}? No.
  **anchor_set = {orders}**
- Inherit: R' = anchor_set = {orders}.
  C_f entries with det = orders: (orders, customers) → C' = {(orders, customers)}
- Extend: add (orders, addresses). No (addresses, Y) in C_p.

**Output:**

    R = {orders}
    C = {(orders, customers), (orders, addresses)}

Reading: "orders is row-preserving. orders reaches both customers and
addresses through NOT NULL FK chains."

### Interpretation

Notice that customers and addresses are *not* in R — an inner join
can drop their rows if no referencing row matches. Two practical
consequences follow:

1. **orders can be referenced.** Because orders ∈ R, this derived
   relation preserves the complete row set of orders. A further FK
   join with orders as the referenced base table would pass the
   `p ∈ R` check (Section 8).

2. **orders survives further joins through customers or addresses.**
   Because (orders, customers) and (orders, addresses) are in C, if
   customers or addresses serves as the referencing base table in a
   further FK join, orders will appear in anchor_set (Step 3a) and
   its row-preserving status will propagate through.

## 8. Entry Point Validation

At the top level, `transformAndValidateForeignKeyJoin` invokes the
recursive `analyze_join_tree` on each side of the FK join and then
checks three conditions on the referenced side:

1. **p ∈ U** — the referenced base table's uniqueness is preserved
   through all joins in the referenced subtree. This ensures the
   join produces at most one match per referencing row.

2. **p ∈ R** — the referenced base table's complete row set appears
   in the referenced subtree's result. This ensures every valid FK
   value will find its match.

3. **p ∉ O** — the referenced base table's columns are not subject
   to NULL introduction from outer joins within the derived relation.
   Ghost rows with NULL key columns violate the PK-like invariant
   (unique and not null) that the FK join depends on.

For base tables accessed directly (not through a derived relation),
all three conditions hold trivially and the checks are skipped.

These conditions are necessary for the FK constraint's guarantee
("every non-null FK value references an existing row") to hold
through derived relations. Without uniqueness preservation, the join
could fan out rows. Without row preservation, valid FK values could
fail to match.
