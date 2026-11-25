# Bulk data paths for ML workloads: design note

Scope: this note describes the design of the research line on MariaDB
13.3. Version 0.5 differs in two points:

| Section | In version 0.5 |
|---|---|
| §2 `query_array` | Same interface. The statement runs through the plugin SQL service: values pass through text and the result is buffered in full (see `py_inhost_rowsink.cc`). The binary row sink, the streaming, the `KILL` forwarding and the 2-D `float32` form of VECTOR columns are not part of 0.5. The measured figures do not apply |
| §3 `py_agg` | Not included |

Status on the 13.3 line: implemented (§2 `query_array`, §3 `py_agg`).

- `py_agg` registers as a native aggregate function plugin (MDEV-40672,
  server 13.3+).
- `query_array` receives binary values through a plugin-defined
  `Protocol` instead of the text client interface.
- Both are additions: existing semantics are unchanged.
- Both belong to [ROADMAP](../ROADMAP.md) Phase 2 (plugin-only band).
  Benchmarks belong to the Phase 3 harness.
- Neither requires server-core changes.

Motivation: running ML inference (onnxruntime / PyTorch), and possibly
training, inside the plugin.  GPU work requires columnar batches
(contiguous numpy arrays, thousands of rows per call).  The scalar data
paths (`inhost.sql.query()` and `py_call`) deliver boxed scalars one
cell or one row at a time.  This document describes the costs of the
scalar paths and specifies the two additions that remove them, one per
direction.

---

## 1. The two scalar paths and their costs

### 1.1 Pull path: `inhost.sql.query()` (py_inhost_module.cc)

This section describes the first implementation of `query()`, which
used the plugin SQL service (`mysql_real_connect_local`).  The row sink
(§2) replaced it because of the costs listed here.

Python requested data with one call and received the whole result set.
Bottlenecks in descending order of cost for bulk numeric data:

| # | Bottleneck | Where | Cost character |
|---|---|---|---|
| 1 | Every cell boxed as a `PyObject` (list of tuples) | result loop in `db_run()` | dominant: ~10M allocations for 1M×10 floats, all under the GIL; `torch.tensor(rows)` then walks all of it again |
| 2 | Text protocol: binary to text in the server, then `strtod`/`PyLong_FromString` | `cell_to_py()` | ~2× per-cell work; inherent to `mysql_real_connect_local` |
| 3 | Double materialization: `mysql_store_result` buffers all rows in C, then the full Python list is built beside it | `db_run()` | peak RAM ≈ 2× data plus Python object overhead (several × raw size) |
| 4 | Fresh internal THD per call (`mysql_real_connect_local` to `mysql_close`) | `db_run()` | negligible for one large query; accumulates for per-batch `LIMIT/OFFSET` loops |

This path was usable for train-once workloads (fetch once, convert
once, iterate epochs in RAM) and costly for any workload that streams
from SQL repeatedly.

### 1.2 Push path: `SELECT py_call('f', cols...) FROM t` (item_func_py_named.cc, marshal.cc)

The executor evaluates the scalar `Item_func` once per row.  Per-cell
marshaling is binary and cheap (`val_real()` to `PyFloat_FromDouble`;
a VECTOR column is a single memcpy into a `float32` ndarray), with no
text round trip.  The costs are per-row and structural:

| # | Cost | Where |
|---|---|---|
| 1 | Granularity (dominant): one Python invocation, hence one model forward pass, per row.  GPU dispatch costs ~0.1 to 1 ms per call regardless of batch size; per-row GPU work is 100 to 1000× slower than batched work, and training cannot be expressed | shape of scalar `Item_func` |
| 2 | GIL acquire/release per row | `*_op()` methods |
| 3 | Fresh globals dict built and destroyed per row (deliberate isolation) | `Item_func_py_call::execute()` |
| 4 | Inline `py_eval` only: compile and dedent on every call (`py_exec` compiles once per statement) | `Item_func_python_base::execute()` |

Items 2 and 3 cost a few µs per row, which is seconds over 1M rows.
Item 1 cannot be fixed inside a scalar function: the call shape does
not allow it.  `JSON_ARRAYAGG(...)` as a single argument batches the
rows, but reintroduces the text bottleneck as one large string plus a
Python-side parse.

