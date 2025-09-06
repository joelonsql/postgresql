# Implement `json_schema_generate()` in PostgreSQL core

> **Goal**
> Add a built‑in SQL-callable function `json_schema_generate()` that inspects a function’s signature and (when available) its SQL‑body parse tree to emit a \[JSON Schema draft‑2020‑12] document describing the structure of the value it returns.
> – Deep introspection for SQL‑body functions (`BEGIN ATOMIC … END` or `RETURN (…)`).
> – Shallow introspection for all other functions (based on declared return type only).

This document is written for **Claude Code** (or any code assistant) operating in a local clone of the official PostgreSQL repo. It contains exact file paths, skeletons, and test scaffolding.

---

## Table of contents

1. [Background & references](#background--references)
2. [User‑facing specification](#userfacing-specification)
3. [Architecture & algorithms](#architecture--algorithms)
4. [Patch plan (files to add/change)](#patch-plan-files-to-addchange)
5. [C implementation skeletons](#c-implementation-skeletons)
6. [Catalog entry (`pg_proc.dat`)](#catalog-entry-pg_procdat)
7. [Build system updates (Meson & Make)](#build-system-updates-meson--make)
8. [Regression tests](#regression-tests)
9. [Developer notes, limits, and follow‑ups](#developer-notes-limits-and-followups)

---

## Background & references

* **SQL‑body functions** (SQL‑standard `RETURN` and `BEGIN ATOMIC`) are parsed at *definition time* and stored as parse trees, in `pg_proc.prosqlbody` (`pg_node_tree`). At run time no further parsing is required. ([PostgreSQL][1])
* The helper **`pg_get_function_sqlbody(oid)`** deparses `prosqlbody`; its implementation shows how to read and traverse `prosqlbody` using `stringToNode()`, and that it may contain either a single `Query*` or a `List` of `Query*` (for `BEGIN ATOMIC`). It also calls `AcquireRewriteLocks(query, ...)` before deparsing. See `src/backend/utils/adt/ruleutils.c`. ([doxygen.postgresql.org][2])
* Catalog docs for **`pg_proc`** and initial‑data/OID allocation (**`unused_oids`**, 1–9999 reserved) explain how to add built‑in functions. Random pick 8000–9999 during development to avoid collisions. ([PostgreSQL][3], [Database Administrators Stack Exchange][4])
* SQL functions return the **result of the last statement**; if returning a set, they return a set of rows (or values). ([PostgreSQL][5])
* JSON construction/query nodes and function calls appear as `FuncExpr`, `Aggref`, operators (`OpExpr`), and SQL/JSON’s `JsonExpr`. (See parser/optimizer/executor references). ([doxygen.postgresql.org][6])
* Tree walking utilities: `stringToNode()` + `expression_tree_walker()`/`query_tree_walker()` in `nodeFuncs.*`. ([doxygen.postgresql.org][7])
* JSON functions and operators reference (official docs) for recognizing `jsonb_build_object`, `jsonb_agg`, `jsonb_build_array`, etc. ([PostgreSQL][8])

---

## User‑facing specification

**Name & signatures**

* `json_schema_generate(oid) RETURNS jsonb`
* `json_schema_generate(regprocedure) RETURNS jsonb`  *(overload for exact overload resolution)*
* (Optional convenience) `json_schema_generate(regproc) RETURNS jsonb`

**Behavior**

* Works for **any function** (in `pg_proc`):

  * If `prosqlbody` **present** → **deep** introspection: traverse the stored parse tree of the **last statement**, follow JSON constructors (`json[b]_build_object/array`, `json[b]_agg`, `json[b]_object_agg`, SQL/JSON `JsonExpr`, `jsonb_set`, `||`, etc.) and infer a **precise JSON Schema** when provable.
  * If `prosqlbody` **NULL** (PL/pgSQL, C, etc.) → **shallow** introspection: emit schema from the declared **return type** only (no body analysis).
* If the function returns a **set** (`proretset`):

  * Default: schema is `{ "type": "array", "items": <schema-of-one-element> }` and attach `"x-pg-returns": "setof"`.
* Always include metadata:

  ```json
  {
    "$schema": "https://json-schema.org/draft/2020-12/schema",
    "x-pg-source": "schemaname.proname(args)",
    "x-pg-introspection": "sql-body" | "signature-only",
    "x-pg-depth": "deep" | "shallow",
    "x-pg-version": <server_version_num>
  }
  ```
* For non‑SQL‑body functions returning **`json`/`jsonb`**, return **unstructured JSON** schema:

  ```json
  { "type": ["object","array","string","number","boolean","null"] }
  ```
* For **composite** returns (including `RETURNS TABLE (...)`), map to a JSON object with one property per attribute; nullability derives from `attnotnull` etc.
* For **domain** returns, include base‑type schema and refine with discovered constraints (best‑effort).

**Errors**

* Must not throw error when the function is not SQL‑body; it should fallback to signature‑only mode.
* Raise errors only on truly unexpected conditions (e.g., missing `pg_proc` row, OID not a function/procedure).

---

## Architecture & algorithms

### 1) Entry: resolve target `pg_proc`

* Accept `OID` (or `regproc[*]`) and fetch the `pg_proc` tuple.
* Gather: `pronamespace`, `proname`, `prorettype`, `proretset`, argument types and names.
* Check `prosqlbody` presence:

  * If **NULL** → go to **Shallow path**.
  * Else → parse `TextDatumGetCString(prosqlbody)` via `stringToNode()`. If a `List` → it encodes a `BEGIN ATOMIC` block; take the **last** `Query*`. Otherwise a single `Query*`. (See `pg_get_function_sqlbody` in `ruleutils.c`.) ([doxygen.postgresql.org][2])

### 2) Determine the **returned expression(s)**

* For the last `Query*`:

  * Must be `SELECT` **or** DML with `RETURNING` (per SQL function rules). ([PostgreSQL][5])
  * If returning **scalar/composite**: examine `targetList` (`List<TargetEntry>`).
  * For **scalar** returns → infer schema from the expression of the first target entry (Postgres treats “first row of the last query” for scalar SQL functions; we only need the type/shape). ([PostgreSQL][5])
  * For **table/composite** → infer a property per `TargetEntry` (or per attributes when the return type is a named composite).

### 3) **Schema inference** core

* **Type map** (base types):

  * `bool` → `"boolean"`
  * integers (`int2/int4/int8`) → `"number"` (optionally `"multipleOf": 1`)
  * `numeric/float4/float8` → `"number"`
  * `text/varchar/char/uuid` → `"string"`
  * `date` → `"string", "format": "date"`
  * `timestamp[tz]` → `"string", "format": "date-time"`
  * `time[tz]` → `"string", "format": "time"`
  * `bytea` → `"string", "contentEncoding": "base64"`
  * `json/jsonb` → handled specially (see below)
  * **domain** → base + refine if simple constraints are detectable
  * **composite** → `"object"` with properties per attribute
* **Nullability**:

  * If proof of NOT NULL (column `attnotnull`, `COALESCE`, constant non‑null, etc.) → single type.
  * Otherwise → union with `"null"` (e.g., `"type":["string","null"]`).
* **JSON constructors (deep)**:

  * `jsonb_build_object(k1, v1, ...)`: if keys are constants, emit `"properties"` with per‑value schemas and `"additionalProperties": false`; otherwise use `"patternProperties": {".*": <valueSchemaUnion>}`.
  * `jsonb_build_array(v1, v2, ...)`: emit `"type":"array", "prefixItems":[...], "items": false`.
  * `jsonb_agg(x)`: `"type":"array", "items": Schema(x)`.
  * `jsonb_object_agg(k,v)`: `"type":"object", "patternProperties": {".*": Schema(v)} , "additionalProperties": false`.
  * SQL/JSON `JsonExpr` (`JSON_QUERY`, `JSON_VALUE`, etc.): inspect `returning` to obtain the element type/shape; where not fully determinable, conservatively union possible shapes. ([doxygen.postgresql.org][6])
  * `jsonb_set(target, path, value, ...)` and `||` (concat): represent as `allOf` of merged shapes; if `path` non‑constant, fall back to `"additionalProperties": true`.
* **Control flow**:

  * `CASE` → `anyOf` of branch schemas.
  * `COALESCE` → first non‑null branch schema; if heterogeneous, `anyOf`.
  * Casts → destination type; you may attach `"x-pg-cast-from"` if it narrows.
* **Aggregates**:

  * Recognize `Aggref` → handle `jsonb_agg/jsonb_object_agg`; otherwise fall back to result type.
* **Operators**:

  * Use result type info; for JSON operators like `||`, apply merge rules above.

### 4) Output

* Emit a JSON Schema `jsonb` via `JsonbParseState` helpers, with top‑level `$schema`, `title`, and the `"x-pg-*"` annotations above.

---

## Patch plan (files to add/change)

```
src/backend/utils/adt/jsonschema.c        # NEW: core implementation
src/include/utils/jsonschema.h            # NEW: (small) public header for our symbol(s)
src/include/utils/builtins.h              # + prototype(s)
src/include/catalog/pg_proc.dat           # + function catalog entries
src/backend/utils/adt/meson.build         # + add jsonschema.c
src/backend/utils/adt/Makefile            # + add jsonschema.o
src/test/regress/sql/jsonschema.sql       # NEW tests
src/test/regress/expected/jsonschema.out  # NEW expected
doc/src/sgml/func.sgml                    # + reference entry (concise blurb)
```

> **Why `utils/adt/`?** This is where most SQL‑callable built‑ins live, including JSON helpers; `ruleutils.c` shows a pattern to consume `prosqlbody` safely. ([doxygen.postgresql.org][2])

---

## C implementation skeletons

> The following skeletons are designed to compile as-is once you wire up includes and catalogs. Flesh out the TODOs incrementally.

### 1) `src/backend/utils/adt/jsonschema.c`

```c
/*
 * jsonschema.c
 *   Generate JSON Schema for a function's return value.
 *
 * See jsonschema.h for the exported symbol(s).
 */

#include "postgres.h"
#include "fmgr.h"

#include "access/htup_details.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type.h"
#include "nodes/nodeFuncs.h"
#include "nodes/parsenodes.h"
#include "nodes/primnodes.h"
#include "parser/parse_type.h"
#include "rewrite/rewriteHandler.h"   /* AcquireRewriteLocks */
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/jsonb.h"
#include "utils/typcache.h"

PG_FUNCTION_INFO_V1(json_schema_generate_oid);
PG_FUNCTION_INFO_V1(json_schema_generate_regprocedure);
PG_FUNCTION_INFO_V1(json_schema_generate_regproc);

/* ----- minimal helpers' prototypes ----- */

static Jsonb *schema_for_type(Oid typid, int32 typmod, bool notnull);
static Jsonb *schema_for_composite(Oid typid);
static Jsonb *schema_for_json_any(void);
static Jsonb *wrap_array_items(Jsonb *item_schema, bool is_setof);
static Jsonb *schema_from_query(Query *query, Oid prorettype, bool proretset);

static Jsonb *schema_from_expr(Node *expr);
static Jsonb *schema_merge_allof(Jsonb *a, Jsonb *b);
static Jsonb *schema_union_anyof(List *schemas);

static void  push_meta(JsonbParseState *ps, Oid funcid, bool deep);

/* ----- utilities to build jsonb conveniently ----- */
/* (You can inline small utilities here or add a tiny builder.) */

/* Entry points */

Datum
json_schema_generate_oid(PG_FUNCTION_ARGS)
{
    Oid funcid = PG_GETARG_OID(0);
    /* delegate to common worker */
    PG_RETURN_JSONB_P(json_schema_generate_worker(funcid));
}

Datum
json_schema_generate_regprocedure(PG_FUNCTION_ARGS)
{
    Oid funcid = PG_GETARG_OID(0);
    PG_RETURN_JSONB_P(json_schema_generate_worker(funcid));
}

Datum
json_schema_generate_regproc(PG_FUNCTION_ARGS)
{
    Oid funcid = PG_GETARG_OID(0);
    PG_RETURN_JSONB_P(json_schema_generate_worker(funcid));
}

/* Common worker */

Jsonb *
json_schema_generate_worker(Oid funcid)
{
    HeapTuple   proctup;
    Form_pg_proc proc;
    Datum       d;
    bool        isnull;
    Jsonb      *result = NULL;

    proctup = SearchSysCache1(PROCOID, ObjectIdGetDatum(funcid));
    if (!HeapTupleIsValid(proctup))
        ereport(ERROR, (errmsg("function with OID %u does not exist", funcid)));

    proc = (Form_pg_proc) GETSTRUCT(proctup);

    /* Shallow: when no SQL-body is stored */
    d = SysCacheGetAttr(PROCOID, proctup, Anum_pg_proc_prosqlbody, &isnull);
    if (isnull)
    {
        /* Signature-only */
        Jsonb *root;
        JsonbParseState *ps = NULL;

        pushJsonbValue(&ps, WJB_BEGIN_OBJECT, NULL);
        /* $schema, title, x-pg-* */
        /* ... fill top-level, then "type" from declared return type ... */

        if (proc->prorettype == JSONB_OID || proc->prorettype == JSONOID)
            root = schema_for_json_any();
        else if (get_typtype(proc->prorettype) == TYPTYPE_COMPOSITE)
            root = schema_for_composite(proc->prorettype);
        else
            root = schema_for_type(proc->prorettype, -1, !proc->proretset /* unknown */);

        if (proc->proretset)
            root = wrap_array_items(root, true);

        /* emit root under, say, "type"/properties directly */
        /* For simplicity, splice root object members into top-level here */

        /* TODO: merge 'root' into current ps */

        push_meta(ps, funcid, false);
        pushJsonbValue(&ps, WJB_END_OBJECT, NULL);
        result = JsonbValueToJsonb(pushJsonbValue(&ps, WJB_END_OBJECT, NULL));
        ReleaseSysCache(proctup);
        return result;
    }

    /* Deep: SQL-body present — parse it like pg_get_function_sqlbody does */
    {
        Node   *n;
        Query  *lastq = NULL;

        n = stringToNode(TextDatumGetCString(d));  /* pg_node_tree -> Node* */

        if (IsA(n, List))
        {
            /* BEGIN ATOMIC case: the Node is a List containing a List of Query* */
            List *stmts = linitial_node(List, (List *) n);
            if (stmts != NIL)
                lastq = llast_node(Query, stmts);
        }
        else
        {
            lastq = castNode(Query, n);
        }

        if (lastq == NULL)
        {
            ReleaseSysCache(proctup);
            ereport(ERROR, (errmsg("malformed SQL-body parse tree for function %u", funcid)));
        }

        AcquireRewriteLocks(lastq, false, false);  /* match ruleutils.c */
        result = schema_from_query(lastq, proc->prorettype, proc->proretset);

        /* TODO: attach top-level $schema/title/x-pg-* metadata here */

        ReleaseSysCache(proctup);
        return result;
    }
}

/* ---------------- Schema builders ---------------- */

static Jsonb *
schema_for_json_any(void)
{
    /* { "type": ["object","array","string","number","boolean","null"] } */
    /* Build and return a Jsonb* */
    /* ... */
    return /* jsonb */;
}

static Jsonb *
schema_for_type(Oid typid, int32 typmod, bool notnull)
{
    /* Map common base types to JSON Schema; include "format" when relevant.
     * Return a Jsonb* object (e.g., {"type":"string"}) */
    /* ... */
    return /* jsonb */;
}

static Jsonb *
schema_for_composite(Oid typid)
{
    /* Inspect pg_type/pg_attribute to list fields; respect attnotnull */
    /* Build {"type":"object","properties":{...},"additionalProperties":false} */
    /* ... */
    return /* jsonb */;
}

static Jsonb *
wrap_array_items(Jsonb *item_schema, bool is_setof)
{
    /* { "type":"array", "items": item_schema, "x-pg-returns":"setof" } */
    /* ... */
    return /* jsonb */;
}

/* ------------- Deep: from a Query/Expr tree ------------- */

static Jsonb *
schema_from_query(Query *query, Oid prorettype, bool proretset)
{
    /* For CMD_SELECT (or DML with RETURNING), analyze targetList.
     * If proretset, return array(items = per-row schema).
     * If scalar, use first TLE's expr. If composite, build per-field schemas.
     */
    /* ... detect json constructors and aggregates in target exprs ... */
    return /* jsonb */;
}

static Jsonb *
schema_from_expr(Node *expr)
{
    /* Core dispatcher:
     * - Const/Var/RelabelType -> schema_for_type(...)
     * - FuncExpr (jsonb_build_object/array, to_json[b]) -> special handling
     * - Aggref (jsonb_agg/jsonb_object_agg) -> array/object agg rules
     * - OpExpr (|| etc.) -> merge
     * - CaseExpr/CoalesceExpr -> union/first-non-null
     * - JsonExpr -> use returning to identify shape (object/array/scalar/bool)
     */
    return /* jsonb */;
}

/* Merging helpers: schema_merge_allof(), schema_union_anyof(), push_meta(), ... */
```

> The key *pattern* above mirrors how `ruleutils.c` reads `prosqlbody` and walks `Query*`. You can copy small snippets (e.g., `stringToNode`, `AcquireRewriteLocks`) with appropriate includes. ([doxygen.postgresql.org][2])

### 2) `src/include/utils/jsonschema.h`

```c
#ifndef JSONSCHEMA_H
#define JSONSCHEMA_H

#include "nodes/pg_list.h"
#include "utils/jsonb.h"

extern Jsonb *json_schema_generate_worker(Oid funcid);

#endif /* JSONSCHEMA_H */
```

### 3) `src/include/utils/builtins.h` (add prototypes)

Add near other JSON or ruleutils prototypes:

```c
/* jsonschema.c */
extern PGDLLEXPORT Datum json_schema_generate_oid(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum json_schema_generate_regprocedure(PG_FUNCTION_ARGS);
extern PGDLLEXPORT Datum json_schema_generate_regproc(PG_FUNCTION_ARGS);
```

---

## Catalog entry (`pg_proc.dat`)

Add entries for three SQL-callable signatures. **Choose temporary OIDs** in the 8000–9999 range during development; they will be renumbered at commit time. Use `src/include/catalog/unused_oids` to find free values. ([PostgreSQL][9], [Database Administrators Stack Exchange][4])

`src/include/catalog/pg_proc.dat`:

```perl
# JSON schema generator
{ oid => '8901', descr => 'generate JSON Schema for a function (by oid)',
  proname => 'json_schema_generate', prolang => 'internal', provolatile => 's',
  prorettype => 'jsonb', proargtypes => 'oid',
  prosrc => 'json_schema_generate_oid' },

{ oid => '8902', descr => 'generate JSON Schema for a function (by regprocedure)',
  proname => 'json_schema_generate', prolang => 'internal', provolatile => 's',
  prorettype => 'jsonb', proargtypes => 'regprocedure',
  prosrc => 'json_schema_generate_regprocedure' },

{ oid => '8903', descr => 'generate JSON Schema for a function (by regproc)',
  proname => 'json_schema_generate', prolang => 'internal', provolatile => 's',
  prorettype => 'jsonb', proargtypes => 'regproc',
  prosrc => 'json_schema_generate_regproc' },
```

> `prolang => 'internal'` with `prosrc` = C symbol name, like other SQL-callable built‑ins. See patterns around similar entries. (Built‑in function OIDs are commonly macro‑exposed automatically.) ([rockdata.net][10])

---

## Build system updates (Meson & Make)

**Meson** (supported officially): add the new source to `src/backend/utils/adt/meson.build`. ([PostgreSQL][11])

```diff
--- a/src/backend/utils/adt/meson.build
+++ b/src/backend/utils/adt/meson.build
@@
 backend_sources += files(
   'json.c',
   'jsonb.c',
   'jsonfuncs.c',
+  'jsonschema.c',
   ...
 )
```

**Make** (Autoconf): add to `src/backend/utils/adt/Makefile`.

```diff
 OBJS = \
   json.o \
   jsonb.o \
   jsonfuncs.o \
+  jsonschema.o \
   ...
```

---

## Regression tests

Create `src/test/regress/sql/jsonschema.sql`:

```sql
-- Objects under test
CREATE TABLE orders (
  id          bigserial PRIMARY KEY,
  customer_id bigint      NOT NULL,
  status      text        NOT NULL,
  created_at  timestamptz NOT NULL
);

CREATE TABLE order_items (
  id        bigserial PRIMARY KEY,
  order_id  bigint      NOT NULL REFERENCES orders(id),
  sku       text        NOT NULL,
  qty       int         NOT NULL CHECK (qty > 0),
  price     numeric(12,2) NOT NULL
);

-- SQL-body function (deep path)
CREATE FUNCTION get_order(_order_id bigint)
RETURNS TABLE (
  id          bigint,
  customer_id bigint,
  status      text,
  created_at  timestamptz,
  items       jsonb
)
RETURN (
  SELECT
    o.id,
    o.customer_id,
    o.status,
    o.created_at,
    COALESCE(
      jsonb_agg(
        jsonb_build_object(
          'id',    i.id,
          'sku',   i.sku,
          'qty',   i.qty,
          'price', i.price
        )
      ) FILTER (WHERE i.id IS NOT NULL),
      '[]'::jsonb
    ) AS items
  FROM orders o
  LEFT JOIN order_items i ON i.order_id = o.id
  WHERE o.id = _order_id
  GROUP BY o.id, o.customer_id, o.status, o.created_at
);

-- Shallow path: non-SQL-body (plpgsql) jsonb return
CREATE FUNCTION trivial_blob(int) RETURNS jsonb
LANGUAGE plpgsql AS $$
BEGIN
  RETURN '{}'::jsonb;
END$$;

-- Exercise the generator
SELECT jsonb_pretty(json_schema_generate('get_order(bigint)'::regprocedure));
SELECT jsonb_pretty(json_schema_generate('trivial_blob(int)'::regprocedure));
```

Expected file `src/test/regress/expected/jsonschema.out` (keep formatting stable with `jsonb_pretty`):

```text
SELECT jsonb_pretty(json_schema_generate('get_order(bigint)'::regprocedure));
                                jsonb_pretty                                
-------------------------------------------------------------------------------
 {                                                                        +
   "$schema": "https://json-schema.org/draft/2020-12/schema",             +
   "title": "public.get_order(bigint)",                                   +
   "type": "array",                                                       +
   "items": {                                                             +
     "type": "object",                                                    +
     "properties": {                                                     +
       "id":          { "type": "number" },                               +
       "customer_id": { "type": "number" },                               +
       "status":      { "type": "string" },                               +
       "created_at":  { "type": "string", "format": "date-time" },        +
       "items": {                                                         +
         "type": "array",                                                 +
         "items": {                                                       +
           "type": "object",                                              +
           "properties": {                                                +
             "id":    { "type": "number" },                               +
             "sku":   { "type": "string" },                               +
             "qty":   { "type": "number", "minimum": 1, "multipleOf": 1 },+
             "price": { "type": "number" }                                +
           },                                                             +
           "required": ["id","sku","qty","price"],                        +
           "additionalProperties": false                                   +
         }                                                                +
       }                                                                  +
     },                                                                   +
     "required": ["id","customer_id","status","created_at","items"],      +
     "additionalProperties": false                                        +
   },                                                                     +
   "x-pg-returns": "setof",                                               +
   "x-pg-introspection": "sql-body",                                      +
   "x-pg-depth": "deep"                                                   +
 }
(1 row)

SELECT jsonb_pretty(json_schema_generate('trivial_blob(int)'::regprocedure));
                jsonb_pretty                 
---------------------------------------------
 {                                           +
   "$schema": "https://json-schema.org/draft/2020-12/schema",+
   "title": "public.trivial_blob(integer)",  +
   "type": ["object","array","string","number","boolean","null"],+
   "x-pg-introspection": "signature-only",   +
   "x-pg-depth": "shallow"                   +
 }
(1 row)
```

> Notes:
> • We represent `RETURNS TABLE` as `"type":"array"` with `"items"` describing one row. This is generally more useful to downstream JSON consumers and we annotate `"x-pg-returns": "setof"`.
> • For the deep path we demonstrate that `jsonb_build_object` keys are constants, therefore `properties` is exact; and `COALESCE(..., '[]')` makes `"items"` non‑nullable.

**Hook tests into schedules**: Append `jsonschema` to `src/test/regress/serial_schedule` (or appropriate `parallel_schedule`) so it runs in `make check`.

---

## Build & run

**Meson (recommended)** ([PostgreSQL][11])

```bash
# From repo root
meson setup build
ninja -C build
ninja -C build install  # optional
# Run regression tests
cd build
meson test --suite regress --print-errorlogs
```

**Autoconf/Make**

```bash
./configure
make -s -j
make check
```

---

## Developer notes, limits, and follow‑ups

1. **Security / visibility**
   We only read catalog metadata and stored parse trees; we never execute target functions. We should not leak names of objects the caller cannot see. Prefer avoiding detailed provenance in `"x-pg-*"` unless necessary.

2. **Precision vs. safety**
   The SQL type is always known; what’s uncertain is JSON *shape* when keys/paths are data‑dependent. In such cases, emit unions (`anyOf`) or `patternProperties` rather than failing. (Examples: dynamic keys for `jsonb_object_agg`, dynamic `jsonb_set` path.) The existence of the SQL return type is guaranteed; we never return “unknown type.” ([PostgreSQL][5])

3. **SQL/JSON nodes**
   Postgres has a `JsonExpr` node family for SQL/JSON features (`JSON_QUERY`, `JSON_VALUE`, `JSON_TABLE`, etc.). You can initially support only simple `JSON()`/`JSON_SCALAR()` and extend later; add stubs recognizing `T_JsonExpr` and deriving `"type"` from `returning`. ([doxygen.postgresql.org][6], [PostgreSQL][12])

4. **Walking trees**
   Use `query_tree_walker` / `expression_tree_walker` idioms; do *not* call `expression_tree_walker` directly at top level—follow the documented pattern of the walker function calling the macro for children. ([PostgreSQL][13], [doxygen.postgresql.org][7])

5. **OID handling**
   During development, pick OIDs in **8000–9999** to minimize collisions. Before submission, maintainers will renumber. Use `src/include/catalog/unused_oids` to find gaps. ([PostgreSQL][9], [Database Administrators Stack Exchange][4])

6. **Docs**
   Add a concise entry under `doc/src/sgml/func.sgml` (“JSON” section or new “Introspection” subsection), noting deep vs. shallow behavior and the presence of `"x-pg-*"` metadata.

7. **Future enhancements**

   * Add `json_schema_generate_sql(sql text)` for ad‑hoc SELECTs (handy for testing visitors).
   * Expose `json_schema_pretty(jsonb)` (thin wrapper around `jsonb_pretty`).
   * Smarter nullability and CHECK inference (parse simple range checks into `"minimum"/"maximum"`, length checks into `"maxLength"`).
   * Recognize more JSON operators (`#>>`, `#-`) and path semantics.

---

## Appendix: Why we rely on `prosqlbody` (and not `prosrc`)

Traditional `LANGUAGE SQL` bodies were stored as text in `prosrc`. The SQL‑standard form stores a **parse tree** in `prosqlbody`, allowing dependency tracking and deparsing without reparsing at call time. Our implementation reads `prosqlbody` with `stringToNode()`, just like `pg_get_function_sqlbody()`, and then inspects the resulting `Query*`/`Expr` trees. ([doxygen.postgresql.org][2])

---

### End-to-end acceptance checklist (Claude)

* [ ] `jsonschema.c` compiles and links; symbols exported via `builtins.h`.
* [ ] `pg_proc.dat` contains 3 new rows; OIDs chosen in 8xxx range; `unused_oids` shows no collisions. ([PostgreSQL][9])
* [ ] Added to Meson & Make build files. ([PostgreSQL][11])
* [ ] Regression tests create sample schema and assert pretty JSON.
* [ ] `make check` / `meson test` passes locally.
* [ ] Minimal docs line added in `func.sgml`.

---

**That’s it.** This patch introduces a safe, read‑only introspection primitive that leverages Postgres’s SQL‑body storage to generate practical JSON Schemas—precise when possible, conservative otherwise.

[1]: https://www.postgresql.org/message-id/E1lUEF5-00027L-N4%40gemulon.postgresql.org?utm_source=chatgpt.com "pgsql: SQL-standard function body"
[2]: https://doxygen.postgresql.org/ruleutils_8c_source.html "PostgreSQL Source Code: src/backend/utils/adt/ruleutils.c Source File"
[3]: https://www.postgresql.org/docs/current/catalog-pg-proc.html?utm_source=chatgpt.com "Documentation: 17: 51.39. pg_proc"
[4]: https://dba.stackexchange.com/questions/316723/oid-release-ranges-for-built-in-aka-standard-database-objects-during-postgresq?utm_source=chatgpt.com "OID release ranges for built-in (aka standard) database ..."
[5]: https://www.postgresql.org/docs/current/xfunc-sql.html?utm_source=chatgpt.com "Documentation: 17: 36.5. Query Language (SQL) Functions"
[6]: https://doxygen.postgresql.org/parse__expr_8c_source.html?utm_source=chatgpt.com "src/backend/parser/parse_expr.c Source File"
[7]: https://doxygen.postgresql.org/nodeFuncs_8h_source.html?utm_source=chatgpt.com "src/include/nodes/nodeFuncs.h Source File"
[8]: https://www.postgresql.org/docs/current/functions-json.html?utm_source=chatgpt.com "Documentation: 17: 9.16. JSON Functions and Operators"
[9]: https://www.postgresql.org/docs/current/system-catalog-initial-data.html?utm_source=chatgpt.com "Documentation: 17: 67.2. System Catalog Initial Data"
[10]: https://www.rockdata.net/docs/11/system-catalog-initial-data.html?utm_source=chatgpt.com "69.2. System Catalog Initial Data"
[11]: https://www.postgresql.org/docs/current/install-meson.html?utm_source=chatgpt.com "17: 17.4. Building and Installation with Meson"
[12]: https://www.postgresql.org/message-id/E1qOZaa-001PJD-4z%40gemulon.postgresql.org?utm_source=chatgpt.com "pgsql: Add more SQL/JSON constructor functions"
[13]: https://www.postgresql.org/message-id/26246.1192198997%40sss.pgh.pa.us?utm_source=chatgpt.com "Re: expression_tree_walker() and primitive node types"

