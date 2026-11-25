#!/usr/bin/env bash
# Run the per-row Python call from several connections at once and print
# the total throughput per connection count.
#
# Usage: bench/concurrent.sh [options] [-- mariadb client options...]
#
#   -b BUILD_DIR  start a server from this MariaDB build tree (through MTR,
#                 with the plugin loaded), run against it, shut it down
#   -j LIST       connection counts to run (default "1 2 4 8")
#   -n RUNS       repetitions per connection count (default 3); the median
#                 is reported
#   -r ROWS       rows scanned by each connection (default 100000)
#   -l LABEL      label of this run (default: git short hash, "+" if the
#                 working tree has changes)
#   -o DIR        directory for the CSV (default: bench/results)
#
# Without -b the client options after "--" select a running server that has
# the plugin loaded.
#
# Every connection runs SELECT SUM(py_call('bench_double', x)) over ROWS
# rows.  The wall time from the start of the first client to the end of the
# last is measured on the client side.  Output: a table on stdout and
# DIR/LABEL-concurrent.csv (connections,median_s,rows_per_s,us_per_row).
#
# The Python bodies of all connections share the GIL, so the throughput is
# expected to stay flat as connections are added; a drop shows contention
# cost, a rise shows work done outside the GIL.
set -euo pipefail

dir=$(cd "$(dirname "$0")" && pwd)
build= conns="1 2 4 8" runs=3 rows=100000 label= outdir=$dir/results

while getopts "b:j:n:r:l:o:" opt; do
    case $opt in
        b) build=$OPTARG ;;
        j) conns=$OPTARG ;;
        n) runs=$OPTARG ;;
        r) rows=$OPTARG ;;
        l) label=$OPTARG ;;
        o) outdir=$OPTARG ;;
        *) sed -n '2,28p' "$0"; exit 2 ;;
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

# setup.sql puts @rows / 10 rows into bench.t_small
{
    echo "SET @rows= $rows * 10;"
    cat "$dir/setup.sql"
} | sql > /dev/null

stmt="SELECT SUM(py_call('bench_double', x)) INTO @r FROM bench.t_small"
sql -e "$stmt" > /dev/null                      # warm up

mkdir -p "$outdir"
csv=$outdir/$label-concurrent.csv
echo "connections,median_s,rows_per_s,us_per_row" > "$csv"
for n in $conns; do
    times=()
    for _ in $(seq "$runs"); do
        start=$(date +%s.%N)
        for _ in $(seq "$n"); do
            sql -e "$stmt" > /dev/null &
        done
        wait
        times+=("$(echo "$(date +%s.%N) - $start" | bc)")
    done
    median=$(printf '%s\n' "${times[@]}" | sort -n | sed -n "$(( (runs + 1) / 2 ))p")
    awk -v n="$n" -v t="$median" -v rows="$rows" 'BEGIN {
        printf "%d,%.4f,%.0f,%.3f\n", n, t, n * rows / t, t * 1e6 / (n * rows)
    }' >> "$csv"
done

echo "lang_python concurrent benchmark: $label, $(sql --skip-column-names -e 'SELECT VERSION()'), rows=$rows per connection, runs=$runs"
column -s, -t "$csv"
echo "written: $csv"

sql -e "
    SELECT py_drop_function('bench_double', 1);
    SELECT py_drop_function('bench_query', 1);
    SELECT py_drop_function('bench_query_str', 1);
    SELECT py_drop_function('bench_query_array', 1);
    SELECT py_drop_function('bench_point_query', 1);
    DROP SCHEMA bench;" > /dev/null
