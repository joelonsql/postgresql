# Key Join

## Introduction

This patch adds explicit syntax to express a key join.
Key joins per-se are nothing new, they are probably one of the most common ways
of joining two tables already.  What's new is just the explicit syntax,
which gives the user two additional benefits:

1. An error at compile-time if the key join is invalid.
2. The syntax visually immediately reveal which join operand that is
   referencing and referenced.

If the key join is accepted at compile-time, it is guaranteed to be valid,
and it is transformed during parse time to a traditional ON-equijoin.

This document will also establish a formal definition of what a key join is,
and a present a specification that is sufficient under such definition.

For the definition to be useful, we must first discuss what the intention
of a key join is, since only then can we say if the definition make sense
or not.

## Intention

The intention of a key join is to enrich a referencing table with a referenced
table, by following a declared referential constraint between the two, so that
the result contain all rows from the referencing table, without any row loss or
duplication, where all referencing non-null foreign keys find a unique
matching referenced row.

The intention of the key join syntax is to make it visually easy to immediately
identify which join operand that is the referencing and referenced table,
and the referencing and referenced columns.

It is also to relief the user of the cognitive load of having to inspect
if the key join syntax is valid or not, by automatically verifying at
compile-time if it can be proven to be correct under the definition,
and if not, reject it with an error.

The key join proof checker guarantees soundness, meaning that an invalid
key join must never be accepted, but a valid key join could possibly be
rejected.

The cases supported by the specification and the implementation, are thought
to cover a vast majority of the query shapes already written by users that
by definition already are key joins.

### Preservation Of Referencing Rows

Preservation is about row survival and multiplicity: which input rows
remain present, and how many result rows correspond to each one.  Every
referencing row that reaches the key join appears in the join result
exactly once.

The intent is asymmetric.  The referencing side is the side whose rows
the key join is meant to preserve.  The referenced side is consulted for
matching and must be unique for the referenced key, but its rows are not
the rows whose preservation the syntax is asserting.  A particular outer
join type may also preserve the referenced side, adding null-extended
rows to the result; that does not change the preservation claim for the
referencing side.

### Enrichment From The Referenced Side

Enrichment means that a referencing row with non-null key columns gets
exactly one matching row from the referenced side.  Existence comes from
the FK-backed value-containment proof, and singularity comes from
uniqueness of the referenced key.

### Identification Of The Referencing Side

The syntax should let a reader identify the referencing side from a
single key join, without consulting the schema, the data, or the rest of
the query.

Together with preservation, this identification lets the reader track
which rows are preserved through a chain of key joins.  In the common
case, the reader carries one fact in working memory: which
already-introduced table's rows are currently preserved.  At each key
join, either those rows remain preserved in the result, or the newly
introduced referencing table becomes the table whose rows are known to
survive.

## Definition

A **key join** is an equijoin between two tables, the
**referencing table** and the **referenced table**, where the join
predicate compares **referencing columns** in the referencing table with
**referenced columns** in the referenced table.  The two column lists
have the same arity.

The conditions under which an equijoin is a key join are stated in terms
of two multisets:

* The **referenced multiset** is the projection of the referenced table
  onto the referenced columns, restricted to rows where every referenced
  column is non-null.
* The **referencing multiset** is the projection of the referencing
  table onto the referencing columns.

Such an equijoin is a **key join** if and only if all of the following
conditions hold:

1. Every element of the referenced multiset has multiplicity exactly one.
2. Every distinct referencing value whose columns are all non-null is
   present in the referenced multiset.  Duplicate referencing values are
   allowed; the FK proof is value containment, not strict multiset
   subcontainment.
3. Either:
   * no element of the referencing multiset contains a null value, or
   * the referencing table is preserved via an outer join type.

### Exact Proof Identity

PostgreSQL referential constraints can be valid even when FK and PK
columns use different but comparable types or collations.  The first
key-join proof implementation deliberately accepts less than PostgreSQL
can enforce.  A key position is usable only when the FK column, PK
column, uniqueness fact, row-coverage fact, generated join equality, and
any matched filters all have exactly the same:

* type OID;
* typmod;
* collation OID;
* equality operator OID.

