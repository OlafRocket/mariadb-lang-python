# lang_python benchmark harness

Measures the plugin's execution paths with repeatable timings, so a
change can be compared against the commit before it.

## Files

| File | Content |
|---|---|
| `run.sh` | Runner: creates the fixtures, runs every case several times, prints a table, writes a CSV |
| `cases.sql` | The cases, one SQL statement each |
| `setup.sql` | Fixtures: schema `bench`, three tables, the named functions the cases call |
| `compare.sh` | Compares the CSVs of two runs |
| `results/` | CSVs written by `run.sh` (not tracked by git; timings are machine-specific) |

## Running

Against a server started from a build tree (the runner starts it through
MTR with the plugin loaded, and shuts it down afterwards):

```bash
bench/run.sh -b <build-dir>
```

Against a running server that has the plugin loaded:

```bash
bench/run.sh -- -h 127.0.0.1 -u root
```

| Option | Meaning | Default |
|---|---|---|
| `-b DIR` | MariaDB build tree to start a server from | none: use the client options after `--` |
| `-n RUNS` | Repetitions per case | 5 |
| `-r ROWS` | Rows in `bench.t` | 1000000 |
| `-c REGEX` | Run only the cases whose name matches | all |
| `-l LABEL` | Label of the run, used as CSV file name | git short hash, `+` appended when the tree has changes |
| `-o DIR` | Directory for the CSV | `bench/results` |
| `-k` | Keep schema `bench` and the `bench_*` functions | drop them |

Use a release build tree (`RelWithDebInfo`), not the debug tree, and an
otherwise idle machine.

## Comparing two commits

```bash
git checkout <before>; <build>; bench/run.sh -b <build-dir> -l before
git checkout <after>;  <build>; bench/run.sh -b <build-dir> -l after
bench/compare.sh bench/results/before.csv bench/results/after.csv
```

`speedup` is before / after: above 1 means the second run is faster.
Differences of a few percent between runs of the same code are noise;
the `min_s` and `max_s` columns show the spread within a run.

## Method

- Time is taken inside the server: `NOW(6)` before and after each
  statement. Every case stores its answer with `INTO @r`, so no result
  travels to the client.
- The reported time is the median of the runs. `us_per_row` is the
  median divided by the rows the case processes.
- Row counts per case kind, so one run stays in the range of minutes:

  | Table | Rows | Used by |
  |---|---|---|
  | `bench.t` | `ROWS` | Batch paths: `py_agg`, `query`, `query_array` |
  | `bench.t_small` | `ROWS / 10` | One Python call per row |
  | `bench.t_tiny` | `ROWS / 100` | One internal SQL statement per row |

- `native_*` cases are plain SQL doing the same arithmetic, as a
  baseline for the Python case of the same shape.

## Cases

| Case | Measures |
|---|---|
| `native_per_row`, `native_aggregate` | Plain SQL baselines |
| `py_eval_per_row` | `py_eval` evaluated per row: compiles every call |
| `py_exec_per_row`, `py_exec_per_row_block` | `py_exec` per row, one-line and multi-line body: compile cache |
| `py_call_per_row` | Named function per row |
| `py_agg_function`, `py_agg_function_grouped` | `py_agg` with a function, one group and 1000 groups |
| `py_agg_aggregate` | `py_agg` with a Python aggregate (chunked) |
| `inhost_query_numeric`, `inhost_query_strings` | `inhost.sql.query()` returning all rows |
| `inhost_query_array` | `inhost.sql.query_array()` |
| `inhost_point_query_per_row` | One indexed `inhost.sql.query()` per row: cost of an internal session per call |

## Adding a case

Add to `cases.sql`:

```sql
-- case: NAME ROWS_EXPRESSION
SELECT ... INTO @r FROM ...;
```

`ROWS_EXPRESSION` is `@rows`, `@rows_small` or `@rows_tiny`, matching
the table the statement reads. A named function the case needs goes into
`setup.sql` with a `bench_` prefix, and its drop into the cleanup list at
the end of `run.sh`.
