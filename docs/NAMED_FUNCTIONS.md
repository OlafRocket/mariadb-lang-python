# Named Python functions: design (Phase 1.5)

Status: implemented.

- Catalog SQL runs through the plugin SQL service
  (`mysql_real_connect_local`); `Ed_connection` is not used because
  `Protocol_local` is private to `sql_prepare.cc`.
- Catalog boot is lazy with retry, because the plugin initialises
  before Aria.
- Catalog writes are not replicated: they run on an internal connection
  with `sql_log_bin=0`.
- `py_replace_function` uses `REPLACE INTO`, which resets the `created`
  column.
- The body compiles unmodified and is not wrapped in a `def` (§6).
- Implementation: `plugin/lang_python/` (`py_func_catalog.cc`,
  `item_func_py_named.cc`); test: the `py_named` MTR test.

Scope: Phase 1.5 of the [roadmap](../ROADMAP.md): persistent, named
Python functions with no SQL grammar changes, as a step towards
Phase 5 (`CREATE FUNCTION ... LANGUAGE PYTHON`).

---

## 1. Goal & non-goals

### Goal

A Python function is defined once, persisted, and called by name from
SQL.  All of this goes through management functions, with no parser or
server-code changes:

```sql
SELECT py_create_function('multiply', 'INT', 'x,y', 'result = x * y');
SELECT py_call('multiply', 3, 4);      -- 12
```

This delivers the substance of Phase 5 (named functions, compile-once
cache, persistence) entirely in plugin code.  Phase 5 is a syntax layer
that writes to the same catalog this phase introduces.

### Non-goals

| Non-goal | Reason |
|---|---|
| Direct name dispatch (`SELECT multiply(3,4)`) | Not cleanly possible without grammar changes; see the ROADMAP section "Function registration: why direct name-dispatch is Phase 5".  `py_call('name', ...)` is the interim form |
| Calling back into SQL from the function body | Phase 1.6 / Phase 6 |
| Parallel execution, subinterpreters | Phase 4 |

---

## 2. Decisions

| Decision | Choice | Reason |
|---|---|---|
| Calling convention | `py_call('name', args...)` | No grammar change; direct call is Phase 5 |
| Parameter style | Both: named locals or `args[]` | Selected per catalog row by `param_names` (NULL selects `args[]`) |
| Catalog location | `mysql.lang_python_func` | The `lang_*` prefix leaves room for sibling language plugins; mysqldump and backup cover the table without extra work |
| Redefining | Separate `py_replace_function` | `py_create_function` fails on an existing name, so a function is not overwritten by accident |
| Catalog creation | Automatic, at catalog boot (lazy with retry) | `CREATE TABLE IF NOT EXISTS` through the plugin SQL service; no installation step |
| Validation | At create time | `Py_CompileString` runs synchronously in `py_create_function` |
| Listing | None; `SELECT * FROM mysql.lang_python_func` | A `py_list_functions` function is not needed |
| Replication | Catalog writes are not replicated | They run on an internal connection with `sql_log_bin=0` |
| Privileges | No dedicated model | Any account that can execute the management functions can create, replace and drop functions. The catalog is written by an internal connection without grant checks |
| Schema migration | Not implemented (§7) | Planned mechanism: `information_schema` probe plus `ALTER TABLE` |

---

## 3. SQL surface

```sql
-- create (errors if 'name' already exists)
py_create_function(name, return_type, param_names, body [, strict_errors [, deterministic]])
-- replace (create-or-overwrite)
py_replace_function(name, return_type, param_names, body [, strict_errors [, deterministic]])
-- drop (errors if absent; py_drop_function(name, /*if_exists=*/1) tolerant)
py_drop_function(name)
-- call
py_call(name, arg0, arg1, ...)
```

| Argument | Meaning |
|---|---|
| `return_type` | One of `INT`, `DOUBLE`, `VARCHAR`, `BLOB`, `DECIMAL`, `BOOL` (same set as `py_eval`/`py_exec`) |
| `param_names` | Comma-separated (`'x,y'`) to bind the arguments as named locals, or `NULL` to expose them as `args[0], args[1], ...` |
| `body` | Python source; dedented with `dedent_source()` at create time, as on the inline path.  The body is a SQL string literal and subject to SQL string escaping until Phase 5 adopts a delimited body |
| `strict_errors` | Stored in the catalog row at create time |
| `deterministic` | Stored in the catalog row at create time (column `is_deterministic`; `DETERMINISTIC` is a reserved word). Declares the function deterministic to the optimizer; not checked |