### 1.3 Summary

|  | pull `inhost.sql.query` | push `py_call` |
|---|---|---|
| per-cell conversion | text + boxed object | binary + boxed object (VECTOR: memcpy) |
| call granularity | one call, whole result | one call per row |
| GPU-batch friendly | yes, at the cost of boxing | no |

Each path has the property the other lacks.  The two additions below
close the gap from each end.  They share the same columnar output
convention (§4), so user code is identical for both.

---

## 2. Addition A (pull): `inhost.sql.query_array()`

```python
import inhost
cols = inhost.sql.query_array(
    "SELECT x1, x2, label FROM test.samples WHERE split = %s", ("train",))
# cols is a dict: {"x1": ndarray, "x2": ndarray, "label": ndarray}
X = numpy.stack([cols["x1"], cols["x2"]], axis=1)   # ready for torch.from_numpy
```

`query_array(sql, params=None, nulls='error')`.  Placeholder
substitution and the separate-transaction caveats are those of
`query()`.  The result is a dict in select order with one numpy array
per column: numeric columns create no per-cell Python objects.

### Design

- **Column dtype from the result metadata**, decided once per result
  set from the type handler of each select item:

  | SQL type | dtype | NULL becomes |
  |---|---|---|
  | TINYINT/SMALLINT/MEDIUMINT/INT/BIGINT | `int64` (`uint64` if unsigned) | error; with `nulls='nan'` the column becomes `float64` with NaN |
  | YEAR | `uint64` | as above |
  | FLOAT/DOUBLE/DECIMAL | `float64` | NaN |
  | VECTOR | 2-D `float32` (rows × dim) | error |
  | anything else | `object` array of `str` (UTF-8) / `bytes` (binary) | `None` |

  DECIMAL degrades to `float64` by design: `query_array` serves
  numerics at scale, and `query()` is the lossless path.  `nulls='nan'`
  converts only the integer columns that contain a NULL.
- **Binary row sink, no text** (`py_inhost_rowsink.cc`).  The statement
  does not go through `mysql_real_connect_local`: that interface is
  built on `Protocol_text` and `MYSQL_ROW`, formats every value as text,
  and buffers the whole result before returning (`loc_use_result()` is
  `mysql_store_result()`, so `mysql_use_result` cannot stream).
  The plugin creates a private THD, set up like the one the SQL
  service creates, and runs the statement with
  `Ed_connection::execute_direct(Protocol *, LEX_STRING)`, passing its
  own `Protocol` subclass (`Column_sink`).  The executor hands every
  value to the typed `Protocol::store_*()` calls, which append it to a
  per-column C buffer.  This removes bottlenecks 1, 2 and 3 of §1.1
  together.  DECIMAL is the one type the server still delivers as
  text; it is parsed with `my_strtod`.
- **Streaming:** rows reach the sink as the executor produces them.
  The row count is unknown up front, so the buffers grow geometrically
  and each is copied once into an exact-size ndarray at the end, then
  freed column by column.  Peak RAM is the data plus one column.
- **GIL:** released for the whole statement; held only to build the SQL
  text from the parameters and to create the arrays.
- **Session of the private THD:** grants skipped, binlog off, query
  cache off, character sets utf8mb4 (the statement text is UTF-8 and
  strings are returned as UTF-8 whatever the column charset), backslash
  escapes on (parameter escaping relies on them).
- **KILL:** the private THD is not visible to `KILL`.  The sink checks
  the calling THD at every row and forwards a kill, so an extraction
  stops at the next row.  A statement that scans for a long time
  without producing rows is not interrupted until it produces one.
- **Errors:** more than one result set, duplicate column names, NULL in
  a VECTOR column and VECTOR values of different dimensions raise
  `inhost.sql.Error`.
- **Server internals relied on** (to be checked again when the server
  pin moves): `select_send::send_data()` calls `prepare_for_resend()`
  at the start of every row but `write()` only when the THD has a
  network connection, so the sink takes the row boundary from
  `prepare_for_resend()`; `Field::send()` and
  `Type_handler::Item_send()` dispatch to the typed `store_*()` calls
  for any protocol that is not a `Protocol_text`.
