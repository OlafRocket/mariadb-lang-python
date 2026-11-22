#!/usr/bin/env bash
# Compare two benchmark runs written by bench/run.sh.
#
# Usage: bench/compare.sh BEFORE.csv AFTER.csv
#
# Prints, per case present in both files, the median of each run and the
# speedup (before / after; above 1 means AFTER is faster).
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,7p' "$0"; exit 2; }

awk -F, '
    FNR == 1  { next }
    NR == FNR { before[$1]= $2; next }
    ($1 in before) {
        ratio= $2 > 0 ? before[$1] / $2 : 0
        printf "%s,%s,%s,%.2fx\n", $1, before[$1], $2, ratio
    }
' "$1" "$2" | { echo "name,before_s,after_s,speedup"; cat; } | column -s, -t