All four functions are registered the same way as `py_eval`/`py_exec`:
as `MariaDB_FUNCTION_PLUGIN` entries (plugin name == SQL function name)
in `plugin.cc`.

---

## 4. Catalog schema

```sql
CREATE TABLE IF NOT EXISTS mysql.lang_python_func (
  name          VARCHAR(64)   NOT NULL PRIMARY KEY,
  body          TEXT          NOT NULL,
  return_type   VARCHAR(16)   NOT NULL,    -- INT|DOUBLE|VARCHAR|BLOB|DECIMAL|BOOL
  param_names   VARCHAR(1024) NULL,        -- comma-separated; NULL = args[] mode
  comment       VARCHAR(255)  NULL,
  definer       VARCHAR(384)  NULL,
  strict_errors TINYINT(1)    NOT NULL DEFAULT 1,
  kind          ENUM('function','aggregate') NOT NULL DEFAULT 'function',
                                           -- see AGGREGATE_PROTOCOL.md
  is_deterministic TINYINT(1) NOT NULL DEFAULT 0,
  created       TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  modified      TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP
                  ON UPDATE CURRENT_TIMESTAMP
) ENGINE=Aria TRANSACTIONAL=1;
```

Aria with `TRANSACTIONAL=1`: crash-safe, no InnoDB dependency for a
system table, consistent with other `mysql.*` system tables.

---

## 5. In-memory cache

```cpp
struct PyFuncEntry {
  std::string              name;
  PyCodeObject            *code;          // strong ref, immutable once set
  TypeHandler              return_type;
  std::vector<std::string> param_names;   // empty: args[] mode
  bool                     strict_errors;
};
std::unordered_map<std::string, std::shared_ptr<const PyFuncEntry>> cache;
mysql_rwlock_t cache_lock;
```

### Lifecycle

| Event | Steps |
|---|---|
| Catalog boot (lazy with retry) | Create the catalog if absent, then scan every row, compile it with `Py_CompileString` into a `PyCodeObject`, and publish it into `cache` |
| `py_create_function` | Validate and compile, `INSERT`, publish under the write lock.  Fails if the name exists |
| `py_replace_function` | Validate and compile, `REPLACE INTO` (resets `created`), replace under the write lock |
| `py_drop_function` | `DELETE`, erase under the write lock.  In-flight callers keep their own `shared_ptr`, so a concurrent drop never frees code that a running call uses |
| `py_call` | Hash lookup under the read lock, copy the `shared_ptr`, release the lock, then `PyEval_EvalCode`.  The lock is held for nanoseconds and never across user Python |

The `shared_ptr<const PyFuncEntry>` makes execution lock-free: lookup
is the only locked step, and evaluation runs on a private reference.

---

## 6. Execution

`py_call` mirrors the inline path
([item_func_python.cc](../item_func_python.cc)):

1. Look up the entry (§5).
2. Build a fresh globals dict with `__builtins__`.
3. Marshal each SQL argument to Python: if `param_names` is set, bind
   by name into globals; otherwise build an `args` list/tuple.
4. `PyEval_EvalCode(entry->code, globals, globals)`.
5. Marshal the result back to `return_type`; apply
   `entry->strict_errors`.

Body semantics: the body compiles unmodified, with the
`result`-variable convention of `py_exec`, and the named parameters are
injected as globals.  The body is deliberately not wrapped in a `def`;
a top-level `return` is therefore a `SyntaxError` at create time
(`'result = x * y'`, not `'return x * y'`).

GIL handling and exception formatting are identical to
`py_eval`/`py_exec`; the helpers are shared, not duplicated.

---

## 7. Open items

| Item | State |
|---|---|
| Per-row recompile versus cache for the inline path | Handled by Phase 3 |
| `comment` / `definer` semantics | The columns are reserved; they are not populated |
| Schema migration | Not implemented; planned mechanism: `information_schema` probe plus `ALTER TABLE` |
| `py_call` overhead | Name lookup plus marshaling per row; to be examined in Phase 3 (the performance pass) |

---

## 8. Relationship to later phases

- Phase 1.6 (in-process DB access) and Phase 6 (procedures) add SQL
  callbacks; bodies in this phase are pure compute.
- Phase 5 parses `CREATE FUNCTION ... LANGUAGE PYTHON` and writes the
  same `mysql.lang_python_func` rows, then dispatches by name through
  the stored-routine layer.  The cache, catalog and marshaling defined
  here are reused unchanged.