Collatable key positions must use deterministic collations, and the FK's
`conpfeqop`, `conppeqop`, and `conffeqop` entries must all be the same
operator.  The equality operator must accept exactly the key type on both
sides, return `boolean`, not return a set, and have a `STRICT`
`IMMUTABLE` implementation function.  Stored proofs record direct
dependencies on operator implementation functions consumed by the proof.

This restriction is a proof-system boundary, not a change to PostgreSQL
FK validity.  A broader FK schema can still be declared and enforced by
PostgreSQL, but it does not expose key-join proof facts unless it
satisfies this exact identity rule.  Users can often make filters
accepted by spelling the value type explicitly, for example
`tenant_id = 1::bigint` for a `bigint` key.

## Specification

### Syntax

The SQL spelling of a key join is a regular join with a `FOR KEY`
clause on the syntactic right operand:

```
left_operand
JOIN right_operand FOR KEY (right_operand_columns) -> left_alias (left_columns)

left_operand
JOIN right_operand FOR KEY (right_operand_columns) <- left_alias (left_columns)
```

The arrow direction assigns the key-join roles:

| Spelling | Right operand is | `left_alias` names |
| --- | --- | --- |
| `FOR KEY (...) -> left_alias (...)` | referencing side | referenced side |
| `FOR KEY (...) <- left_alias (...)` | referenced side | referencing side |

The column list inside `FOR KEY (...)` always names columns exposed by
the syntactic right operand of the join.  The alias after the arrow must
name a visible relation from the left operand.  This is deliberate:
ordinary join chains usually introduce a new table on the right, while
the left operand is often an already-built join result that needs an
alias to identify the participating surface.

Thus the arrow alone identifies the role of the newly introduced right
operand, which is the syntactic property needed for the visual intention:
the reader can tell which side is the referencing side from the key-join
arrow direction itself.

### Operand Surfaces And Proof Facts

After the formal definition, the specification talks about **operand
surfaces** rather than abstract tables.  A key-join surface is the
visible relation occurrence at a particular point in the query tree,
after projection, filtering, grouping, joins, and null extension have
been accounted for.  Relation, subquery, CTE, view, and join occurrences
are distinct surfaces.  Reusing the same base table, CTE, or view body
under another alias creates a different surface with its own facts.

Every surface can expose a small set of positive facts about its visible
columns:

* `notNull`: a surface column is known non-null.
* `unique`: a surface key is unique.
* `foreignKeys`: a surface key is backed by a usable FK constraint.
* `rowCoverage`: a surface key still covers the relevant underlying rows
  or key values, possibly under matched filters.

These facts are local to the surface.  They may remember catalog origins,
such as the table and columns that supplied a unique index or FK
constraint, but that origin is proof metadata.  Validation of a
`FOR KEY` join looks only at the facts exposed by the two operand
surfaces named by the join.

### Soundness Limits

Key join proves the formal multiset conditions from PostgreSQL catalog
contracts and computed query facts.  It does not re-scan heap contents,
audit trigger state, or verify that catalog constraints have always been
enforced in every past write path.

In particular, key join trusts PostgreSQL's current catalog-level FK
state: the FK is validated, catalog-enforced, nondeferrable, and otherwise
usable as a proof fact.  It does not inspect current or historical RI
trigger enablement, and it does not track rows loaded while
`session_replication_role` suppressed those triggers.  PostgreSQL lets
superusers bypass RI triggers for bulk-load workflows where the DBA
assumes responsibility for data consistency.  If such a bypass leaves
orphaned referencing rows, the data violates referential integrity
independently of key join; key join accepts the same catalog trade-off
instead of adding stricter enforcement.

Key join also trusts user-defined equality operators and operator
classes as catalog contracts, just as PostgreSQL does for indexes,
constraints, and plans that rely on them.  Key join checks the
catalog-level properties required for proof, such as exact key identity,
deterministic collation, and a `STRICT` `IMMUTABLE` boolean equality
function.  It does not prove that an operator function is mathematically
equality, that a btree operator class is internally coherent, or that a
replacement function body preserves prior semantics.  A broken
user-defined operator contract can therefore break key-join guarantees in
the same way it can break other PostgreSQL features that rely on that
contract.

