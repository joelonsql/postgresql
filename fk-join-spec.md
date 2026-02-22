# Foreign Key Joins — Mathematical Specification

## 1. Notation

Relations are multisets (bags) of tuples. We write π_X(M) for the
multiset projection of M onto columns X: each tuple in M is
restricted to the columns in X, preserving multiplicities. The join
of relations R and S on predicate θ is written R ⋈_θ S.

## 2. Foreign Key Join

**Definition 1** (Foreign Key Join). A foreign key join combines a
referencing relation R with a referenced relation S on column lists
**a** ⊆ cols(R) and **b** ⊆ cols(S).

**Traceability.** Every column in **a** traces — by projection and
renaming only, no expressions or aggregation — to a column of a
single base table r. Likewise every column in **b** traces to a
single base table s. Write **a′**, **b′** for the corresponding
base-table columns.

**Validity.** The foreign key join is valid if and only if:

1. A referential constraint FK(r, **a′**, s, **b′**) exists.
2. Multiset equality[^1] holds:

        π_b(S) = π_b′(s)

**Result.** A valid foreign key join produces

    R ⋈_{a = b} S

under any join type τ ∈ {inner, left, right, full}.

Condition 2 is trivially satisfied when S is the base table s itself.
For derived relations (views, CTEs, subqueries), the database must
verify it statically from the schema and query structure.

[^1]: Two multisets are equal if and only if every element appears
    with the same multiplicity, including elements containing NULL.

## 3. Static Verification

For base tables, Condition 2 holds trivially: π_b(S) = π_b′(s) when
S = s and **b** = **b′**.

For derived relations, the database cannot evaluate π_b(S) at parse
time — it depends on the data. Instead, it must verify Condition 2
from the query structure alone.

We define a static analysis that is *sound* but not *complete*: if it
accepts, Condition 2 is guaranteed to hold (soundness); but it may
reject some queries for which Condition 2 does in fact hold
(incompleteness). Soundness means no false positives (accepting
invalid joins); incompleteness means some false negatives (rejecting
valid ones).

The approach: track four properties — uniqueness, row preservation,
null preservation, and chain reachability — bottom-up through the
join tree, then check three conditions at the entry point.

## 4. Join Trees

### 4.1. Nodes

A join tree is built from:

- **Leaf nodes**: a base table t, optionally marked as *filtered*. A
  leaf is filtered when the base table is accessed through a derived
  relation whose query restricts rows (through selection, limit, or
  similar mechanisms). An unfiltered leaf accesses the complete base
  table.

- **FK join nodes**: J = (J_f ⋈[f→p, nn, τ] J_p) with parameters:
  - J_f: the subtree containing the referencing relation
  - J_p: the subtree containing the referenced relation
  - f: the base table identifier of the referencing table
  - p: the base table identifier of the referenced table
  - nn: a boolean — true iff the FK columns carry NOT NULL constraints
  - τ ∈ {inner, left, right, full}: the join type

Only FK joins appear as join nodes. Non-FK joins inside a derived
relation prevent analysis — the algorithm cannot reason about their
row-preservation properties, so their output sets are empty.

### 4.2. Outer Preservation

Define the predicate outer(side, τ) as true when the given side is on
the outer (preserved) side of the join. In a FK join
J = (J_f ⋈ J_p), "left" corresponds to J_f (referencing) and "right"
corresponds to J_p (referenced):

| τ | outer_f (referencing preserved) | outer_p (referenced preserved) |
|---|---|---|
| inner | false | false |
| left | true | false |
| right | false | true |
| full | true | true |

When a side is preserved, all its rows appear in the output. Rows
without a match on the other side are padded with NULLs.

## 5. Tracked Properties

Each node J carries four properties computed bottom-up over the
universe T of base table identifiers in J's subtree:

| Symbol | Name | Type | Meaning |
|--------|------|------|---------|
| **U** | uniqueness | ⊆ T | Tables whose unique-key uniqueness is preserved through all joins in J's subtree |
| **R** | row preservation | ⊆ T | Tables whose complete row set appears in J's result |
| **N** | null preservation | ⊆ T | Tables whose key-column values are intact (not overwritten by NULLs from outer-join padding) |
| **C** | chains | ⊆ T × T | (A, B) ∈ C means A reaches B via a directed NOT NULL FK path with A ∈ R |

**Chain invariant.** (A, B) ∈ C if and only if there exists a
directed path A = t₀ → t₁ → ··· → tₖ = B where:

