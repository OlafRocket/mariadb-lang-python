# lang_python: embedded Python for MariaDB

lang_python is a MariaDB server plugin that runs Python inside the
server process. It bundles a CPython 3.14 interpreter and numpy.

It is a research and development project. The question it investigates:
how far can an embedded language be taken beyond the classic stored
procedure and stored function?

Status: experimental. This is version 0.5: a plugin for an unmodified
MariaDB 12.3 server. It is not intended for production use. See
[LIMITATIONS.md](LIMITATIONS.md).

## Research scope

Stored procedures and functions in an embedded language are well
understood: a named routine, created with DDL, called once per row or
once per statement. This project explores the uses outside that shape.

| Area | Question | Result so far |
|---|---|---|
| Inline code | Can Python appear inside a SQL expression, with no DDL? | `py_eval`, `py_exec` |
| Named functions without grammar changes | How much of `CREATE FUNCTION` can a plugin provide on an unmodified server? | `py_create_function`, `py_call`: persistent, compiled once. Calling a function directly by its name needs the parser. |
| In-process server access | Can Python query the server it runs in, with no client connection? | The built-in `inhost` module on the plugin SQL service; `inhost.sql.query_array()` returns one numpy array per column |
| Numerical and ML workloads | Can VECTOR arguments and bulk numeric data reach numpy without a Python object per cell? | VECTOR arguments as `float32` arrays; column buffers filled with the GIL released |
| Aggregates and window functions | Can Python receive a whole group of rows as numpy arrays? | Not in this version: needs native aggregate function plugins, which MariaDB 12.3 does not have |
| Cost | What does a Python call cost per row? | About 10 µs per row for a scalar call against 0.13 µs for native SQL; measured with the harness in [bench/](bench/README.md) |
| Parallelism | Can Python calls of concurrent connections run in parallel? | Planned: free-threaded CPython |
| Language integration | `CREATE FUNCTION / PROCEDURE / TRIGGER ... LANGUAGE PYTHON` | Later: needs server grammar changes |

The first phases are restricted to an unmodified server on purpose:
they measure how much the existing plugin interfaces allow, and they
identify which features need a server change and why. The questions,
results and rejected alternatives per phase are in
[ROADMAP.md](ROADMAP.md).

## Examples

Inline code:

```sql
SELECT py_eval('args[0] * args[1]', 'DOUBLE', price, quantity) FROM orders;

SELECT py_exec('
import json
result = json.loads(args[0])["name"]
', 'VARCHAR', json_column) FROM documents;
```

A named function, compiled once and stored in the server:

```sql
SELECT py_create_function('multiply', 'INT', 'x,y', 'result = x * y');
SELECT py_call('multiply', price, quantity) FROM orders;
```

A query from inside Python, returned as numpy arrays:

```sql
SELECT py_exec('
import inhost, numpy
cols = inhost.sql.query_array(
    "SELECT x1, x2 FROM test.samples WHERE split = %s", (args[0],))
X = numpy.stack([cols["x1"], cols["x2"]], axis=1)
result = float(X.mean())
', 'DOUBLE', 'train');
```

More examples, including triggers that call external services, are in
[examples/](examples/README.md).

## SQL interface

| SQL function | Purpose |
|---|---|
| `py_eval(expr, type, ...)` | Evaluate one Python expression |
| `py_exec(code, type, ...)` | Execute a Python code block |
| `py_create_function`, `py_replace_function`, `py_drop_function` | Manage named, persistent Python functions |
| `py_call(name, ...)` | Call a named function once per row |

| Python module | Purpose |
|---|---|
| `inhost.sql.query()`, `query_sets()` | Run a statement in the hosting server, return rows |
| `inhost.sql.query_array()` | Run a statement, return one numpy array per column |
| `inhost.sql.execute()` | Run a statement, return the affected row count |

Reference: [USAGE.md](USAGE.md).

## Requirements

| Item | Requirement |
|---|---|
| Server | MariaDB 12.3 at the release pinned in the [Containerfile](Containerfile). No server changes. The plugin uses internal server classes; it is built for one exact server version. |
| Python | CPython 3.14 and numpy, downloaded and built by the plugin's CMake. No system Python is used. |
| Platform | Linux |

## Quick start

Build the container image (server and plugin) and start it:

```bash
podman build -t mariadb-langpython .
podman run -d --name mdb -p 3306:3306 mariadb-langpython
podman exec -it mdb mariadb -h 127.0.0.1 -u root
```

```sql
SELECT py_eval('2 ** 32', 'INT');
```

The client runs inside the container: the image has only the account
`root@localhost` without a password, so a connection through the
published port is refused until an account for other hosts is created.

Building inside a MariaDB source tree, CMake options and the test suite
are described in [BUILD.md](BUILD.md).

## Security

The plugin has no sandbox. Python code runs inside the `mariadbd`
process with the privileges of the server user, including file system
and network access, and a crash in Python or in a C extension stops the
server. SQL issued through `inhost.sql` runs without grant checks. Any
account that can execute the `py_*` functions can run arbitrary code.
Install the plugin only on servers where every such account is trusted.
Details: [LIMITATIONS.md](LIMITATIONS.md).

## Documentation

| Document | Content |
|---|---|
| [USAGE.md](USAGE.md) | SQL functions, the `inhost` module, type mapping, error handling |
| [LIMITATIONS.md](LIMITATIONS.md) | What the plugin does not do, and why |
| [ROADMAP.md](ROADMAP.md) | Research questions, results, rejected alternatives, next phases |
| [BUILD.md](BUILD.md) | Container build, local build, server version pin |
| [DEVELOPER.md](DEVELOPER.md) | Source layout, development loop, tests |
| [bench/README.md](bench/README.md) | Benchmark harness and cases |
| [docs/NAMED_FUNCTIONS.md](docs/NAMED_FUNCTIONS.md) | Design: named functions and the catalog |
| [docs/BULK_DATA.md](docs/BULK_DATA.md) | Design: `query_array` |

## License

GPL version 2. See [COPYING](COPYING).