Key join also trusts PostgreSQL function volatility for matched-filter
evidence.  Volatile matched-filter expressions are rejected, but
expressions marked `STABLE` are accepted according to PostgreSQL's
catalog contract.  This is not a complete soundness guarantee for
expressions that read backend-local mutable state.  In particular,
`current_setting(...)` is marked `STABLE` and is a natural spelling for
session tenant filters, but a statement can also execute volatile
`set_config(...)` calls that change the setting before another
`current_setting(...)` evaluation in the same statement.  A key join
whose proof depends on matched filters such as
`tenant_id = current_setting('tenant.id')` is therefore sound only under
the operational assumption that the relevant setting, or other non-MVCC
state read by the `STABLE` expression, is not changed during execution of
the statement.

### Validation Obligations

Validation of a `FOR KEY` join succeeds only when all required proof facts
are available on the two operand surfaces:

1. The referencing surface has a `foreignKeys` fact for exactly the
   selected referencing columns.
2. The referenced surface has a `unique` fact for exactly the selected
   referenced columns.  If the unique fact is catalog-backed, its origin
   must match the referenced side of the FK fact.  Query-level
   uniqueness, such as from `GROUP BY` or `DISTINCT`, can also prove
   uniqueness when the selected key identity matches the FK equality
   identity.
3. The referenced surface also has a matching `rowCoverage` fact for that
   same referenced key; this is the fact that supplies FK-backed
   containment for the derived referenced operand.
4. If referenced row coverage is conditional on matched filters, the
   referencing side must expose corresponding matched-filter evidence
   under the FK column mapping.
5. If the join type does not preserve nullable referencing rows, each
   selected referencing column has a `notNull` fact.

The executable join predicate is generated from the FK constraint's
equality operator.  Validation must not depend on a search-path lookup of
`=` and must not depend on parser coercions in the generated predicate.

These obligations prove the three formal multiset conditions.  The
referenced `unique` fact proves condition 1.  The FK fact plus referenced
`rowCoverage` proves condition 2 as FK-backed value containment.
Referencing-side `notNull` facts, or a join type that preserves nullable
referencing rows, prove condition 3.  Referencing-side uniqueness is not
required for basic validation; it is used only when deciding which facts
can be exported from the join result.

### Fact Sources And Query Shapes

Base relation facts come only from catalog objects whose current catalog
state says PostgreSQL enforces them.  This is a catalog-contract proof:
it is not an audit of RI trigger enablement or heap consistency after
privileged enforcement bypass.

* `notNull` facts come from enforced and validated `NOT NULL`
  constraints.
* `unique` facts come from valid, immediate, non-partial unique indexes
  over plain key columns with exact key identity.
* `rowCoverage` facts are available for usable base unique keys and
  usable referencing FK keys, justified by the corresponding unfiltered
  base scan.
* `foreignKeys` facts come from catalog-enforced, validated,
  non-deferrable, non-PERIOD FK constraints whose FK and PK columns have
  exact key identity.  Current or historical RI trigger enablement is
  intentionally outside this fact.  PERIOD/temporal FKs are not equijoin
  proofs and are not usable key-join FK facts.

Unsupported relation kinds and scan shapes do not expose the facts whose
meaning they cannot preserve.  `TABLESAMPLE` destroys row coverage, while
leaving not-null, uniqueness, and FK-backed value facts available for the
rows that remain.  Base relations with row-level security enabled do not
expose any key-join facts.  Matching RLS policies is not supported;
policy filtering is role- and environment-dependent and is applied
outside the base catalog proofs used by key joins.

Facts project only through direct column references.  A target entry that
is an expression, function call, set-returning expression, transformed
key value, system column, or otherwise not a direct surface attribute
drops the affected fact.  Relabels are preserved only when they keep the
same type, typmod, and collation.  For multi-column keys, each selected
column must identify one distinct position in the same logical key under
the exact key identity rule.

A referenced operand of a key join needs both uniqueness and row
coverage.  A derived relation may still be unique on a key after
filtering, but it may have removed rows that a referencing key value
needs to find.  The FK proves containment against that derived referenced
operand only when the derived operand is row-covered, or when any
referenced-side filter is matched by corresponding referencing-side
filter evidence.

