# lang_python: Current Limitations

The current limitations of the plugin and their causes.  Each item
notes the roadmap phase that addresses it where applicable (see
[ROADMAP.md](ROADMAP.md)).  General items cover the inline functions
(`py_eval` / `py_exec`); named functions and in-process DB access have
their own sections below.

## Server version

- **Requires MariaDB `main` (13.3, alpha) at the pinned commit** (see
  [BUILD.md](BUILD.md)).  No released (GA) server is supported.
- **Bound to the exact server build.**  Function plugins carry
  `MariaDB_FUNCTION_INTERFACE_VERSION` (`MYSQL_VERSION_ID << 8`) and use
  internal server C++ classes, so the plugin is rebuilt for every server
  version.

## Concurrency & performance

- **Python execution is globally serialized by the GIL.**  The GIL is
  acquired per call (`PyGILState_Ensure`) around every `py_eval`/`py_exec`.
  SQL processing between calls runs in parallel, but the Python portion of
  all connections passes through one lock and does not scale across cores.
  Addressed by Phase 4 (free-threaded CPython; subinterpreters are the
  fallback, limited by numpy not supporting PEP 684).

- **A runaway Python call blocks the whole server's Python, not just its own
  connection.**  Because of the single GIL, an infinite loop or a
  long-running `py_exec` holds the GIL and stalls every other connection's
  Python calls.  There is no execution timeout, no memory cap, and no way
  to interrupt a call.

- **`py_eval` compiles its expression on every call**, so per-row use
  pays the parse and compile cost for every row (about 40 µs per row
  measured, against about 10 µs for `py_exec` and `py_call`).  This is
  deliberate: `py_eval` is the one-off form.  `py_exec` compiles once per
  statement and reuses the code for the following rows; named functions
  compile once at create time.  For per-row work use `py_exec` or a
  named function.

- **The `py_exec` compile cache is per statement.**  Every new statement
  compiles its block once again; there is no cache shared between
  statements or connections.

- **One shared interpreter across all connections.**  Module-level state
  (anything in `sys.modules` / module globals) is process-wide and visible to
  all threads, so shared state can race; user code must add its own
  `threading.Lock`.  Each call gets a fresh namespace dict (no per-call
  leakage), but there is no connection-scoped state.

## Functionality

- **Scalar-only, single value per row.**  `py_eval`/`py_exec` are scalar
  functions: one scalar result per row.  No table-valued functions, no result
  sets, no multi-column returns.  Aggregation is available only through
  `py_agg` (see Aggregates below).
  True table-valued results need Phase 6 (only the JSON + `JSON_TABLE`
  convention before then).

- **SQL from Python runs outside the calling statement.**  `inhost.sql`
  (Phase 1.6) reads and writes through a separate internal connection; see
  In-process DB access below.  SQL atomic with the outer statement is
  addressed by Phase 6.

- **No SQL DDL for functions.**  Named functions are created with
  `py_create_function` (Phase 1.5) and called through `py_call('name', ...)`.
  `SELECT name(...)` and `CREATE FUNCTION ... LANGUAGE PYTHON` are
  addressed by Phase 5.

- **Limited, fixed return types**: INT, DOUBLE, VARCHAR, BLOB, DECIMAL, BOOL.
  The return type must be a compile-time string constant (`args[1]` must
  be constant) and cannot be computed at runtime.  Inline `py_eval`/`py_exec`
  arguments are positional only (`args[0]`, `args[1]`, ...); named parameters
  exist for named functions (`param_names`, Phase 1.5).

- **Coarse error reporting.**  Python exceptions surface as a generic
  `ER_UNKNOWN_ERROR` carrying the exception text; there is no fine-grained
  SQLSTATE mapping.  Behavior is governed by the strict/lax toggle
  (`lang_python_strict_errors`).

## Code delivery

- **Escaping is not fully solvable while the body is a SQL string literal**
  (quotes/backslashes collide with SQL syntax; MariaDB has no dollar-quoting
  or heredoc).  Only mitigations exist: parameter binding, quote
  choice, `NO_BACKSLASH_ESCAPES`, base64 (see the Escaping section in
  [USAGE.md](USAGE.md)).  A structural fix comes only with a delimited DDL
  body in Phase 5.  Indentation, by contrast, is solved via auto-dedent.

## Named functions (Phase 1.5)

- **Catalog changes are not replicated.**  `py_create_function` /
  `py_create_aggregate` / `py_drop_function` run inside a `SELECT`
  (never binlogged), and the internal connection writing
  `mysql.lang_python_func` runs with `sql_log_bin=0`.  On a replicated
  topology, run the management calls on every server (or replicate the
  catalog table out of band).

