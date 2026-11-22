#!/bin/sh
# Run example files with the source code visible: -vvv echoes each
# statement before its result, --comments keeps the explanatory comments.
#
# Usage: ./run.sh [example-file] [mariadb client options...]
#
# With no example file, every side-effect-free example runs. slack_send
# and pagerduty_send notify real endpoints (Slack / PagerDuty), so they
# are optional: they run only when named explicitly.
#
# Examples:
#   ./run.sh                          # get_python_path, misc, ping
#   ./run.sh ping -h 127.0.0.1        # one example, extra client options
#   ./run.sh slack_send               # optional example, explicit opt-in
set -eu

dir=$(dirname "$0")
default_examples="get_python_path misc ping"

case "${1-}" in
    ''|-*) files=$default_examples ;;
    *)     files=$1; shift ;;
esac

for f in $files; do
    echo "-- ==== $f ===="
    mariadb --comments -vvv --show-warnings "$@" < "$dir/$f"
done