Matched-filter capture is a positive allowlist.  Top-level `AND` is only
a separator between independent conjuncts.  Each retained conjunct must
be canonical `key = value`, with the key on the left, the value on the
right, the exact key equality operator, the key collation as operator
input collation, and an exact value type, typmod, and collation match.
The commuted `value = key` form is rejected instead of normalized.
Volatile expressions, sublinks, transformed key values, collation or
coercion changes, Boolean wrappers, `<>`, `IN`, `NOT IN`,
`IS [NOT] DISTINCT FROM`, `IS [NOT] NULL`, and predicates on columns
outside the covered key are not safe matched-filter conjuncts.  Accepted
`STABLE` value expressions remain subject to the known soundness
limitation for backend-local mutable state described above.

For row coverage, every row-removing conjunct must be represented for the
covered key; otherwise row coverage is lost.  For FK facts,
unrepresentable conjuncts supply no matching evidence, but they do not
invalidate the FK fact for rows that remain.

`GROUP BY`, `DISTINCT`, and `DISTINCT ON` can create `unique` facts when
their key columns are direct projected columns.  They never create row
coverage.  They can only preserve an existing row-coverage fact when the
row-collapsing columns cover that same fact under the exact key identity
used by the key proof.  The referenced multiset excludes rows where any
referenced key column is null, so collapsing null-containing referenced
rows cannot remove a value required by an all-non-null referencing key.
Extra grouped columns are harmless when the covered subset is already
unique.

`HAVING`, `LIMIT`, `OFFSET`, `FOR UPDATE SKIP LOCKED`, grouping sets,
`ROLLUP`, `CUBE`, set operations, data-modifying CTEs, recursive CTEs,
`LATERAL` subqueries, target-list SRFs, and volatile evaluated
expressions are conservative barriers for the facts they can invalidate.
`LATERAL` subqueries expose no key-join facts because their facts would
need per-outer-row cardinality and containment proof.

### Join-Local FILTER

A `FOR KEY` join may carry a join-local `FILTER (WHERE ...)` clause.
This filter is applied to the key-join result with join-qual semantics;
it is not part of the FK equality predicate and is not evidence about
the input operands used to prove the key join.

Key-join validity is proven from the operand surfaces as they stand
before the join-local filter is applied.  Therefore, a join-local filter
cannot satisfy matched-filter row-coverage evidence for a filtered
referenced operand, and cannot satisfy not-null evidence for nullable
referencing columns.  Required proof evidence must come from the input
operand surfaces themselves, such as a view, subquery, or CTE supplying
filtered rows to the key join.

For an inner key join, `FILTER` is equivalent to filtering the already
proven key-join result.  It may therefore remove rows from the final
query result, including referencing rows; that is ordinary filter
semantics and not a failed key-join proof.  For an outer key join, rows
preserved by the outer join type remain preserved, while non-matching or
filtered-out rows on the non-preserved side are null-extended according
to the join type.

### Fact Propagation

Ordinary joins export no surface facts, except that an inner cross join
with a guaranteed single-row companion can pass through the other side's
facts.  The companion must be guaranteed to produce exactly one row and
must be proof-safe: evaluated volatile expressions do not qualify because
they can change state read by matched filters in later operands.
Aggregate queries with `HAVING`, row-limiting uncertainty, volatile
target expressions, or other zero-row possibilities do not qualify.

An accepted key join can export facts to later joins, but each fact kind
has its own preservation condition:

* Referencing-side facts are projected to the join output, subject to
  normal outer-join null extension.
* Referenced-side not-null facts are projected only when the join type
  does not null-extend the referenced side.
* Referenced-side FK facts can survive null-extension as nullable
  value-containment facts for non-null projected FK values.  They do not
  imply not-nullness or row coverage.
* Referenced-side uniqueness is projected only when the accepted join
  cannot fan out referenced rows because the selected referencing FK
  columns are unique enough.
* Row coverage is propagated only where the accepted join proof preserves
  the rows that make the coverage fact true.
* Filter evidence can be propagated through direct projection and
  accepted FK equality mappings, but it is evidence about present rows,
  not row-coverage proof by itself.

Filter evidence moves only along proven key-position relationships.  A
filter on one key position can be copied to the corresponding equal key
position in the join result.  It cannot be copied to another key position
merely because both positions belong to the same relation or appear in the
same output row.

### Stored Objects And DDL

