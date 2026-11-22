#!/usr/bin/env bash
# Run the lang_python benchmark cases and print a table of timings.
#
# Usage: bench/run.sh [options] [-- mariadb client options...]
#
#   -b BUILD_DIR  start a server from this MariaDB build tree (through MTR,
#                 with the plugin loaded), run against it, shut it down
#   -n RUNS       repetitions per case (default 5); the median is reported
#   -r ROWS       rows in bench.t (default 1000000)
#   -c REGEX      run only the cases whose name matches
#   -l LABEL      label of this run (default: git short hash, "+" if the
#                 working tree has changes)
#   -o DIR        directory for the CSV (default: bench/results)
#   -k            keep the bench schema and functions afterwards
#
# Without -b the client options after "--" select a running server that has
# the plugin loaded, for example:  bench/run.sh -- -h 127.0.0.1 -u root
#
# Timing is taken inside the server (NOW(6) before and after each
# statement); results never travel to the client.  Output: a table on
# stdout and DIR/LABEL.csv (name,median_s,min_s,max_s,us_per_row,rows,runs).
# Compare two runs with bench/compare.sh.
set -euo pipefail

dir=$(cd "$(dirname "$0")" && pwd)
build= runs=5 rows=1000000 filter=. label= outdir=$dir/results keep=0

while getopts "b:n:r:c:l:o:k" opt; do
    case $opt in
        b) build=$OPTARG ;;
        n) runs=$OPTARG ;;
        r) rows=$OPTARG ;;
        c) filter=$OPTARG ;;
        l) label=$OPTARG ;;
        o) outdir=$OPTARG ;;
        k) keep=1 ;;
        *) sed -n '2,24p' "$0"; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

if [ -z "$label" ]; then
    label=$(git -C "$dir" rev-parse --short HEAD 2>/dev/null || echo run)
    git -C "$dir" diff --quiet HEAD 2>/dev/null || label="$label+"
fi

client=mariadb
client_args=("$@")
if [ -n "$build" ]; then
    client=$build/client/mariadb
    sock=$build/mysql-test/var/tmp/mysqld.1.sock
    (cd "$build/mysql-test" &&
     perl mariadb-test-run.pl --suite=lang_python --start-and-exit py_eval) \
        > /dev/null
    client_args=(--no-defaults -S "$sock" -u root)
    trap '"$build/client/mariadb-admin" --no-defaults -S "$sock" -u root shutdown' EXIT
fi
sql() { "$client" "${client_args[@]}" --batch "$@"; }

# Expand cases.sql: wrap every selected case with timing, repeated $runs times
script=$(mktemp)
awk -v runs="$runs" -v filter="$filter" '
    function flush(   i) {
        if (name == "" || name !~ filter) return
        for (i = 1; i <= runs; i++) {
            print "SET @t= NOW(6);"
            printf "%s", body
            printf "INSERT INTO bench.results (name, run, seconds, row_count) VALUES (\x27%s\x27, %d, ", name, i
            printf "TIMESTAMPDIFF(MICROSECOND, @t, NOW(6)) / 1e6, %s);\n", nrows
        }
    }
    /^-- case: / { flush(); name= $3; nrows= $4; body= ""; next }
    /^--/        { next }
    name != ""   { body= body $0 "\n" }
    END          { flush() }
' "$dir/cases.sql" > "$script"

{
    echo "SET @rows= $rows;"
    cat "$dir/setup.sql"
    echo "SET @rows_small= GREATEST(@rows DIV 10, 1);"
    echo "SET @rows_tiny= GREATEST(@rows DIV 100, 1);"
    cat "$script"
} | sql > /dev/null
rm -f "$script"

mkdir -p "$outdir"
csv=$outdir/$label.csv
echo "name,median_s,min_s,max_s,us_per_row,rows,runs" > "$csv"
sql --skip-column-names -e "
    SELECT CONCAT_WS(',', name, ROUND(med, 4), ROUND(MIN(seconds), 4),
                     ROUND(MAX(seconds), 4),
                     ROUND(med * 1e6 / MAX(row_count), 3),
                     MAX(row_count), COUNT(*))
    FROM (SELECT name, seconds, row_count,
                 MEDIAN(seconds) OVER (PARTITION BY name) AS med,
                 MIN(seq) OVER (PARTITION BY name) AS first_seq
          FROM bench.results) r
    GROUP BY name, med ORDER BY MIN(first_seq)" >> "$csv"

echo "lang_python benchmark: $label, $(sql --skip-column-names -e 'SELECT VERSION()'), rows=$rows, runs=$runs"
column -s, -t "$csv"
echo "written: $csv"

if [ "$keep" = 0 ]; then
    sql -e "
        SELECT py_drop_function('bench_double', 1);
        SELECT py_drop_function('bench_batch', 1);
        SELECT py_drop_function('bench_chunked', 1);
        SELECT py_drop_function('bench_query', 1);
        SELECT py_drop_function('bench_query_str', 1);
        SELECT py_drop_function('bench_query_array', 1);
        SELECT py_drop_function('bench_point_query', 1);
        DROP SCHEMA bench;" > /dev/null
fi
