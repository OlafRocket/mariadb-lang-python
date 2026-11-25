-- Benchmark cases.
--
-- Format: a line "-- case: NAME ROWS" starts a case; the SQL up to the next
-- case line is one statement that stores its answer with INTO @r (nothing
-- is sent to the client).  ROWS is an SQL expression giving the number of
-- rows the case processes (@rows, @rows_small, @rows_tiny); it is used for
-- the per-row time.  run.sh wraps every case with timing and repeats it.
--
-- Naming: native_* are plain SQL baselines for the Python case of the same
-- shape.

-- ---- one Python call per row ---------------------------------------------

-- case: native_per_row @rows_small
SELECT SUM(x * 2.0) INTO @r FROM bench.t_small;

-- case: py_eval_per_row @rows_small
SELECT SUM(py_eval('args[0] * 2.0', 'DOUBLE', x)) INTO @r FROM bench.t_small;

-- case: py_exec_per_row @rows_small
SELECT SUM(py_exec('result = args[0] * 2.0', 'DOUBLE', x)) INTO @r FROM bench.t_small;

-- case: py_exec_per_row_block @rows_small
SELECT SUM(py_exec('
    import math
    v = args[0]
    if v > 0:
        result = math.sqrt(v) * 2.0
    else:
        result = 0.0
', 'DOUBLE', x)) INTO @r FROM bench.t_small;

-- case: py_call_per_row @rows_small
SELECT SUM(py_call('bench_double', x)) INTO @r FROM bench.t_small;

-- ---- native aggregate baseline ---------------------------------------------

-- case: native_aggregate @rows
SELECT SUM(x * 2.0) INTO @r FROM bench.t;

-- ---- SQL from Python ------------------------------------------------------

-- case: inhost_query_numeric @rows
SELECT py_call('bench_query') INTO @r;

-- case: inhost_query_strings @rows
SELECT py_call('bench_query_str') INTO @r;

-- case: inhost_query_array @rows
SELECT py_call('bench_query_array') INTO @r;

-- case: inhost_point_query_per_row @rows_tiny
SELECT SUM(py_call('bench_point_query', id)) INTO @r FROM bench.t_tiny;
