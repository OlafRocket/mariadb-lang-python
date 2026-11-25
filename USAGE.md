# lang_python: Embedded Python for MariaDB

A MariaDB plugin that runs Python inside the server.  Ships with a
bundled Python 3.14 interpreter and numpy.  Version 0.5 runs on an
unmodified MariaDB 12.3 (see [BUILD.md](BUILD.md)).

| SQL function | Purpose |
|---|---|
| `py_eval(expr, type, ...)` | Evaluate one Python expression |
| `py_exec(code, type, ...)` | Execute a Python code block |
| `py_create_function`, `py_replace_function`, `py_drop_function` | Manage named, persistent Python functions |
| `py_call(name, ...)` | Call a named function once per row |

Inside Python, the built-in `inhost` module gives access to the hosting
server: `inhost.sql.query()`, `inhost.sql.query_sets()`,
`inhost.sql.execute()` and
`inhost.sql.query_array()`.

## Loading the plugin

```sql
INSTALL SONAME 'lang_python';
```

To load the plugin automatically on startup, add to the server
configuration file:

```ini
[mariadbd]
plugin_load_add = lang_python.so
plugin_maturity = experimental
```

Verify the plugin is loaded:

```sql
SELECT * FROM information_schema.PLUGINS WHERE PLUGIN_NAME = 'lang_python';
SHOW PLUGINS;
```

## System variables