- **`query()` and `execute()`** use the same sink in a lossless mode
  (`Rowsink_options::exact`): DECIMAL stays text, FLOAT converts
  through its shortest decimal form, and VECTOR is returned as bytes.
  `Rowsink_options::multi` selects the handling of several result
  sets: an error (`query`, `query_array`), all kept (`query_sets`), or
  the first kept (`execute`).  Each result set carries its column
  names (`inhost.sql.Rows.columns`).  The rows are turned into tuples
  after the statement, with the GIL held.  Measured on the table
  below: `query()` alone 0.37 to 0.41 s.  The cost table in §1.1
  describes the path this replaced.
- **THD reuse** across calls is not implemented.  It would amortize
  bottleneck 4 of §1.1 for per-batch query loops and is independent of
  the row sink.

Measured (RelWithDebInfo, 1M rows × 6 numeric columns, one run on the
development machine): `query()` + `numpy.array(rows)` 3.1 to 3.3 s,
`query_array()` 0.11 s, about 30×.  A repeatable figure belongs to the
Phase 3 benchmark harness.

---

## 3. Addition B (push): `py_agg`, batching aggregate

```sql
-- one Python call per GROUP, receiving columnar arrays
SELECT py_agg('embed_batch', body_text) FROM test.docs;
SELECT label, py_agg('train_partial', x1, x2, y)
FROM test.samples GROUP BY label;
```

The named function (an ordinary catalog entry) receives each argument
as a numpy array covering all rows of the group instead of a scalar:

```sql
SELECT py_create_function('embed_batch', 'BLOB', 'texts',
'
import numpy
result = model.encode(list(texts)).tobytes()   # one batched forward pass
');
```

### Design

Implementation: `item_sum_py_agg.cc`.

- **Registration:** a native aggregate function plugin (MDEV-40672,
  server 13.3+; decision in [ROADMAP](../ROADMAP.md) Phase 2).  `py_agg`
  is a `MariaDB_FUNCTION_PLUGIN` like `py_eval`/`py_exec`, in the same
  `.so`: its `Create_func` returns `is_aggregate() == true` and builds
  `Item_sum_py_agg`, an `Item_sum_plugin` subclass.  No `CREATE
  AGGREGATE FUNCTION` step exists.  The function name must be a string
  constant, resolved at fix time as in `py_call`, because the catalog
  entry fixes the result type; the argument count is checked there too.
  `Item_sum_plugin` has only single-argument constructors, so the item
  is constructed with the first argument and then given the whole list
  (`set_arguments()`), as `Item_sum(THD *, List<Item> &)` does.
- **`add_row()` is Python-free:** per row, it reads each argument
  through `aggregation_arg(i)` and appends its binary value
  (`val_int`/`val_real`/string bytes, VECTOR payload) to per-column
  growing C buffers.  No GIL, no PyObject, no dict.  The buffers and the
  SQL type to dtype mapping are those of §2 (`Rowsink_column`,
  `rowsink_kind_for()`), decided from the argument types at fix time.
- **Result (`val_*()` accessors):** the first accessor call after a
  group acquires the GIL once, copies each buffer into an exact-size
  ndarray (`py_column_to_array()`, shared with §2), evaluates the cached
  code object with the arrays bound to the declared parameter names (or
  as the tuple `args`), and keeps `result` until the next `add_row()` or
  `clear()`.  Conversion to SQL uses the existing unmarshal path.
- **Empty group:** SQL NULL; Python is not called.
- **Result type:** the declared return type of the function.  A VARCHAR
  return becomes the TEXT type that fits (the result field of an
  aggregate is created directly from its type handler, and an
  unbounded VARCHAR is not a valid column).
- **DISTINCT / FILTER:** supplied by the server; `add_row()` sees only
  rows that pass `FILTER (WHERE ...)`, deduplicated under `DISTINCT`.
  DISTINCT replay order is not guaranteed; `py_agg` keeps
  `supports_distinct() == true`, since group row order is unspecified
  without DISTINCT as well.
- **NULLs:** NaN for float columns, `None` in object columns; a NULL in
  an integer or VECTOR argument is an error (to be excluded with
  `FILTER (WHERE ... IS NOT NULL)` or `COALESCE`).