1. A ∈ R (A's complete row set appears in J's result)
2. Each edge tᵢ → tᵢ₊₁ corresponds to a NOT NULL FK join with tᵢ as
   the referencing table and tᵢ₊₁ as the referenced table
3. At the time edge tᵢ → tᵢ₊₁ was processed, tᵢ₊₁ was row-preserving
   (tᵢ₊₁ ∈ R of the referenced subtree at that node)

## 6. Base Cases

At a leaf node for base table t:

**Unfiltered:**

    U = {t},  R = {t},  N = {t},  C = ∅

**Filtered:**

    U = {t},  R = ∅,  N = {t},  C = ∅

Uniqueness is always preserved: filtering cannot create duplicate
keys. Row preservation is lost because filtering may exclude rows.
Null preservation holds because no join padding has occurred.

## 7. Propagation Rules

Given J = (J_f ⋈[f→p, nn, τ] J_p) with input properties
(U_f, R_f, N_f, C_f) from the referencing subtree and
(U_p, R_p, N_p, C_p) from the referenced subtree, compute the output
properties (U', R', N', C').

### 7.1. Outer Preservation Predicate

The shorthand outer_f and outer_p denotes whether the referencing and
referenced sides are outer-preserved, as defined in §4.2:

| τ | outer_f | outer_p |
|---|---------|---------|
| inner | false | false |
| left | true | false |
| right | false | true |
| full | true | true |

### 7.2. Uniqueness (U)

    U' = U_f ∪ (U_p if unique(f) ∧ f ∈ U_f else ∅)

where unique(f) means the FK columns form a unique key on f.

The referencing side's uniqueness set is always inherited. The
referenced side's set is inherited only when the FK columns are unique
and f itself preserves uniqueness — making the join one-to-one, so no
referenced rows are duplicated.

### 7.3. Null Preservation (N)

Start with the union of both inputs:

    N' = N_f ∪ N_p

Then remove entries from the null-padded (inner) side:

- If outer_f ∧ ¬nn: set N' = N' \ N_p.

  When the referencing side is preserved, unmatched referencing rows
  produce ghost rows with NULLs on the referenced side. All tables
  from J_p have their column values corrupted in these ghost rows.

  Exception: if nn holds, the FK guarantee ensures every referencing
  row matches a referenced row (the FK value is non-null and the
  constraint ensures a match exists). No unmatched referencing rows
  exist, so no ghost rows appear and N_p is not removed.

- If outer_p: set N' = N' \ N_f.

  When the referenced side is preserved, unmatched referenced rows
  produce ghost rows with NULLs on the referencing side. All tables
  from J_f have their column values corrupted. This removal is
  unconditional — the FK constraint provides no guarantee in this
  direction (not every referenced row need be referenced).

**Net effect on entries from each side:**

| τ | N_f entries | N_p entries |
|---|-------------|-------------|
| inner | inherited | inherited |
| left | inherited | removed if ¬nn; inherited if nn |
| right | removed | inherited |
| full | removed | removed if ¬nn; inherited if nn |

Here "inherited" means the entries pass through from the input, and
"removed" means they are unconditionally absent from the output.

### 7.4. Row Preservation and Chains (R, C)

The computation proceeds in three phases.

**Phase 1 — Outer join preservation.** Entries from preserved sides
are copied unconditionally:

    R'_outer = (R_f if outer_f else ∅) ∪ (R_p if outer_p else ∅)
    C'_outer = (C_f if outer_f else ∅) ∪ (C_p if outer_p else ∅)

**Phase 2 — Guard.** If any of the following conditions fail, set
R' = R'_outer, C' = C'_outer, and stop:

    nn  ∧  f ∈ N_f  ∧  p ∈ R_p

The guard requires three things:

- nn: the FK columns carry NOT NULL constraints on the base table.
- f ∈ N_f: the FK columns have not been corrupted by null-padding
  from prior outer joins. Even with NOT NULL base constraints, prior
  joins may have introduced ghost rows with NULL FK values; f ∈ N_f
  confirms this has not happened.
- p ∈ R_p: the referenced base table's complete row set appears in
  J_p's result.

Without all three, a referencing row may fail to find its match:
either because its FK value is NULL or corrupted, or because the
matching referenced row is missing.

**Phase 3 — Chain extension.** When the guard passes:

*Step 3a.* Compute the anchor set — tables that reach f:

    anchor = {A : (A, f) ∈ C_f} ∪ ({f} if f ∈ R_f)