Stored views, materialized views, rules, RLS policies, and new-style SQL
function bodies persist accepted key-join proof annotations, not
advertised surface facts.  When a later key join mentions a view operand,
the view's facts are recomputed for proof analysis only; this does not
rewrite or update the stored view.

When DDL changes an object that a stored key-join proof depends on,
dependent stored key-join objects are revalidated against current catalog
state after the DDL change has been made visible.  This includes producer
view replacement, function replacement or alteration for functions used
in matched-filter proofs, constraint usability changes, inheritance
changes, and row-level-security changes.  The DDL is rejected if a stored
proof no longer holds, if replay would change executable semantics such
as selected equality operators or join quals, or if replay would require
a new proof dependency that is not already recorded in `pg_depend`.
Dependency shrinkage is accepted without rewriting the stored object;
stale extra dependencies may remain until the owning object is explicitly
recreated.  Dropping a proof source such as a constraint, unique index,
view, or function is blocked when a stored key-join proof still depends
on it, including when that dependency is stale but conservative.

Constraint changes that can make a stored proof unusable, such as an FK
becoming `NOT ENFORCED` or `DEFERRABLE`, or a consumed NOT NULL
constraint becoming `NO INHERIT`, also revalidate dependent stored
key-join objects and reject the DDL if proof replay fails or would need
new dependencies.

Adding an inheritance child to an ordinary relation, via
`CREATE TABLE ... INHERITS` or `ALTER TABLE ... INHERIT`, also
revalidates dependent stored key-join objects.  A non-partitioned parent
that gains a subclass no longer exposes base key-join facts under
`inh = true`, because the scan can include child rows outside the
parent's single-table catalog proof surface.  If a stored proof depended
on that surface, whether as referencing or referenced side, the
inheritance change is rejected.

Attaching a partition to a partitioned-table parent also revalidates
dependent stored key-join objects, but normal inherited scans of the
partitioned parent remain valid because PostgreSQL enforces unique
constraints across partitions.  `ONLY` scans of partitioned parents do
not expose key-join facts.

`ALTER TABLE ... ENABLE ROW LEVEL SECURITY` likewise revalidates
dependent stored key-join objects.  Base relations with row-level
security enabled expose no key-join facts, so a stored proof against an
RLS-free surface becomes unprovable as soon as RLS is enabled and the
change is rejected.  Disabling RLS does not invalidate any proof.

Materialized views participate in the same revalidation as regular
views.  User-defined rules, RLS policy `USING` and `WITH CHECK`
expressions, and new-style SQL function bodies also participate.
Old-style quoted SQL functions do not store analyzed key-join proof
bodies; their SQL text is parsed later and is not revalidated as a
stored key-join body.

Functions and operators used in matched-filter conjuncts are proof
dependencies, so `ALTER FUNCTION` and `CREATE OR REPLACE FUNCTION`
replay dependent stored proofs.  The replay checks the stored filter
expression against current catalog volatility and subplan rules, but it
does not prove that a replacement function body is extensionally
equivalent.  Equality operator implementation functions consumed by FK
and uniqueness proofs are also direct proof dependencies; replay requires
their catalog-level strict, immutable, non-set-returning boolean equality
contract to remain usable.

### Conservative Contract

When a fact cannot be proved through a construct, the fact is dropped.
Key join does not drill through arbitrary query text, infer arbitrary
equivalences, or assume two different expressions are extensionally
equal.

The core contract is:

* Key-join validation is local to the two operand surfaces.
* Reusing a table, CTE, or view body under multiple aliases creates
  independent key-join fact surfaces.
* Matched-filter evidence on FK facts describes rows present on the
  current surface; it is not row-coverage proof by itself.
* Stored key-join proofs are trustworthy because their dependencies are
  recorded and dependent stored objects are revalidated during DDL that
  changes consumed proof dependencies.
* Rejecting a valid but unproven key join is acceptable.  Accepting a key
  join whose proof depends on an unstated assumption is not.

## Implementation

The implementation is a demand-driven parser-analysis proof pass.  A
`FOR KEY` join asks its operand RTEs to expose key-join surface facts;
the computation remains bottom-up from those requested surfaces.  Facts
are projected through supported derived relations, extra facts are
created for supported query shapes, and accepted joins store enough
metadata to replay validation for stored objects.

