# lang_python Roadmap

lang_python is a research and development project. It embeds a bundled
CPython interpreter in the MariaDB server and investigates how far an
embedded language can be taken beyond the classic stored procedure and
stored function: inline code, columnar aggregates, window functions,
in-process access to the hosting server, and numerical and ML workloads
on numpy arrays.

The roadmap is organised by research question. Each phase states the
question, the status, the result, and the alternatives that were
rejected.

## Overview

| Phase | Question | Status |
|---|---|---|
| 1 | Can Python run inline in a SQL expression, with no DDL? | Done |
| 1.5 | Can functions be named and persistent without SQL grammar changes? | Done |
| 1.6 | Can Python code query the server it runs in, in process? | Done |
| 2 | Can Python work on whole groups of rows as columnar arrays (aggregates, window functions, bulk reads)? | Done |
| 3 | What does a Python call cost per row, and how far can it be reduced? | In progress |
| 4 | Can Python calls of concurrent connections run in parallel? | Planned |
| 5 | `CREATE FUNCTION ... LANGUAGE PYTHON` | Later, needs server changes |
| 6 | `CREATE PROCEDURE ... LANGUAGE PYTHON`, table-valued functions | Later, needs server changes |
| 7 | Triggers and events in Python | Later, needs server changes |

The phases form two bands:

| Band | Phases | Constraint |
|---|---|---|
| Plugin only | 1 to 4 | No SQL grammar changes and no edits to server code outside the plugin. Everything uses the embedded interpreter and the plugin APIs of an unmodified server. |
| Server core | 5 to 7 | Requires changes to the MariaDB grammar or core. Deferred until the plugin-only band is complete. |

The plugin-only band comes first on purpose: it measures how much is
possible with the extension points the server already offers, and it
identifies precisely which features need a server change and why.

## Server target

Version 0.5 targets a release of MariaDB 12.3, without server changes.
The pin and the procedure for moving it are in [BUILD.md](BUILD.md).

| Phase | In version 0.5 |
|---|---|
| 1, 1.5 | Included |
| 1.6 | Included, on the plugin SQL service: values pass through text, the result is buffered in full |
| 2 | `inhost.sql.query_array` included. Aggregates (`py_agg`, Python aggregate classes) not included: they need native aggregate function plugins (MDEV-40672), which are in MariaDB 13.3 |
| 3 and later | Not included |

The phase descriptions below record the results of the research line on
MariaDB `main` (13.3), including the parts that version 0.5 leaves out.

## Plugin-only band

### Phase 1: Inline functions (done)

Question: can Python run inline in a SQL expression, with no DDL and no
function definition?

- `py_eval(expr, type [, args...])` evaluates one Python expression.
- `py_exec(code, type [, args...])` executes a code block that assigns
  `result`.
- Values are marshalled in both directions: INT, DOUBLE, DECIMAL,
  VARCHAR, BLOB, BOOL. VECTOR columns arrive as numpy arrays.
- `lang_python_strict_errors` selects between aborting the statement and
  returning NULL with a warning.
- The interpreter and numpy are bundled; the build is containerised.

### Phase 1.5: Named functions (done)

Question: can Python functions be named, persistent and compiled once,
without SQL grammar changes?

- SQL functions: `py_create_function`, `py_replace_function`,
  `py_drop_function`, `py_call` (see [USAGE.md](USAGE.md)).
- The catalog table `mysql.lang_python_func` stores the functions. It is
  created on first use.
- Bodies compile once, at create time and at catalog load after a
  restart. Syntax errors surface at create time. `py_call` evaluates the
  cached code object for every row.
- Bodies compile unmodified as module-level code with the `py_exec`
  convention (assign `result`). They are not wrapped in a `def`.
- Parameters are named variables when `param_names` is set, otherwise
  the `args` tuple.
- `strict_errors` is fixed per function at create time.
- Catalog SQL runs through the plugin SQL service
  (`mysql_real_connect_local()`). `Ed_connection` was rejected: its
  result-collecting `Protocol_local` is private to `sql_prepare.cc`.

Design: [docs/NAMED_FUNCTIONS.md](docs/NAMED_FUNCTIONS.md).