Each member of anchor is row-preserving and connected to f through a
NOT NULL FK chain (or is f itself when f ∈ R_f).

*Step 3b.* Inherit from the referencing side. Copy R and C entries
rooted at anchor members:

    R'_inherit = anchor
    C'_inherit = {(A, Y) ∈ C_f : A ∈ anchor}

Every member of anchor is in R_f (by the chain invariant and the
condition on f in Step 3a), so R'_inherit ⊆ R_f. Tables in R_f that
do not reach f are excluded because the join may drop some of their
rows.

*Step 3c.* Extend chains across the FK boundary. Each anchor member
now reaches p, and transitively everything p reaches:

    C'_extend = {(A, p) : A ∈ anchor}
              ∪ {(A, Y) : A ∈ anchor, (p, Y) ∈ C_p}

**Final result:**

    R' = R'_outer ∪ R'_inherit
    C' = C'_outer ∪ C'_inherit ∪ C'_extend

### 7.5. GROUP BY

When a derived relation applies GROUP BY, it modifies the four
properties of the underlying relation's output before the result is
used in further joins. Let (U_in, R_in, N_in, C_in) be the properties
of the underlying relation.

**Conditions for uniqueness restoration.** GROUP BY can restore
uniqueness when:

- Every grouping expression is a direct column reference (no computed
  expressions)
- All grouping columns reference the same base table t
- The grouping columns cover all columns of some unique key on t

**Effect on the four properties.** When the conditions above are met:

- **U**: add t to U_in (uniqueness restored). This holds even when a
  group-level filter is present, since uniqueness is about
  deduplication, not row completeness.
- **R**: propagate R_in, but only if no group-level filter is present
  (a group-level filter may discard groups, breaking row
  preservation).
- **C**: propagate C_in (same condition as R).
- **N**: propagate N_in only if all columns of the matched unique key
  carry NOT NULL constraints. If any column is nullable, GROUP BY
  collapses all NULL values in that column into a single group,
  producing a NULL key value that may not correspond to any base
  table row. In this case, N is set to ∅.

When GROUP BY does not restore uniqueness, or when a group-level
filter is present, R, C, and N are not propagated.

## 8. Acceptance Criterion

At the entry point, the algorithm computes the four properties for the
referenced subtree J_p. The FK join is accepted if and only if:

    p ∈ U  ∧  p ∈ R  ∧  p ∈ N

where U, R, N are the output properties of J_p.

- **p ∈ U**: the referenced base table's unique key is preserved,
  ensuring each referencing row matches at most one referenced row.
- **p ∈ R**: the referenced base table's complete row set appears,
  ensuring every valid FK value finds its match.
- **p ∈ N**: the referenced base table's key-column values are intact,
  ensuring the unique key functions correctly (no spurious NULLs from
  outer-join padding or GROUP BY on nullable columns).

For base tables accessed directly (not through a derived relation),
all three conditions hold trivially.

## 9. Soundness