### Pipeline

Base relation RTEs receive facts from catalog inspection only when a
`FOR KEY` proof asks for them.  Non-lateral subquery and CTE RTEs
receive facts by analyzing their query surfaces and projecting direct
Vars to the outer surface.  View RTEs are handled the same way, but from
a private copy of the stored view query made during key-join parse
analysis.  Join RTEs receive facts only when requested, from accepted key
joins or the single-row companion cross-join case.

Stored dependency scans ignore transient RTE facts.  Stored objects keep
their accepted `KeyJoinNode` proof records and dependencies.  When a
view's facts are needed later, the parser revalidates a copied stored
query, lazily computes the requested output facts, and projects that
copy's output facts onto the view operand.

### Internal Representation

`RangeTblEntry.keyJoinFactsComputed` and
`RangeTblEntry.keyJoinFacts` form a demand-driven cache.  A false
computed bit means no computation has been attempted.  A true computed
bit means computation ran, and `keyJoinFacts` may still be NULL when the
surface exposes no usable facts.  The cache is parser-analysis scratch
metadata: node serialization omits it, stored dependency scans ignore
it, and stored key-join proofs carry only the consumed `KeyJoinNode`
proof dependencies.  It is ignored for query equality and query jumbling
because it is proof metadata rather than SQL semantics or query identity.

`KeyJoinSurfaceFacts` contains one `facts` list of tagged
`KeyJoinFact` nodes.  Each fact's `kind` identifies which proof
meaning its fields carry:

* `KJF_NOT_NULL`: `attnum` is the surface column proven non-null.
* `KJF_UNIQUE`: `keyPositions`, `relid`, and `baseAttnums` describe a
  surface key proven unique.
* `KJF_FOREIGN_KEY`: `keyPositions`, `relid`, `baseAttnums`,
  `referencedRelid`, `referencedAttnums`, and `constraint` describe a
  surface key backed by a usable FK.
* `KJF_ROW_COVERAGE`: `keyPositions`, `relid`, and `baseAttnums`
  describe covered rows or key values.

Common fact fields:

* `keyPositions` is an ordered list of `KeyJoinKeyPosition` nodes.  Each
  position contains an alias set of direct surface columns for one
  logical key column plus exact key identity: type OID, typmod,
  collation OID, and equality operator OID.
* `relid` and `baseAttnums` identify the catalog relation and base
  columns that justify catalog-backed facts.  Query-shape uniqueness can
  use `InvalidOid` with local proof positions.
* `dependencies` is a list of `KeyJoinProofDependency` nodes recording
  catalog provenance.  These dependencies become persistent only when a
  stored `KeyJoinNode` consumes the fact.
* `filterConjuncts`, on row-coverage and FK facts, is the normalized
  matched-filter evidence for the key positions.

`KeyJoinNode` replaces the raw `KeyJoinClause` on an accepted
`JoinExpr`.  It stores the direction, selected proof-surface columns, FK
constraint, not-null dependencies, and proof dependencies.  It also
stores the parser-resolved alias and columns named after the arrow, so
rule deparsing can print the original key-join participant even when the
proof surface is an unnamed join RTE.  It is a stored proof and
dependency record; it is not replayed during ordinary `SELECT` execution.

`KeyJoinMatch` is only an internal parser-analysis helper.  It holds the
matched FK constraint OID, equality operators extracted from selected key
identities, not-null dependencies, and accumulated proof dependencies
while validating one key join.

Key-join fact nodes must support copy, read, out, walk, mutate, and
pgindent typedef generation like other parse-analysis nodes.  Storage
cleanup prevents surface fact sets from becoming a stored object's
persistent contract.

### Catalog Fact Extraction

`ensureKeyJoinSurfaceFacts` triggers base fact extraction when the
requested surface is a relation.  It reads validated and enforced
`NOT NULL` constraints, usable unique indexes, and usable FK
constraints.  Unique indexes produce both uniqueness and row coverage for
the unique key.  FK constraints produce both an FK fact and row coverage
for the referencing key.  Unique indexes and FKs are usable only when
every key position has exact proof identity, including deterministic
collation and a strict immutable exact equality operator.

