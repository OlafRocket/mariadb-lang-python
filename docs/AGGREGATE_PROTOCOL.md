# Incremental Python aggregates: design

Status: **implemented**.
Scope: the remaining item of [ROADMAP](../ROADMAP.md) Phase 2, the Python
accumulate protocol. Builds on `py_agg` ([BULK_DATA](BULK_DATA.md) §3).

## 1. Problem

`py_agg` buffers a whole group and calls Python once. Memory per group is
bounded by `lang_python_agg_max_mem`, so a group larger than that limit
cannot be aggregated at all. Workloads that hit this:

| Workload | Why whole-group buffering fails |
|---|---|
| Aggregate over a full large table (no `GROUP BY`) | One group holds every row |
| Online training (`partial_fit`, SGD epochs) | The model consumes batches; the data set does not fit in memory |
| Streaming statistics, sketches | State is small; the input is not |

An incremental aggregate keeps Python state per group, feeds it rows as
they arrive and asks it for the result at the end.

## 2. Design in brief

- A new catalog kind, **aggregate**, created with
  `py_create_aggregate(name, return_type, param_names, body [, strict_errors])`.
- The body defines a class named `Aggregate` with two methods:
  `step(self, <params>)` and `finalize(self)`.
- `py_agg('name', cols...)` stays the only SQL aggregate. It dispatches on
  the catalog kind: a function gets the whole group in one call (today's
  behaviour), an aggregate gets the group in chunks.
- `step` receives **numpy arrays covering one chunk of rows**, not one row.
  Rows are still collected in C without running Python.

```sql
SELECT py_create_aggregate('mean', 'DOUBLE', 'x', '
class Aggregate:
    def __init__(self):
        self.n = 0
        self.total = 0.0
    def step(self, x):            # x: float64 array, one chunk
        self.n += len(x)
        self.total += float(x.sum())
    def finalize(self):
        return self.total / self.n if self.n else None
');

SELECT grp, py_agg('mean', val) FROM big_table GROUP BY grp;
```

## 3. Decisions

In every row option (a) was chosen.

| # | Question | Options | Decision and reason |
|---|---|---|---|
| 1 | What does `step` receive? | (a) arrays for a chunk of rows; (b) scalars for one row, as Python's `sqlite3.create_aggregate` | (a). One Python call per row costs about as much as `py_call` (9.5 s per 1M rows measured) and defeats the purpose of Phase 2. Chunks keep the C-side accumulation of `py_agg` and bound memory at the same time. A per-row aggregate is the special case chunk size 1. |
| 2 | How is an aggregate declared? | (a) new management function plus a `kind` column in `mysql.lang_python_func`; (b) detect a class named `Aggregate` in an ordinary function body | (a). Detection needs the body to be executed at create time, which runs user imports and side effects during DDL; bodies are only compiled today. An explicit kind also maps directly to `CREATE AGGREGATE FUNCTION ... LANGUAGE PYTHON` in Phase 5. |
| 3 | SQL surface | (a) reuse `py_agg`; (b) a second aggregate, for example `py_agg_inc` | (a). The caller does not need to know how the function is written, and a function can be reimplemented as an aggregate without touching queries. |
| 4 | Form of the body | (a) class with methods; (b) one body run per phase with a `phase` variable; (c) three separate catalog functions (state, step, final), PostgreSQL style | (a). State lives in the instance; (b) forces state into globals and string dispatch; (c) triples the catalog entries per aggregate. |
| 5 | Chunk size control | (a) session variable `lang_python_agg_chunk_rows`, default 65536; (b) per-aggregate option at create time | (a). It is a tuning knob of the session (GPU batch size); a per-aggregate override can be added later without breaking anything. |
| 6 | Window frames with removal (`inverse`) | (a) not in the first version; (b) optional `inverse` method | (a). Whether removal is supported must be known before execution starts, which would force the body to run at prepare time. Frames still work through recomputation, as for `py_agg` today. |

## 4. Semantics

### 4.1 Lifecycle

| Server event | Python action |
|---|---|
| First row of the statement for this item | Execute the body once in a fresh namespace; look up `Aggregate` |
| `clear()` (start of a group or frame) | Drop the previous instance; create `Aggregate()` lazily on the first chunk |
| `add_row()` | Append to the C buffers (no Python). When the buffer holds `lang_python_agg_chunk_rows` rows: build arrays, call `step`, empty the buffers |
| First `val_*()` after rows | Flush the pending partial chunk through `step`, call `finalize`, cache the result until the next row or `clear()` |

