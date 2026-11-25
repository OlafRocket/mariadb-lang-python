# lang_python Developer Guide

Build prerequisites, CMake options, the server version pin and the
container build are in [BUILD.md](BUILD.md). This page covers the source
layout, the development loop and the tests.

## Source layout

| File | Content |
|---|---|
| `plugin.cc` | `maria_declare_plugin` block: the daemon plugin and one function plugin per SQL function |
| `lang_python.cc`, `lang_python.h` | Interpreter start and stop, system variables, numpy API initialisation |
| `item_func_python.cc`, `.h` | `py_eval`, `py_exec`; return type parsing shared by all functions |
| `item_func_py_named.cc`, `.h` | `py_call` and the management functions (`py_create_function`, `py_replace_function`, `py_drop_function`) |
| `py_func_catalog.cc`, `.h` | Catalog table `mysql.lang_python_func` and its in-memory cache of compiled bodies |
| `py_inhost_module.cc`, `.h` | Built-in Python module `inhost` (`inhost.sql.query`, `query_sets`, `execute`, `query_array`): parameters and Python result objects (`inhost.sql.Rows`) |
| `py_inhost_rowsink.cc`, `.h` | Row sink that executes every `inhost.sql` statement through the plugin SQL service and parses the text result into column buffers |
| `py_columns.cc`, `.h` | Column buffer to numpy array conversion for `query_array` |
| `marshal.cc`, `unmarshal.cc` | SQL value to Python object and back, for scalar calls |
| `py_numpy.h` | The one include for the numpy C API; every source file that uses numpy includes it |
| `py_thread_pin.h`, `py_thread_pin.cc` | `Py_thread_pin`: keeps the Python thread state of a server thread for the duration of a statement |
| `mysql-test/lang_python/` | MTR suite |
| `bench/` | Benchmark harness ([bench/README.md](bench/README.md)) |
| `docs/` | Design notes |

## Design notes

| Document | Topic |
|---|---|
| [ROADMAP.md](ROADMAP.md) | Phases and their status |
| [docs/NAMED_FUNCTIONS.md](docs/NAMED_FUNCTIONS.md) | Named functions and the catalog |
| [docs/BULK_DATA.md](docs/BULK_DATA.md) | `query_array` |

## GIL and thread state

| Rule | Detail |
|---|---|
| GIL per call | Every entry into Python is bracketed by `PyGILState_Ensure` / `PyGILState_Release`. The GIL is not held between calls. |
| Thread state per statement | An item that calls Python owns a `Py_thread_pin`. `pin()` runs before the first `PyGILState_Ensure` of the item and creates the thread state (`PyGILState_Ensure` + `PyEval_SaveThread`); the item's `cleanup()` calls `unpin()`. Without the pin, each `Ensure` / `Release` pair creates and destroys a thread state. |
| One pin per thread | `pin()` is a no-op on a thread that already has a thread state: a second Python item of the statement, or a statement run from Python through `inhost.sql`. |
| Same thread | `pin()` and `unpin()` run on the same thread. The debug build asserts it. |
| New items | A new item class that calls Python per row or per group holds a pin and releases it in `cleanup()`. |
| Names | Names looked up on every call are interned once (`py_name` in `lang_python.h`; `PyFuncEntry::param_objs` for parameter names). Use `PyDict_SetItem` / `PyDict_GetItem` with them, not the C string variants, on per-call paths. |

## Build trees

The plugin is built inside a MariaDB server tree at the pinned commit
(see [BUILD.md](BUILD.md), Repo layout). Two build trees are used:

| Tree | Configuration | Purpose |
|---|---|---|
| Release with debug info | `-DCMAKE_BUILD_TYPE=RelWithDebInfo` | Daily work, timing |
| Debug | `-DCMAKE_BUILD_TYPE=Debug` | Server assertions active; catches misuse of server internals that the other tree accepts silently |

Configure with `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON` to get
`compile_commands.json` for IDE code navigation (VSCode:
`"C_Cpp.default.compileCommands": "<build-dir>/compile_commands.json"`).

Rebuild only the plugin, with an explicit job count:

```bash
cmake --build <build-dir> -j <jobs> --target lang_python
```

## Tests

```bash
cd <build-dir>/mysql-test
perl mariadb-test-run.pl --suite=lang_python --force
perl mariadb-test-run.pl --suite=lang_python --record <test>    # new or changed test
```

Run the suite in both build trees before committing. `--repeat=N
--parallel=2` exposes timing-dependent failures in the tests that use
several connections.

| Test | Covers |
|---|---|
| `py_eval`, `py_exec` | Inline functions; `py_exec` compile cache |
| `arg_types`, `null_handling` | Argument marshaling (including VECTOR to numpy), NULL levels |
| `errors`, `errors_lax` | Strict and lax error handling |
| `settings` | System variables, numpy availability |
| `py_named` | Named functions and the catalog |
| `py_inhost` | `inhost.sql.query` / `query_sets` / `execute`: cell types, column names, parameters, several result sets, session separation |
| `py_query_array`, `py_query_array_threads` | `query_array`: types, NULLs, parameters, errors; concurrent connections |
| `py_optimizer` | Optimizer contract: calls per row with constant arguments, no call in `PREPARE` and `EXPLAIN`, no index lookup, no index condition pushdown, generated columns |
| `py_thread_state` | Statement shapes that create and release the Python thread state: several Python items, subqueries, prepared statements, failing statements, stored procedure loops, nested statements through `inhost.sql` |

Conventions:

- A test that creates a named function ends with
  `DROP TABLE mysql.lang_python_func;` so MTR's check of the `mysql`
  schema passes.
- A test that needs another connection to act while a Python body runs
  has the body insert a marker row first and waits for that row; waiting
  on the processlist alone can act before the body starts.

## Benchmarks

```bash
bench/run.sh -b <release-build-dir> -l before     # on the commit before
bench/run.sh -b <release-build-dir> -l after      # on the change
bench/compare.sh bench/results/before.csv bench/results/after.csv
bench/concurrent.sh -b <release-build-dir> -l after   # throughput by connection count
```

A change made for performance states its before and after figures from
this harness in the commit message. Cases, options and method:
[bench/README.md](bench/README.md).

## Server internals the plugin depends on

The plugin compiles against internal server classes, so a move of the
server pin can break the build or change behaviour without a compile
error. Places to re-check:

| Area | Dependency |
|---|---|
| All SQL functions | `Create_native_func`, `Plugin_function`, `Item_func_hybrid_field_type` |
| `inhost.sql` (`query`, `query_sets`, `execute`, `query_array`) | Plugin SQL service. Beyond its functions: `mysql_real_connect_local()` creating a THD of its own when no THD is current; `MYSQL_RES::fields`; `MYSQL::methods->fetch_lengths` and `read_query_result` (details in `py_inhost_rowsink.cc`) |
| Catalog | Plugin SQL service (`mysql_real_connect_local`) |