The extraction step records constraint or index provenance in the facts.
Bare unique indexes depend on the index relation; constrained indexes
depend on the constraint.  Base relation extraction suppresses facts for
relation kinds and scan shapes whose surface contract cannot preserve
the fact, including row coverage under `TABLESAMPLE` and all facts under
row-level security.

### Projection And Attribute Mapping

Projection builds a temporary list-valued input-to-output attribute map
for one source surface at a time.  A direct source column can map to more
than one output column, which is how duplicate direct key projections are
represented without inventing multiple keys.

Not-null facts copy to every direct output alias.  Key facts map each key
position to all direct output aliases for that logical key column.  If
any position has no direct output alias, or if a cast/relabel/collation
change alters type, typmod, or collation, the key-shaped fact is
dropped.

Join RTE projection uses PostgreSQL's join alias metadata to build the
same kind of map.  This is an internal projection detail only; ordinary
join results still export no facts unless the specification's join rules
allow it.

### Filter Canonicalization

Filter capture starts by splitting a qual with `make_ands_implicit`.
Each resulting item is accepted only if it has an admitted predicate
shape and can be canonicalized against the current key positions.

Canonicalization replaces direct key-column Vars with positional Params.
That makes comparison structural after applying an FK key-position map.
The code accepts only canonical `key = value` filters with exact key
identity and rejects volatile conjuncts, sublinks, transformed key
values, unsupported Boolean shapes, commuted equality, and key Vars
hidden under functions, coercions, or collations.  For row-coverage
facts, a non-canonical filter drops the coverage fact; for FK facts, it
simply means no evidence is retained from that conjunct.

Accepted key joins add another source of filter evidence: generated FK
equality proves that a filter on one selected key position also holds for
the corresponding selected position on the other side of the join result.
The implementation materializes that evidence onto projected FK facts,
and onto row-coverage facts only when those coverage facts are otherwise
preserved.

### Join Validation And Output Facts

`transformAndValidateKeyJoin` resolves the syntactic column lists
against visible aliases, determines which side is referencing, and maps
the selected columns to operand-surface attnums.  `find_key_join_match`
then matches FK, uniqueness, row-coverage, filter, and not-null facts.
Join-local `FILTER` is deliberately not part of this proof; it is
transformed and merged into the executable join quals only after the raw
key-join clause has been accepted.

On success, parse analysis replaces the raw key-join clause with a
`KeyJoinNode`, builds the executable equality quals from the FK
equality operators, and records proof dependencies.  If a later proof
asks for facts from that join result, the join RTE lazily computes the
facts it may export.  Exported facts describe the rows after the
join-local filter has been applied.

Referenced-side uniqueness is exported only when the join cannot fan out
referenced rows.  Nullable-side outer joins drop not-null facts, while
directly projected FK facts can survive as nullable value-containment
evidence.  Row coverage is exported only through row-preserving shapes.
Any missing piece causes validation to fail conservatively.

### Stored Objects, Revalidation, And Deparse

Key-join surface facts are node-tree scratch data, so the node support
code copies, serializes, reads, walks, and mutates them.  Stored node
trees do not keep surface fact sets as dependency sources.  The
dependency walker ignores any remaining `KeyJoinSurfaceFacts` provenance,
but turns `KeyJoinProofDependency` nodes in `KeyJoinNode` proof lists
back into `ObjectAddress` entries when storing a rule, policy, or SQL
function body.

DDL that changes a proof dependency revalidates dependent stored
key-join objects after the change is visible in the catalogs.  Replay
uses private copies of stored rule actions, policy expressions, and SQL
function bodies.  If a dependent stored proof can still be replayed
without adding dependencies or changing stored semantics, the DDL is
accepted without rewriting the dependent object.  Any `keyJoinFacts`
rebuilt during replay remain scratch state because node serialization
omits them and dependency recording ignores them.  If the proof cannot
be replayed, would change executable quals or equality operators, or
would require a new unrecorded proof dependency, the DDL is rejected.

FK `ALTER CONSTRAINT` changes that affect proof usability use the same
replay check for stored objects that depend on the FK proof.  NOT NULL
`ALTER CONSTRAINT ... INHERIT/NO INHERIT` changes likewise replay stored
proofs that consumed the NOT NULL fact.  Inheritance and RLS changes use
the same machinery for stored proofs whose fact sources would no longer
be exposed after the DDL.