**Theorem.** If the acceptance criterion holds (p ∈ U ∧ p ∈ R ∧ p ∈ N
on the referenced side's output), then π_b(S) = π_b′(s).

*Proof sketch.* The three conditions together establish that the
referenced side S contains the same key values as the base table s
with the same multiplicities:

- p ∈ R guarantees that all rows of s appear in S (no row loss). Every
  key value in π_b′(s) appears in π_b(S) with at least its original
  multiplicity.
- p ∈ U guarantees that p's unique key is preserved through all joins
  in J_p (no row duplication). Since the key columns of s carry a
  unique constraint, and this uniqueness is preserved, no key value in
  π_b(S) appears with multiplicity greater than one.
- p ∈ N guarantees that the key-column values have not been corrupted
  by NULLs. Without this, ghost rows from outer joins could introduce
  NULL key values, or GROUP BY could collapse multiple NULL rows into
  one, violating the multiset equality.

Together, these imply π_b(S) = π_b′(s): every key value appears
exactly once, and no spurious values are introduced.

### 9.1. Chain Invariant Proof

The chain invariant (§5) is proved by structural induction on the
join tree.

**Base case.** At a leaf node, C = ∅ and the invariant holds
vacuously.

**Inductive step.** Assume the invariant holds for both subtrees J_f
and J_p. We show it holds for J = (J_f ⋈[f→p, nn, τ] J_p).

*Outer-preserved entries.* When a side is preserved by the outer join,
all its rows appear in the result (possibly padded with NULLs on the
other side). The R and C entries from that side remain valid because no
rows are lost. The invariant is maintained by direct copy.

*Guard condition.* When ¬nn ∨ f ∉ N_f ∨ p ∉ R_p, no new chain is
valid: either NULL FK values could cause row loss (violating condition
1 of the invariant), or the FK columns have been corrupted by prior
outer joins (also violating condition 1), or the referenced table is
incomplete (violating condition 3). Returning with only
outer-preserved sets is correct.

*Chain extension — key lemma.* If A ∈ anchor, then every row of A in
J_f's result survives the join.

*Proof.* A ∈ anchor means either A = f with f ∈ R_f, or (A, f) ∈ C_f.
In both cases, A has a NOT NULL FK chain ending at f, so every row of
A in J_f is associated with a specific value of f's FK columns. Since
f ∈ N_f (from the guard), these FK values have not been corrupted by
prior joins. Since nn holds, they are non-null. Since p ∈ R_p, the
referenced table is complete. The FK constraint guarantees that every
non-null FK value references an existing row. Therefore every row of A
matches exactly one row in J_p and survives the join.

*Inherit step.* Chains (A, Y) from C_f are copied only when
A ∈ anchor. By the lemma, A remains row-preserving. By the inductive
hypothesis, the chain from A to Y was valid in J_f. Since A's rows are
preserved, the chain remains valid in J.

*Extend step.* The new chain (A, p) is valid: A ∈ anchor ensures
A ∈ R', the edge f → p is a NOT NULL FK join, and p ∈ R_p holds at
the time of processing — satisfying all three conditions of the chain
invariant. Extensions (A, Y) for (p, Y) ∈ C_p compose the path from A
through p to Y; by the inductive hypothesis on J_p, the chain
p → ··· → Y was valid, and prepending the path from A through f to p
preserves validity.

### 9.2. Null Preservation Correctness

*Base case.* N = {t} is trivially correct: a single base table has no
joins, so its key-column values are intact.

*Remove step (outer joins).* When a side is outer-preserved, unmatched
rows from the preserved side produce ghost rows with all-NULL columns
on the inner side. Removing the inner side's N entries is correct
because those tables' key columns may now contain spurious NULLs.

Exception: when the referencing side is outer-preserved and nn holds,
every referencing row has a non-null FK value and the FK constraint
guarantees a match. No unmatched referencing rows exist, so no ghost
rows appear on the referenced side. Therefore N_p is not removed.

*Remove step (GROUP BY).* When GROUP BY on a nullable column restores
uniqueness, it collapses all NULL values into a single group. This
group's key value is NULL, which may not correspond to any base table
row (the base table may have multiple rows with NULL in that column,
or none). Removing the table from N is correct.

### 9.3. Selective Inheritance

Tables in R_f that are not in anchor are correctly excluded from R'.
Such a table B has no NOT NULL FK chain to f. The join on f's FK
columns may drop some of B's rows — specifically, rows paired with
f-rows whose FK values are NULL or whose FK columns have been
corrupted by prior joins. Without a guarantee that all of B's rows
survive, B cannot be in R'.

## 10. Conservativeness

The analysis is deliberately conservative. The following classes of
valid queries are rejected:

1. **Non-FK joins preserving rows.** A non-FK join inside a derived
   relation may happen to preserve all rows (e.g., a join whose
   condition matches every row), but the analysis cannot determine
   this. Any non-FK join causes all tracked properties to be lost.

2. **Set operations.** Operations that combine multiple relations
   (union, intersection, difference) are not analyzed, even when they
   might preserve the required properties.

3. **Complex grouping.** The analysis requires grouping columns to be
   direct column references from a single base table covering a unique
   key. It rejects grouping on expressions (even deterministic ones),
   grouping on columns from multiple tables, and grouping on columns
   from nested derived relations.

4. **Deduplication.** Even when deduplication would not change the
   result (e.g., applied to an already-unique relation), it is
   rejected because the analysis cannot verify this statically.

5. **Semantically vacuous filters.** A filter that happens to exclude
   no rows (e.g., a tautological predicate) breaks row preservation
   in the analysis, even though the data is unchanged.

6. **Transitive reasoning across non-FK joins.** If two FK chains are
   connected by a non-FK join that happens to preserve rows, the
   analysis cannot chain through the non-FK join.

This conservativeness is by design: false negatives (rejecting valid
joins) are acceptable; false positives (accepting invalid joins)
are not.