- **No dedicated privilege model.**  Anyone who can execute the functions
  can create/replace/drop named functions and aggregates; the catalog
  table itself is written by an internal grants-free connection, so
  `INSERT`/`DELETE` grants on `mysql.lang_python_func` are not consulted.

- **`py_call`'s function name must be a string constant.**  It is resolved
  at prepare time because the catalog entry determines the statement's
  return type.  A statement keeps the entry it resolved: a concurrent
  `py_replace_function` affects the next statement, not one in flight,
  which makes drop/replace safe under load.

## In-process DB access (Phase 1.6)

- **Every `inhost.sql` call (`query`, `execute`, `query_array`) is its
  own session and transaction.**  No visibility into the outer
  statement's uncommitted changes, independent autocommit writes, no
  session state between calls (temp tables, user variables,
  BEGIN/COMMIT), no access to the caller's user variables, no current
  database (qualify table names).  Atomic-with-the-caller SQL is Phase 6;
  the cost of a new internal THD per call is a Phase 3 (reuse) item.

- **One statement per call.**  Multi-statement text
  (`SELECT 1; SELECT 2`) is a syntax error.  Of several result sets
  (`CALL`), `query()` and `query_array()` raise an error,
  `query_sets()` returns all, and `execute()` counts the first and
  discards the rest.  The error is raised when the second result set
  starts: the procedure stops there, and changes it made before that
  point can already be committed.

- **`query_sets()` holds every result set in memory** until the
  statement ends; there is no streaming across sets.

- **`columns` is lost on a slice or copy** of a `query()` result
  (`rows[:10]`, `list(rows)` are plain lists).  Rows are plain tuples;
  there is no access to a cell by column name.

- **Internal SQL runs grants-free**, like the catalog writes: there is
  no per-user credential model for the internal connection.  The
  `lang_python_db_access` global is the only control (ON by default).

- **Deadlock/timeout risk against the outer statement's locks.**  An
  internal query touching a table the calling statement has locked
  exclusively waits until lock timeout.

- **`inhost.sql` depends on server internals beyond the plugin SQL
  service** (a private THD and a `Protocol` subclass; see
  `docs/BULK_DATA.md` §2).  The internal statement is invisible to
  `KILL`; a kill of the calling statement is forwarded only when a row
  is produced, so a long scan that returns no rows, or a long write, is
  not interrupted.

- **The built-in `inhost` module shadows any pip-installed package named
  `inhost`** in the bundled interpreter (built-in modules win over
  site-packages).  The real PyPI `mariadb` connector is unaffected and
  can be pip-installed normally.

## Aggregates (Phase 2)

- **`py_agg` with a function buffers a whole group in memory** before
  calling Python, bounded by `lang_python_agg_max_mem`.  Larger groups
  need a Python aggregate (`py_create_aggregate`), which receives the
  group in chunks.
- **A Python aggregate must be written to be independent of chunk
  boundaries, and its `finalize` must be repeatable.**  The plugin cannot
  check either; a class that violates them returns results that vary
  with `lang_python_agg_chunk_rows` or with the window frame type.
- **Memory held by a Python aggregate's own state is not limited**;
  `lang_python_agg_max_mem` covers the argument buffers only.
- **No incremental window frames** (`inverse`): see the frame item below.
- **No parallel aggregation** (`merge`).
- **A NULL in an integer or VECTOR argument is an error**; there is no
  per-function option to map it.
- **Window frames are recomputed in full** for every output row
  (`supports_removal()` is false): quadratic cost on large frames.
- **Row order inside a group is undefined**, also under `DISTINCT`.
- **The Python call of a group is not interruptible by `KILL`**; the
  row scan before it is.
- **DECIMAL results are declared as `DECIMAL(64,38)`** in
  `CREATE TABLE ... SELECT`, since the function declares no precision.

## Safety & operations

- **No sandboxing: arbitrary code execution as the server user.**  Any
  caller of `py_eval`/`py_exec` runs arbitrary Python in the `mariadbd`
  process with its privileges (filesystem, network, etc.).  There is no
  per-function privilege model or capability restriction beyond controlling
  who can call the functions at all.  In untrusted environments this is
  the main operational constraint.

- **In-process embedding means the crash blast radius is the whole server.**
  A segfault in Python or a buggy C extension takes down `mariadbd`, not just
  the connection.

- **The incompatibility of numpy with subinterpreters** limits the
  subinterpreter fallback of Phase 4 (the primary route is free-threaded
  CPython).

- **Package management is interpreter-global.**  `pip install` goes into the
  one bundled interpreter; the numpy version drifts on clean rebuilds, and
  some package changes require a server restart to take effect.