Materialized views store key-join annotations exactly like regular
views.  User-defined rule actions and qualifications, RLS policy
expressions, and new-style SQL function bodies are walked and replayed
without rewriting their stored definitions.  Old-style quoted SQL
functions are excluded because their SQL text is parsed later and is not
stored as an analyzed key-join body.

Dependency-shrinking replay is allowed to leave old `pg_depend` entries
behind.  Those stale dependencies are conservative: they can block later
DDL that tries to drop a no-longer-needed proof source, but they never
allow a required proof source to be dropped unnoticed.  Explicitly
recreating the owning view, policy, rule, or SQL function body stores
fresh proof metadata and removes stale dependency edges.

Rule deparsing prints accepted key joins in source form:

```
JOIN child FOR KEY (parent_id) -> parent (id)
```

or the corresponding `<-` form.  Generated equality quals are not printed
as an `ON` clause, because they implement the key join.  A user-written
`FILTER (WHERE ...)` attached to the join is deparsed separately.

### Cache Lookup Checks In Assert Builds

Some stored-object revalidation lookups are expected to find catalog
tuples because dependency scanning or the DDL caller identified the
object.  A missing tuple indicates a catalog consistency bug, stale
dependency metadata, or an unexpected race or locking hole, not a normal
user error.

For those checks, assert-enabled builds use `Assert(...)` so developer
testing fails as a PostgreSQL bug.  Non-assert builds keep the defensive
`elog(ERROR, "cache lookup failed ...")` path so an unexpected catalog
state is still reported as a controlled backend error rather than
continuing toward an invalid tuple dereference or crashing through an
assertion failure.

This split also lets the local LLVM MC/DC report reach 100% without
tests that manufacture corrupt catalog dependency state.  Plain
`Assert(...)` remains appropriate for purely internal invariants, such as
exhausted switch cases or locally-proven recursion preconditions, where a
production runtime branch would only duplicate unreachable control flow.

### Future Work

A future version could replace validation-only replay with metadata-only
dependency replacement during the DDL command that changed a proof
source.  Revalidation would replay dependent stored key-join objects
against current catalogs and classify the result as unchanged,
metadata-only, or unsafe.  Unchanged replay would need no catalog update.
Metadata-only replay would write the replayed tree back to the owning
catalog row and rebuild outgoing `pg_depend` edges.  Unsafe replay would
still reject the DDL.

Only `KeyJoinNode.constraint`, `KeyJoinNode.notNullConstraints`, and
`KeyJoinNode.proofDependencies` should be refreshable metadata.  The
classifier would compare executable trees after normalizing those fields
on a temporary copy, leaving the replayed tree intact for persistence.
Proof-source replacement, such as a producer view switching from one
equivalently constrained table to another, would be accepted only when
executable quals, equality operators, filters, target lists, and
non-proof tree structure remain unchanged.

The refresh should cover the same stored-object containers that already
participate in key-join replay: rewrite rules for views and materialized
views, user-defined rule actions and qualifications, RLS policy `USING`
and `WITH CHECK` expressions, and new-style SQL function bodies.
Old-style quoted SQL functions would remain excluded.  After refreshing
one object, revalidation should make that catalog change visible before
recursing to downstream objects, so descendants see the refreshed proof
metadata and dependencies.

### Alternative Designs Considered

One alternative was to cache surface facts in stored view rules.  That is
fast and clean at parse time, but invasive for ordinary views and awkward
for DDL invalidation.  It changes view metadata even when no key join is
used.

Another alternative was to move proof after rewrite.  That is an
architectural mismatch: parse analysis normally resolves and validates
constructs like this, and a key join must become executable equality
quals before planning.

The chosen design computes view facts on demand during parse analysis.
This is imperfect because it inspects stored view queries before normal
rewrite expansion, but it is well scoped.  Only queries that use key join
pay the cost, and only stored key joins create persistent dependencies.

### Failure Mode

The implementation is intentionally conservative.  When proof
information is lost or cannot be represented by the supported fact
machinery, the corresponding fact is discarded.  If validation then lacks
a required fact, the `FOR KEY` join is rejected.