| Variable | Type | Default | Scope | Description |
|---|---|---|---|---|
| `lang_python_vector_numpy` | BOOL | ON | GLOBAL | When ON, VECTOR columns are passed to Python as `numpy.ndarray(float32)`.  When OFF, they are passed as raw `bytes`. |
| `lang_python_strict_errors` | BOOL | ON | SESSION + GLOBAL | When ON, Python exceptions and result-type conversion failures abort the query with `ER_UNKNOWN_ERROR`.  When OFF, they emit a MariaDB warning and return SQL NULL.  See [Error handling](#error-handling). |
| `lang_python_db_access` | BOOL | ON | GLOBAL | When OFF, the built-in `inhost.sql` module raises `inhost.sql.Error` instead of running SQL.  See [In-process server access](#in-process-server-access-the-inhost-module). |

```sql
-- Check current value
SHOW VARIABLES LIKE 'lang_python%';

-- Change at runtime
SET GLOBAL lang_python_vector_numpy = OFF;
```

## SQL functions

### py_eval(expr, type [, arg0, arg1, ...])

Evaluates a single Python expression and returns its value.

```sql
SELECT py_eval('2 ** 32', 'INT');
-- 4294967296

SELECT py_eval('args[0] * args[1]', 'DOUBLE', price, quantity)
  FROM orders;

SELECT py_eval('"hello " + args[0].upper()', 'VARCHAR', name)
  FROM users;
```

### py_exec(code, type [, arg0, arg1, ...])

Executes a Python code block.  The block must assign its answer to
a variable named `result`.  If `result` is absent or `None`, SQL NULL
is returned.

```sql
SELECT py_exec('result = 2 ** 32', 'INT');
-- 4294967296

SELECT py_exec('
import math
result = math.sqrt(args[0] ** 2 + args[1] ** 2)
', 'DOUBLE', side_a, side_b);

SELECT py_exec('
import json
data = json.loads(args[0])
result = data["name"]
', 'VARCHAR', json_column)
  FROM documents;
```

### Which form to use

| Form | Compiles | Use for |
|---|---|---|
| `py_eval` | On every call | A one-off expression |
| `py_exec` | Once per statement; the following rows reuse the code | An inline block, also when evaluated per row |
| Named function + `py_call` | Once, when created | Code that is reused across statements |

Per row, `py_eval` costs about four times as much as `py_exec` or
`py_call`, because it parses and compiles its expression each time.
Measured figures: [bench/README.md](bench/README.md).

### Named functions: py_create_function / py_replace_function / py_drop_function / py_call

A named Python function is defined once, persisted, and called by name.
The body is compiled once, when the function is created, and cached:

```sql
SELECT py_create_function('multiply', 'INT', 'x,y', 'result = x * y');
SELECT py_call('multiply', 3, 4);            -- 12
SELECT py_call('multiply', price, quantity) FROM orders;
SELECT py_drop_function('multiply');
```

Signatures:

```
py_create_function(name, return_type, param_names, body [, strict_errors])
py_replace_function(name, return_type, param_names, body [, strict_errors])
py_drop_function(name [, if_exists])
py_call(name [, arg0, arg1, ...])
```

- `name`: identifier, max 64 chars of `[A-Za-z0-9_]`.  `py_call` requires
  it as a string constant (the return type is resolved at prepare time).
- `return_type`: same set as `py_eval`: INT, DOUBLE, VARCHAR, DECIMAL,
  BOOL, BLOB (and their aliases).
- `param_names`: a comma-separated list (`'x,y'`) injects call arguments
  as named variables; `NULL` exposes them as the `args` tuple instead.
  In named mode the call-site argument count must match.  The names
  `result`, `args`, and `__builtins__` are reserved (they already have a
  meaning in the call namespace).
- `body`: a code block with the same semantics as `py_exec`: assign
  the answer to `result` (absent or `None` returns SQL NULL).  It is
  compiled unmodified, by design without a hidden `def` wrapper, so a
  top-level `return` is a create-time syntax error.  Auto-dedent applies
  (see below).  Syntax errors are reported by `py_create_function`
  itself.
- `strict_errors`: frozen into the function at create time (default 1);
  it overrides the session `lang_python_strict_errors` for this function.

`py_create_function` errors if the name exists; `py_replace_function`
overwrites.  Functions are stored in the `mysql.lang_python_func` system
table (auto-created on first use) and reloaded+recompiled at server
start, so they survive restarts.  `SELECT * FROM mysql.lang_python_func`
is the catalog listing.  Its `kind` column is `function` for every entry
this version creates; `aggregate` entries belong to plugin versions with
aggregate support and cannot be called here.

### In-process server access (the inhost module)

The built-in `inhost` module is the namespace through which Python bodies
reach the hosting server's services (available in `py_eval`, `py_exec`,
and named-function bodies).  It is by design not a client library:
there is nothing to connect to, because the code already runs inside the
server.  Current members: `inhost.version` (server version string) and
`inhost.sql` (database access); future services (e.g. `inhost.uid`) will be
added as further submodules.

```sql
SELECT py_exec('
import inhost
rows = inhost.sql.query("SELECT id, name FROM test.users WHERE id > %s", (args[0],))
result = ";".join(r[1] for r in rows)
', 'VARCHAR', 100);

SELECT py_eval('__import__("inhost").sql.execute(
    "INSERT INTO test.audit (msg) VALUES (%s)", (args[0],))', 'INT', msg);
```

- `inhost.sql.query(sql, params=None)` returns the result set: a list of
  row tuples that also has a `columns` attribute, the tuple of column
  names (statements without a result set return an empty list).
- `inhost.sql.query_sets(sql, params=None)` returns a list of result
  sets, for statements that return more than one; see
  [Column names and several result sets](#column-names-and-several-result-sets).
- `inhost.sql.execute(sql, params=None)` returns the affected-row count;
  for a statement with a result set, the row count of its first result
  set.
- `inhost.sql.query_array(sql, params=None, nulls='error')` returns a
  dict with one numpy array per column; see
  [Columnar results](#columnar-results-inhostsqlquery_array).
- `%s` placeholders bind values with escaping (`%%` is a
  literal `%`): `None` becomes NULL, bool becomes 1/0, int/float a
  numeric literal, str a quoted string, `bytes` an `x'...'` hex literal.
- `query()` result cells map by column type: integer types map to `int`,
  FLOAT/DOUBLE to `float`, NULL to `None`, binary columns (BLOB,
  VARBINARY, BIT, VECTOR) to `bytes`, everything else (strings, DECIMAL,
  dates/times) to `str`.  DECIMAL keeps its exact digits.
- The statement text and all returned strings are UTF-8, whatever the
  server's default or the column's character set.
- One statement per call; `"SELECT 1; SELECT 2"` is a syntax error.
- SQL failures raise `inhost.sql.Error`, which follows the usual error
  handling (strict aborts the query, lax warns and returns NULL).

Execution context: every call runs in a separate internal session,
never in the session of the calling statement, whether or not the
calling statement reads a table:

- It cannot see the outer statement's uncommitted changes, and its
  writes commit independently (autocommit).  Transactional coupling with
  the calling statement is planned for Phase 6.
- There is **no current database**: always qualify table names
  (`test.t1`).  No session state survives between calls, and the
  caller's user variables and temporary tables are not visible.
- `KILL QUERY` on the calling statement does not reach the internal
  statement: the calling statement ends when the internal statement
  has finished.
- Reading a table the outer statement holds locked exclusively can
  block until timeout.
- Writes from a per-row scalar function are semantically hazardous (the
  optimizer may call the function any number of times, in any order).
  Prefer reads per row and writes in one-shot statements.

`SET GLOBAL lang_python_db_access = OFF` disables the module at runtime.

#### Column names and several result sets

`query()` returns an `inhost.sql.Rows` object.  It is a list of row
tuples and can be used as one everywhere; in addition, `columns` holds
the column names as the server reports them:

```python
rows = inhost.sql.query("SELECT id, name, COUNT(*) OVER () FROM test.users")
rows[0]            # (1, 'alice', 3)
rows.columns       # ('id', 'name', 'COUNT(*) OVER ()')
dicts = [dict(zip(rows.columns, r)) for r in rows]
```

- An expression without an alias is named by its text; duplicate names
  are reported as they are.
- A statement without a result set returns an empty `Rows` with
  `columns == ()`.
- A slice or a copy (`rows[:10]`, `list(rows)`) is a plain list without
  `columns`.

A statement can return several result sets, typically `CALL` of a
stored procedure that runs more than one `SELECT`.  `query()` raises
`inhost.sql.Error` for such a statement, so that no result set is
dropped unnoticed; the statement has run to its end by then.
`query_sets()` returns all of them:

```python
for rows in inhost.sql.query_sets("CALL test.report(%s)", (year,)):
    print(rows.columns, len(rows))
```

| Function | One result set | Several result sets | No result set |
|---|---|---|---|
| `query()` | `Rows` | error (use `query_sets`) | empty `Rows` |
| `query_sets()` | list with one `Rows` | list of `Rows`, in order | `[]` |
| `execute()` | row count | row count of the first | affected rows |
| `query_array()` | dict of arrays | error | `{}` |

The number of result sets is decided by the procedure when it runs (a
`SELECT` inside an `IF`), so `query_sets()` can return lists of
different length for the same statement.

#### Columnar results: inhost.sql.query_array

`query_array` returns a result set as a dict of numpy arrays, one per
column, keyed by column name in select order.  It is the bulk path for
numeric data (ML batches, statistics): the result is read and converted
into column buffers with the GIL released, and no Python object is
created per cell for numeric columns.  The values pass through the text
form of the plugin SQL service, and the whole result is held in memory
before the arrays are built.

```sql
SELECT py_exec('
import inhost, numpy
cols = inhost.sql.query_array(
    "SELECT x1, x2, label FROM test.samples WHERE split = %s", (args[0],))
X = numpy.stack([cols["x1"], cols["x2"]], axis=1)
result = float(X.mean())
', 'DOUBLE', 'train');
```

| SQL type | Array dtype | NULL |
|---|---|---|
| TINYINT, SMALLINT, MEDIUMINT, INT, BIGINT | `int64` (`uint64` if unsigned) | error, or NaN with `nulls='nan'` |
| YEAR | `uint64` | error, or NaN with `nulls='nan'` |
| FLOAT, DOUBLE, DECIMAL | `float64` | NaN |
| VECTOR | `object` array of `bytes` (the `float32` values; `numpy.frombuffer(b, dtype=numpy.float32)`) | `None` |
| anything else | `object` array of `str` / `bytes` | `None` |

- `nulls='nan'` returns an integer column that contains NULL as
  `float64` with NaN; integer columns without NULL keep their dtype.
  Values above 2^53 lose precision in that conversion.
- DECIMAL is returned as `float64` and loses exactness; use `query()`
  for exact DECIMAL values.
- Strings are returned as UTF-8 `str` whatever the column character
  set; binary columns as `bytes`; dates and times as `str`.
- Column names must be unique (use aliases); a statement with more
  than one result set is an error.  A statement without a result set
  returns `{}`.
- Parameters, the separate transaction, the missing current database
  and `lang_python_db_access` work as for `query()`.
- `KILL QUERY` on the calling statement does not stop the extraction.

### Indentation

The code body is **auto-dedented** before it runs: the whitespace prefix
common to every non-blank line is stripped (the same rule as Python's
`textwrap.dedent`).  The Python code can therefore be indented to match
the surrounding SQL without triggering `IndentationError`:

```sql
SELECT py_exec('
    import math
    result = math.sqrt(args[0] ** 2 + args[1] ** 2)
', 'DOUBLE', 3, 4);
-- 5
```

Only the shared leading whitespace is removed, so nested indentation
(loops, `if` blocks, function bodies) is preserved exactly.  Code that is
already flush-left is unaffected.  Inconsistent indentation raises an
error, as in a `.py` file.

### Escaping

The Python body is a SQL string literal, so quotes and backslashes in the
code can collide with SQL's own syntax.  MariaDB has no dollar-quoting or
heredoc, so the body cannot be made fully escape-free while it is a
literal.  The following techniques reduce the escaping to a minimum.
None of them requires a change to the plugin.

**1. Bind the code as a parameter (best for application code).**  When
the Python source comes from an application, pass it as a placeholder.
The driver does the escaping, so quotes and backslashes need no
treatment:

```sql
SELECT py_exec(?, 'INT', col) FROM t;
```

```python
cur.execute("SELECT py_exec(?, 'INT', col) FROM t", (python_source,))
```

**2. Use the quote the Python code avoids.**  With the default `sql_mode`
(`ANSI_QUOTES` off) both `'...'` and `"..."` are valid SQL string
delimiters.  Python idiomatically prefers `"`, so keep the SQL literal
single-quoted and write the Python with double quotes.  The only
character that must then be doubled is a literal apostrophe in text or
comments (`it''s`):

```sql
SELECT py_exec('
    msg = "hello"
    result = f"{msg} world"
', 'VARCHAR');
```

**3. Disable backslash mangling for code with backslashes.**  By default
MariaDB processes C-style backslash escapes in the literal, before Python
sees it: `'\d+'` arrives as `d+` and `'\n'` becomes a real newline, which
silently breaks regexes, Windows paths, and `\t`/`\n` in code.  Either
double every backslash (`\\d+`) or turn the processing off so backslashes
pass through verbatim:

```sql
SET SESSION sql_mode = CONCAT(@@sql_mode, ',NO_BACKSLASH_ESCAPES');
SELECT py_exec('
    import re
    result = re.findall("\d+", args[0])[0]
', 'VARCHAR', 'abc123');
-- 123
```

**4. Encode the body (fallback for generated code).**  For
machine-generated or quote-heavy snippets, pass the code base64- or
hex-encoded; this needs no escaping at all:

```sql
SELECT py_exec(FROM_BASE64('aW1wb3J0IG9zCnJlc3VsdCA9IG9zLm5hbWU='), 'VARCHAR');
```

The encoded form is unreadable for humans, but trivial for a tool to emit
and completely escaping-free.

### Arguments

| Position | Name | Required | Description |
|---|---|---|---|
| 1 | expr / code | Yes | Python expression (py_eval) or code block (py_exec) |
| 2 | type | Yes | Return type (must be a string constant, see below) |
| 3+ | arg0, ... | No | Data arguments, available in Python as the tuple `args` |

Data arguments of any MariaDB type can be passed.  They are available
inside the Python code as `args[0]`, `args[1]`, etc.

### Supported return types

| Type keyword | Aliases | Python result type |
|---|---|---|
| `INT` | `INTEGER` | `int` |
| `DOUBLE` | `FLOAT` | `float` or any numeric |
| `VARCHAR` | `TEXT`, `STRING` | `str` (or any object, converted via `str()`) |
| `BLOB` | | `bytes`, `str`, or any object |
| `DECIMAL` | `NUMERIC` | `str`, `decimal.Decimal`, or any object (converted via `str()`) |
| `BOOL` | `BOOLEAN` | `int` |

The type keyword is case-insensitive.

## Type mapping

### MariaDB to Python (input arguments)

| MariaDB type | Python type | Notes |
|---|---|---|
| INT, BIGINT, etc. | `int` | Unsigned values handled correctly |
| DOUBLE, FLOAT | `float` | |
| DECIMAL, NUMERIC | `decimal.Decimal` | Preserves precision (never through `float`) |
| VARCHAR, TEXT, CHAR | `str` | |
| BLOB, VARBINARY | `bytes` | |
| VECTOR | `numpy.ndarray(float32)` | When `lang_python_vector_numpy = ON` (default) |
| VECTOR | `bytes` | When `lang_python_vector_numpy = OFF` |
| NULL | `None` | |

Numeric literal handling depends on the SQL syntax used:

```sql
-- Bare decimal: decimal.Decimal
SELECT py_eval('type(args[0]).__name__', 'VARCHAR', 2.5);
-- 'Decimal'

-- Scientific notation: float
SELECT py_eval('type(args[0]).__name__', 'VARCHAR', 2.5e0);
-- 'float'

-- Explicit cast: float
SELECT py_eval('type(args[0]).__name__', 'VARCHAR', CAST(2.5 AS DOUBLE));
-- 'float'
```

### Python to MariaDB (return values)

| Target type | Expected Python type | Conversion |
|---|---|---|
| INT | `int`, `float` | `float` is truncated with a warning |
| DOUBLE | `float`, `int`, `decimal.Decimal` | Via `float()` |
| VARCHAR | any | Via `str()` |
| BLOB | `bytes`, `str`, any | Via `str()` then UTF-8 |
| DECIMAL | `decimal.Decimal`, `str`, any | Via `str()`, never through `float`; preserves precision |
| BOOL | `int` | |

## NULL handling

The plugin follows a six-level NULL handling model.  Levels 4 and 6
depend on the `lang_python_strict_errors` session variable (default
ON):

| Level | Condition | strict ON (default) | strict OFF |
|---|---|---|---|
| 1 | Code/expression argument is NULL | SQL NULL (silent) | same |
| 2 | Type argument is NULL or not a constant | Error at prepare time | same |
| 3 | A data argument is NULL | Python sees `None` in `args` | same |
| 4 | Python code raises an exception | Query fails (`ER_UNKNOWN_ERROR`) | SQL NULL + MariaDB warning |
| 5 | `result` variable is absent or `None` (py_exec) | SQL NULL (silent) | same |
| 6 | Result cannot be converted to the declared type | Query fails (`ER_UNKNOWN_ERROR`) | SQL NULL + MariaDB warning |

Switch behaviour per session with `SET SESSION lang_python_strict_errors=OFF`
(see [Error handling](#error-handling) below).

## Execution model

### Namespace isolation

Each call to `py_eval()` or `py_exec()` runs in a fresh, private namespace.
A new `dict` with builtins is created for every call.  No state leaks between
calls, between connections, or between rows:

```sql
-- This does NOT accumulate across rows
SELECT py_exec('
counter = counter + 1  # NameError: counter is not defined
result = counter
', 'INT');
```

To share state across calls, use a Python module's global state
(e.g., write to a module attribute):

```sql
-- First, create a helper module that holds shared state
SELECT py_exec('
import types, sys
mod = types.ModuleType("shared")
mod.counter = 0
sys.modules["shared"] = mod
result = "initialized"
', 'VARCHAR');

-- Then, use it from any subsequent call (even from other connections)
SELECT py_exec('
import shared
shared.counter += 1
result = shared.counter
', 'INT');
-- 1  (increments on every call)
```

The interpreter is shared across all connections, so module-level state
is visible to all threads.  Use `threading.Lock` if concurrent updates
could race:

```sql
SELECT py_exec('
import shared, threading
if not hasattr(shared, "lock"):
    shared.lock = threading.Lock()
with shared.lock:
    shared.counter += 1
    result = shared.counter
', 'INT');
```

### Imports

Standard library modules and installed packages (numpy, etc.) can be imported
normally.  Each call gets a fresh namespace, so every call must import
the modules it uses:

```sql
SELECT py_exec('
import math
result = math.factorial(args[0])
', 'INT', n);
```

The import is fast after the first call because Python caches modules in
`sys.modules` for the lifetime of the interpreter (i.e., until the server
shuts down or the plugin is uninstalled).

### How the optimizer treats a Python call

The server treats every call of the plugin's functions as
non-deterministic and expensive, as it treats a stored function that is
not declared `DETERMINISTIC`:

| Rule | Effect |
|---|---|
| Never a constant | A call with constant arguments is evaluated once per row, not once per statement.  It is not evaluated while a statement is prepared, optimized or explained. |
| No index lookup | `WHERE id = py_eval('3', 'INT')` scans the table and compares per row.  Assign the value to a user variable first (`SET @v = py_eval('3', 'INT')`) when an index lookup is wanted. |
| Expensive | Cheaper conditions of the same `WHERE` clause are tested first where the optimizer can order them. |
| Not pushed into the storage engine | A condition with a Python call is evaluated by the server, never inside an engine index scan. |
| Generated columns | Allowed in `DEFAULT` and in `VIRTUAL` generated columns without an index.  Refused in `STORED` generated columns, indexed virtual columns and `CHECK` constraints. |

The same rules apply to `py_create_function`, `py_replace_function` and
`py_drop_function`.

A VARCHAR result has the collation of the system character set with
coercible derivation, like the result of a built-in string function.

### Thread safety

The Python interpreter is initialized once when the plugin loads and shared
across all connections.  The GIL (Global Interpreter Lock) is acquired per
SQL function call via `PyGILState_Ensure()` and released afterward.
Python calls are therefore serialized, but SQL processing between calls
is fully parallel.  The Python thread state of a connection is created by
the first Python call of a statement and released when the statement
ends; the GIL is not held between calls.

### Error handling

By default, the plugin runs in **strict mode**
(`lang_python_strict_errors=ON`): Python exceptions and result-type
conversion failures abort the query with `ER_UNKNOWN_ERROR`, carrying
the Python exception class and message.

```sql
SELECT py_eval('1/0', 'INT');
-- ERROR HY000: py_eval: ZeroDivisionError: division by zero
```

To select lax mode (a failure becomes SQL NULL plus a warning and the
query continues), set the variable to OFF per session or globally:

```sql
SET SESSION lang_python_strict_errors=OFF;

SELECT py_eval('1/0', 'INT');
-- NULL
SHOW WARNINGS;
-- Warning | 1105 | py_eval: ZeroDivisionError: division by zero
```

Strict mode is recommended for production: silent NULLs are easy to
miss in row-by-row computation, and a failing UDF call usually means
the query is producing wrong results regardless of the NULL.

## Installing additional Python packages

The plugin ships with a bundled Python interpreter.  Install
additional packages into it with pip:

```bash
# The bundled Python is under the plugin directory (path depends on install layout):
<plugindir>/lang_python/bin/python3.14 -m pip install <package>

# Examples:
#   STANDALONE: /usr/lib/plugin/lang_python/bin/python3.14 -m pip install <package>
#   RPM:        /usr/lib64/mysql/plugin/lang_python/bin/python3.14 -m pip install <package>
```

Packages are installed into `lang_python/lib/python3.14/site-packages/` and
are immediately available to SQL functions (the interpreter caches modules
in `sys.modules`, so a server restart may be needed for packages that were
already unsuccessfully imported).

Example (installing `pandas`):

```bash
<plugindir>/lang_python/bin/python3.14 -m pip install pandas
```

```sql
SELECT py_exec('
import pandas as pd
df = pd.DataFrame({"a": [1, 2, 3]})
result = str(df.describe())
', 'VARCHAR');
```

## Container image

A ready-to-use container image is available.  See the `Containerfile` in the
plugin source directory for build instructions.

```bash
# Run
podman run -d --name mdb -p 3306:3306 mariadb-langpython

# Connect
podman exec -it mdb mariadb -h 127.0.0.1 -u root

# Run tests
podman run --rm --user root mariadb-langpython \
  bash -c "cd /usr/mariadb-test && \
           perl mariadb-test-run.pl --suite=lang_python --force"
```