Result: persistence and compile-once work without grammar changes.
Calling a user function directly by its name (`SELECT myfunc(3, 4)`)
does not; see
[Finding: direct name dispatch needs the parser](#finding-direct-name-dispatch-needs-the-parser).

### Phase 1.6: In-process server access (done)

Question: can Python code read and modify the database it runs in,
without a client connection?

The built-in extension module `inhost` provides this (see
`py_inhost_module.cc` and [USAGE.md](USAGE.md)). `inhost` is a
namespace: `inhost.sql` exists today, and further server services get
sibling submodules.

```python
import inhost
for row in inhost.sql.query("SELECT name FROM test.t WHERE id > %s", (x,)):
    ...
n = inhost.sql.execute("UPDATE test.t SET hits = hits + 1")
```

The module is not named `mariadb`. It is not the connector API (no
connect, cursor or commit), and the connector's name would shadow the
PyPI package of that name.

The execution context is the central design problem. A scalar function
runs in the middle of the evaluation of the calling statement, on a THD
that is already executing it. Re-entering the SQL layer on that THD is
unsafe. The plugin therefore creates a separate internal THD with no
socket and runs the statement through its own `Protocol` subclass (the
row sink, `py_inhost_rowsink.cc`).

The plugin SQL service (`mysql_real_connect_local()`) was the first
implementation and remains in use for the catalog. It was replaced for
`inhost.sql` for three reasons:

- It converts every value to text.
- It buffers the whole result.
- It switches to a separate THD only when the calling statement holds
  table locks. A call from a statement without tables runs in the
  caller's own session.

Consequences of the separate THD:

| Property | Behaviour |
|---|---|
| Transaction | Separate from the calling statement. Uncommitted changes of the caller are not visible. Writes commit independently (autocommit). |
| Session | One session per call. No session state survives between calls (temporary tables, variables, `BEGIN` / `COMMIT`). There is no current database; table names are qualified. |
| Writes from a per-row function | Hazardous: the optimizer may invoke the function any number of times, in any order. Documented, not prevented. |
| GIL | Released while the internal statement runs. |
| Kill switch | `SET GLOBAL lang_python_db_access = OFF`. |

Open:

- Reuse of the internal session between calls (Phase 3).
- A security context for the internal THD. It runs without grant checks,
  like all plugin SQL. This is acceptable only because the plugin
  already implies trust in arbitrary code.
- SQL that is atomic with the calling statement (Phase 6).

### Phase 2: Aggregates and bulk data paths (done)

Question: can Python work on many rows at once, as columnar numpy
arrays, instead of one boxed scalar per call? This is the shape that
numerical and ML code needs (batched inference, training, GPU work).

Design: [docs/BULK_DATA.md](docs/BULK_DATA.md),
[docs/AGGREGATE_PROTOCOL.md](docs/AGGREGATE_PROTOCOL.md).

| Feature | Direction | Description |
|---|---|---|
| `inhost.sql.query_array()` | Pull | A result set becomes one ndarray per column. Values arrive in binary form through the row sink: no text round trip, streaming fill with the GIL released, no Python object per cell. |
| `py_agg('fn', cols...)` with a named function | Push | A native aggregate (`Item_sum_py_agg`). `add_row()` appends binary values to C buffers without the GIL. The named function is called once per group with columnar arrays. |
| `py_agg` with a Python aggregate | Push | For groups too large to buffer. `py_create_aggregate` stores a class `Aggregate` with `step` and `finalize`; `py_agg` passes `step` one chunk of rows at a time as numpy arrays. |
| `inhost.sql.query()`, `execute()`, `query_sets()` | Pull | All of `inhost.sql` runs through the row sink. `query()` returns its column names (`Rows.columns`). `query_sets()` returns every result set of a statement; `query()` rejects a statement that has several. |

Registration of `py_agg`: a native aggregate function plugin
(MDEV-40672, server 13.3 and later). An aggregate is a
`MariaDB_FUNCTION_PLUGIN` like `py_eval`. Its `Create_func` builder
overrides `is_aggregate()` and creates an `Item_sum_plugin` subclass
that implements `clear()`, `add_row()` and the `val_*()` result
accessors. The server supplies `DISTINCT`, `FILTER (WHERE ...)` and
`OVER (...)` framing, so `py_agg` is also a window function. Arguments
are read as Items (`aggregation_arg(i)`): values arrive in binary form
and pluggable types such as VECTOR keep their type. SQL calls use
ordinary function syntax; there is no `CREATE AGGREGATE FUNCTION` step.

Rejected alternatives:

| Alternative | Reason for rejection |
|---|---|
| Loadable aggregate UDF API (`CREATE AGGREGATE FUNCTION ... SONAME`, `xxx_clear` / `xxx_add`) | Passes only `Item_result` categories, so VECTOR arrives as a string. Needs a separate DDL step. Needs one `.so` to serve as both plugin and UDF. |
| Per-row Python protocol (`clear` / `add` / `finalize`, as `sqlite3.create_aggregate`) | One Python call per row costs about as much as `py_call` and removes the benefit of the columnar path. The chunked `step` covers it as the special case of chunk size 1. |

Not implemented: incremental window frames (`inverse`), parallel
aggregation (`merge`). See [LIMITATIONS.md](LIMITATIONS.md).

### Phase 3: Performance (in progress)

Question: what does a Python call cost per row, where does the time go,
and how far can it be reduced within a single interpreter? Parallelism
across cores is Phase 4.

Done:

- Benchmark harness in `bench/` (runner, cases, comparison of two runs;
  see [bench/README.md](bench/README.md)). Every item of this phase is
  measured with it.
- Compile cache for `py_exec`. The code object is kept in the SQL
  function instance and reused for the following rows while the source
  text is unchanged. Dedent runs once with it. Measured: 48.9 to 10.0 µs
  per row for a one-line body, 81.7 to 11.0 µs for a multi-line body.
  `py_exec` is now level with `py_call`.
- Python thread state kept for the statement (`Py_thread_pin`) and
  interned strings for the names looked up on every call (`__builtins__`,
  `args`, `result`, the parameter names of a named function). Before,
  every call created and destroyed a Python thread state. Measured
  together: `py_call` 9.4 to 0.53 µs per row, `py_exec` 10.0 to 0.58 µs,
  `py_eval` 42 to 23 µs, indexed `inhost.sql` point query 77 to 58 µs.
  The GIL is taken and released per call as before.

Decisions on the thread state:

| Decision | Reason |
|---|---|
| The thread state lives for one statement, not for the server thread. | Nothing of Python's per-thread state outlives the statement. Under the thread pool a server thread serves several connections; a state kept per thread would carry one session's thread-local data into another. |
| The GIL is not held between calls. | Holding it for the statement would block the Python code of every other connection for that long. |

Decisions on the compile cache:

| Decision | Reason |
|---|---|
| `py_eval` is not cached. | It is the one-off form. Per-row work belongs in `py_exec` or a named function. |
| The cache is per function instance, not global. | No lock, no size limit, no eviction. A cache shared between statements is considered only if the harness shows a need. |
| Compiled code is not stored in the catalog table. | Named functions compile once at create time and once at server start. Stored bytecode saves only the start-up compile, is tied to the exact Python version, and can crash the interpreter when corrupted. |

Current figures:

| Path | Cost |
|---|---|
| Native SQL expression | 0.13 µs per row |
| `py_exec`, `py_call` | About 0.55 µs per row |
| `py_eval` | About 23 µs per row |
| `inhost.sql` indexed point query | About 58 µs per call |

Concurrent connections running the per-row `py_call` statement
(`bench/concurrent.sh`, total rows per second):

| Connections | Before the thread state change | After |
|---|---|---|
| 1 | 102,000 | 1,300,000 |
| 2 | 64,000 | 638,000 |
| 4 | 66,000 | 280,000 |
| 8 | 66,000 | 223,000 |

Open:

- GIL handover under concurrency. The total throughput of per-row calls
  falls as connections are added (table above): every row takes and
  releases the GIL, and a contended handover costs more than the call.
  Candidate: keep the GIL for a bounded number of rows.
- Remaining per-row overhead of scalar calls. Candidates: the fresh
  namespace per row; calling a prebuilt function object in `py_call`
  instead of evaluating the module code.
- Internal session per `inhost.sql` call. Candidate: THD reuse.
- Callables resolved once at plugin start (`textwrap.dedent`, which
  `py_eval` still looks up per call) and heavy modules imported once
  (`numpy`).
- GIL discipline: hold the GIL only around interpreter calls, marshal
  arguments and build result buffers outside it, measure GIL contention
  under concurrent queries.

### Phase 4: Python parallelism (planned)

Question: can the Python calls of concurrent connections run in
parallel, instead of being serialised by the GIL?

This phase is plugin code and the bundled CPython build only, with no
server edits.

Primary route: free-threaded CPython (PEP 703, `python3.14t`).

Fallback: subinterpreters (PEP 684). Reasons for the ranking:

| Topic | Subinterpreters | Free-threaded CPython |
|---|---|---|
| numpy | Not PEP 684 compliant | Free-threaded wheels are published |
| PyTorch | Not supported, and structurally unlikely to be | No official cp314t wheels yet |
| Unsupported extension | Cannot be loaded | Re-enables the GIL at import: the behaviour of today |
| Plugin machinery | Interpreter pool, plus pinning of selected functions to the main interpreter | Thread-safety review of plugin state |

Free-threading degrades to the current behaviour when an extension does
not support it; subinterpreters fail to load the extension.

Work items:

- Build the bundled CPython with `--disable-gil` as a Containerfile
  build argument. The GIL build remains the default until benchmarked.
- Review the plugin state that the GIL protects today: the function
  catalog cache, cached callables, `inhost` module statics, marshal
  buffers.
- Measure the single-thread overhead of the free-threaded build with the
  Phase 3 harness before making it the default.
- Track PyTorch free-threaded wheels upstream.

### Finding: direct name dispatch needs the parser

`py_eval` and `py_exec` are native functions: each is a
`MariaDB_FUNCTION_PLUGIN` whose plugin name is the SQL function name
(`plugin.cc`). The question is whether a function created by a user at
run time can be called as `SELECT myfunc(3, 4)` instead of
`py_call('myfunc', 3, 4)` without grammar changes. The server has two
registries that dispatch by name. Neither fits.

| Registry | Property | Obstacle |
|---|---|---|
| Native function plugins (`MariaDB_FUNCTION_PLUGIN`) | Keyed by name, static | Lookup of an unknown name calls `my_plugin_lock_by_name(name, MariaDB_FUNCTION_PLUGIN)` (`sql/item_create.cc`): it requires an installed plugin with exactly that name. There is no catch-all resolver. Creating one plugin per function needs `INSTALL PLUGIN` and a SONAME. `native_functions_hash` is built once at start and read without a lock, so changing it at run time is a data race. |
| Loadable UDFs (`udf_hash`) | Dynamic (add and remove at run time under `THR_LOCK_udf`), bound to a symbol | `CREATE FUNCTION ... SONAME` resolves the implementation with `dlsym(dlhandle, name)` (`sql/sql_udf.cc`), and the C entry point does not receive the function name. One generic Python router cannot serve arbitrary names. A route exists through one libffi closure per name inserted directly into `udf_hash`, which bypasses the public SONAME path. It is not pursued. |

Conclusion: direct name dispatch goes through the parser and the stored
routine layer, which is Phase 5. `py_call('name', ...)` is the form
available without grammar changes. Native aggregate function plugins
have static names as well, so `py_agg('name', ...)` has the same shape.

## Server-core band

Everything in this band requires changes to the MariaDB grammar or core.
It is delivered as server patches or through an upstream contribution;
the form is decided when the band starts.

### Phase 5: `CREATE FUNCTION ... LANGUAGE PYTHON`

A SQL grammar layer over the catalog of Phase 1.5.

```sql
CREATE FUNCTION multiply(x INT, y INT) RETURNS INT LANGUAGE PYTHON AS
  return x * y;
```

- The parser writes to the catalog that Phase 1.5 introduced.
- Functions compile once and are called by name: `SELECT multiply(3, 4)`.
- Parameters are declared with SQL types.
- The body is a delimited block, not a string literal (see
  [Code delivery](#code-delivery)).

### Phase 6: `CREATE PROCEDURE ... LANGUAGE PYTHON`

Procedures that call back into SQL.

- Python executes SQL statements on the caller's session.
- `COMMIT` and `ROLLBACK` from Python; SQL that is atomic with the
  calling statement.
- Result sets returned to the client.
- Table-valued functions (one call, N rows). MariaDB has no table-valued
  function API at the UDF or function plugin level, so a function that
  returns a result set needs this phase. Until then the convention is to
  return JSON and expand it with `JSON_TABLE`.

### Phase 7: Triggers and events

Python in `CREATE TRIGGER ... LANGUAGE PYTHON` and in scheduled events.

## Code delivery

The Python body is passed as a SQL string literal. This creates two
problems.

| Problem | Status |
|---|---|
| Indentation. Python is whitespace-sensitive; code indented to match the surrounding SQL raises `IndentationError`. | Solved in Phase 1. The body is dedented before compilation with CPython's `textwrap.dedent` (`dedent_source()` in `item_func_python.cc`). Only the shared leading whitespace is removed, so nested indentation is preserved. |
| Escaping. Quotes in the code collide with the SQL string delimiter. MariaDB has no dollar quoting or heredoc. | Not solved while the body is a string literal. Named functions do not change it. The fix is a delimited body, read as raw text up to a delimiter like a stored routine body, in the grammar of Phase 5. Mitigations are listed in [USAGE.md](USAGE.md). Possible additions before Phase 5: a variant that reads the body from a `.py` file, and a base64-encoded body for programmatic callers. |