- **Errors:** a Python exception aborts the statement or yields NULL
  with a warning, according to the `strict_errors` setting of the
  function.
- **Memory guard:** `lang_python_agg_max_mem` (session, default 256 MB)
  bounds the bytes one group may buffer; exceeding it aborts the
  statement.  Groups beyond the limit are handled by Python aggregates,
  which receive the same buffers in chunks
  ([AGGREGATE_PROTOCOL](AGGREGATE_PROTOCOL.md)).
- **Window frames:** `supports_removal()` is false.  The server
  evaluates each frame with `clear()` plus `add_row()` per frame row:
  correct, but one Python call per output row over the whole frame.
  Incremental `remove_row()` is not implemented, because removal has
  no simple semantics on the column buffers.
- **KILL:** `add_row()` runs inside the executor loop, so statement kill
  interrupts the scan; only the Python call itself is not interruptible,
  as for any long Python body.
- **Catalog changes:** the item holds its own reference to the catalog
  entry, so a function dropped or replaced while a statement runs does
  not affect that statement; a prepared statement picks up the new
  entry at its next execution.

Measured (RelWithDebInfo, 1M rows, one run on the development
machine): `SUM(py_call('f', x))` with `f` doubling its argument 9.5 s;
`py_agg` with the same arithmetic on the array 0.06 s; native
`SUM(x * 2.0)` 0.08 s.

For batched inference the division of work is: the server performs the
scan, filter and join; the plugin performs one binary columnar marshal;
Python receives arrays in the form `torch.from_numpy` accepts.
`GROUP BY` provides mini-batching by any SQL expression
(`GROUP BY id DIV 4096` for fixed-size chunks).

---

## 4. Shared output convention

Both additions present columns the same way: one 1-D ndarray per
column (2-D `float32` for VECTOR), with dtypes per the §2 table.  User
code that moves between the pull and push styles reuses its
`numpy.stack` / `torch.from_numpy` code unchanged.

---

## 5. Unchanged behaviour

| Item | State | Reason |
|---|---|---|
| `query()` and scalar `py_call` semantics | Unchanged; the additions are new entry points | Scalar per-row calls fit logic that is cheap per row; `query()` fits small or lossless (DECIMAL, mixed-type) results |
| Fresh-namespace isolation of named functions | Kept by `py_agg` (one namespace per group) | A def-wrap plus `PyObject_Vectorcall` fast mode for the scalar path is a separate Phase 3 item (2 to 5× on per-row overhead); it does not solve batching and is outside the scope of this document |

---

## 6. Non-goals

| Non-goal | Reason |
|---|---|
| Binary result path through the plugin SQL service (`Protocol_local`) | Not needed: `query_array` gets binary values from its own `Protocol` subclass (§2) with no server change |
| True table-valued functions (model produces N rows) | Phase 6 machinery, per ROADMAP (server-core band) |
| Parallel or multi-core Python | Phase 4 |
| Zero-copy from the storage engine | Not pursued |

---

## 7. Decisions and open questions

### Decisions

| # | Decision | Choice | Reason |
|---|---|---|---|
| 1 | Return shape of `query_array` | A dict keyed by column name, in select order (`x1, x2 = cols.values()` for positional use); duplicate names are an error; no tuple variant | The cost is per column, not per row |
| 2 | Ownership of the column data | One copy into an exact-size ndarray at the end | No capsule ownership is needed |
| 3 | `py_agg` in `args[]` mode (no parameter names) | A tuple of ndarrays is passed; in named mode the argument count must match the declaration | Mirrors scalar mode |
| 4 | Registration of `py_agg` | Native aggregate function plugin; no aggregate-UDF `SONAME` registration | No UDF registration has to coexist with the `INSTALL PLUGIN`-ed `.so` |

### Open questions

5. Whether `query_array` gains a chunked iterator
   (`for chunk in inhost.sql.query_iter(...)`) for larger-than-RAM
   streaming into training loops.  The row sink already streams, but
   the statement runs to completion inside one call; yielding chunks to
   Python mid-statement requires the sink to call back into Python, or
   a second thread.  Not implemented: no concrete need exists.
