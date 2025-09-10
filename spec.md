# PostgreSQL Common Subexpression Elimination (CSE) Implementation

## Problem
PostgreSQL's query optimizer currently flattens expressions, causing massive duplication when the same subexpression appears multiple times. For example, in the provided Easter calculation query, expressions like `($1 / 100)` and `($1 % 19)` are computed dozens of times instead of once, severely impacting performance.

## Goal
Implement Common Subexpression Elimination (CSE) in PostgreSQL's optimizer to detect duplicate expressions and evaluate them only once.

## Context
- The issue occurs because PostgreSQL flattens LATERAL joins and inlines all expressions
- CSE should identify expressions that appear multiple times and replace them with references to a single evaluation
- This optimization should be safe (no side effects) and not negatively impact other queries

## Requirements
1. **Safety**: Only apply CSE to immutable expressions (no side effects)
2. **Opt-in**: Add a GUC parameter `enable_cse` (default OFF for backward compatibility)
3. **Configurable**: Add threshold for minimum references before applying CSE
4. **Correctness**: Must produce identical query results with CSE on or off

## Test Case
The Easter calculation query from the problem description should show significant improvement with CSE enabled. The EXPLAIN plan should no longer show massive expression duplication.

This query below should run faster with CSE enabled.

To benchmark it, you need to recompile and restart PostgreSQL, here is how:

```
joel@Mac build % pwd
/Users/joel/src/postgresql/build
joel@Mac build % ninja install 1>/dev/null
joel@Mac build % pg_ctl -D "$HOME/pg-patch-0001-data" -l logfile restart
waiting for server to shut down.... done
server stopped
waiting for server to start.... done
server started
joel@Mac build % psql test
psql (19devel)
Type "help" for help.
```

```sql
PREPARE calc_easter_day_for_year AS
SELECT make_date($1::integer, easter_month, easter_day)
FROM (VALUES ($1::integer % 19, $1::integer / 100)) AS step1(g,c)
CROSS JOIN LATERAL (VALUES ((c - c/4 - (8*c + 13)/25 + 19*g + 15) % 30)) AS step2(h)
CROSS JOIN LATERAL (VALUES (h - (h/28)*(1 - (h/28)*(29/(h + 1))*((21 - g)/11)))) AS step3(i)
CROSS JOIN LATERAL (VALUES (($1::integer + $1::integer/4 + i + 2 - c + c/4) % 7)) AS step4(j)
CROSS JOIN LATERAL (VALUES (i - j)) AS step5(p)
CROSS JOIN LATERAL (VALUES (3 + (p + 26)/30, 1 + (p + 27 + (p + 6)/40) % 31)) AS step6(easter_month, easter_day)
;

SET plan_cache_mode = 'force_generic_plan';

EXPLAIN (ANALYZE, VERBOSE) EXECUTE calc_easter_day_for_year(2021);
```                                                                                                                                             

This currently results in the following plan:

```
 Result  (cost=0.00..1.14 rows=1 width=4) (actual time=0.025..0.026 rows=1.00 loops=1)
   Output: make_date($1, (3 + (((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11))))) - (((((($1 + ($1 / 4)) + ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11)))))) + 2) - ($1 / 100)) + (($1 / 100) / 4)) % 7)) + 26) / 30)), (1 + ((((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11))))) - (((((($1 + ($1 / 4)) + ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11)))))) + 2) - ($1 / 100)) + (($1 / 100) / 4)) % 7)) + 27) + (((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11))))) - (((((($1 + ($1 / 4)) + ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) - (((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (1 - ((((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) / 28) * (29 / ((((((($1 / 100) - (($1 / 100) / 4)) - (((8 * ($1 / 100)) + 13) / 25)) + (19 * ($1 % 19))) + 15) % 30) + 1))) * ((21 - ($1 % 19)) / 11)))))) + 2) - ($1 / 100)) + (($1 / 100) / 4)) % 7)) + 6) / 40)) % 31)))
 Planning Time: 5.957 ms
 Execution Time: 0.296 ms
(4 rows)
```

## Hints for Exploration
- The optimizer code is in `src/backend/optimizer/`
- Expression evaluation happens in `src/backend/executor/`
- Look at how `eval_const_expressions()` works as a model for expression tree walking
- Consider how PostgreSQL already handles expression evaluation and caching
- The planner creates expression trees that the executor evaluates
- GUC parameters are defined in `src/backend/utils/misc/guc.c`

## Success Criteria
1. The Easter calculation query shows dramatically reduced expression duplication in EXPLAIN output
2. Performance improves for queries with repeated complex expressions
3. No regression in other queries (CSE can be disabled)
4. Passes PostgreSQL regression tests

Explore the codebase, understand how expression evaluation currently works, and design an appropriate solution for implementing CSE.