- A group without rows returns NULL; `Aggregate` is not instantiated.
- `finalize` can be called more than once on the same instance (running
  window frames read a result, then add rows). It must not consume or
  reset state. This is a documented contract, not something the plugin can
  enforce.
- Chunks arrive in scan order within a group; that order is undefined
  unless the plan guarantees it.
- A chunk boundary carries no meaning. An aggregate must give the same
  result for any chunk size; the test suite runs each aggregate with
  several sizes, including 1.

### 4.2 Arguments, result, errors

| Aspect | Behaviour |
|---|---|
| Argument arrays | Same type mapping as `py_agg` and `query_array` (int64/uint64, float64, 2-D float32, object) |
| Binding | `step` is called with one positional array per SQL argument. With `param_names` the count must match (checked at prepare time). Without, `step(self, *cols)` |
| NULL | As `py_agg`: NaN in float arrays, `None` in object arrays, error for integer and VECTOR |
| Result | The value returned by `finalize`, converted to the declared return type like a `py_call` result; `None` is NULL |
| Exception in `__init__`, `step` or `finalize` | Strict: statement aborts. Lax: warning, the group's result is NULL and its remaining rows are skipped |
| `DISTINCT`, `FILTER` | Applied by the server before `add_row()`, as for `py_agg` |
| Memory | `lang_python_agg_max_mem` remains the hard cap on buffered bytes; with chunking it bounds one chunk instead of one group. Python-side state is the author's responsibility |

### 4.3 Calling a function through the wrong entry point

| Call | Result |
|---|---|
| `py_agg` on a function | Whole-group call (unchanged) |
| `py_agg` on an aggregate | Chunked protocol |
| `py_call` on an aggregate | Error at prepare time |
| `py_create_function` over an existing aggregate name, or the reverse | Error; names share one namespace. `py_replace_*` may change the kind |
| `py_drop_function` | Drops either kind |

## 5. Implementation outline

| Area | Change |
|---|---|
| Catalog | Column `kind ENUM('function','aggregate') NOT NULL DEFAULT 'function'`. `PyFuncEntry` gains `is_aggregate` |
| Management functions | `py_create_aggregate`, `py_replace_aggregate` as two more modes of `Item_func_py_manage`. Create-time validation compiles the body, as today; the presence of `Aggregate` is checked at first use |
| `Item_sum_py_agg` | State gains the namespace, the class and the instance. `add_row()` flushes at the chunk limit; `compute()` flushes and calls `finalize`. The whole-group path is the same code with an unlimited chunk and a function call instead of `step`/`finalize` |
| System variable | `lang_python_agg_chunk_rows` (session, default 65536, minimum 1) |
| Tests | Kinds and dispatch; chunk sizes 1, 7 and default giving identical results; group larger than `agg_max_mem` succeeding; DISTINCT, FILTER, windows; errors in each method, strict and lax; KILL during accumulation; concurrent connections |
| Docs | USAGE.md, BULK_DATA.md, LIMITATIONS.md, ROADMAP.md |

Implemented in `item_sum_py_agg.cc` (`step_chunk()`, `finalize_group()`),
`py_func_catalog.cc` and `item_func_py_named.cc`; test `py_aggregate`.
No server changes; no new dependency on server internals beyond what `py_agg` uses.

## 6. Costs and risks

| Item | Detail |
|---|---|
| Catalog schema change | First change to `mysql.lang_python_func` since Phase 1.5. A table created before the column existed makes the catalog load fail until the column is added by hand (statement in LIMITATIONS.md) |
| GIL per chunk | One acquire and one Python call per 65536 rows by default; negligible against the per-row cost it replaces |
| Correctness burden on authors | Results must not depend on chunk boundaries, and `finalize` must be repeatable. Both are easy to get wrong and are not detectable by the plugin |
| Python state is unbounded | The memory guard covers the C buffers only |
| Body runs once per statement item | Imports and model loading in the body run again for every statement, as for named functions. Caching loaded models across statements is a Phase 3 topic |

## 7. Out of scope

- Per-row scalar `step` as a separate mode (decision 1): available as
  chunk size 1 if ever needed for compatibility.
- `inverse` / `value` for incremental window frames (decision 6).
- Parallel partial aggregation with a `merge` method: the server has no
  parallel aggregation to feed it.
- Changing whole-group `py_agg` functions: they keep working unchanged.
