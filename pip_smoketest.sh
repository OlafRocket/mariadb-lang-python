#!/bin/bash
# Copyright (c) 2026, Olaf Razzoli. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

# Smoke test: verify the bundled CPython's pip works end-to-end.
#
# Installs 'six' (a tiny pure-Python package, ~60 KB) into a scratch dir
# via pip and then imports it through the bundled interpreter. Exits 0 on
# success, non-zero on any failure.
#
# Usage:
#   pip_smoketest.sh <path/to/python3.14>
#
# Example (build tree):
#   plugin/lang_python/pip_smoketest.sh \
#     build/extra/cpython/bin/python3.14
#
# Example (container):
#   podman exec mdb /usr/lib/plugin/lang_python/pip_smoketest.sh \
#     /usr/lib/plugin/lang_python/bin/python3.14

set -euo pipefail

PYTHON="${1:?usage: $0 <python-binary>}"

if [[ ! -x "$PYTHON" ]]; then
  echo "ERROR: $PYTHON is not executable" >&2
  exit 1
fi

SCRATCH="$(mktemp -d)"
trap 'rm -rf "$SCRATCH"' EXIT

echo "==> Using interpreter: $PYTHON"
"$PYTHON" --version

echo "==> Installing 'six' into $SCRATCH"
"$PYTHON" -m pip install --quiet --target "$SCRATCH" six

echo "==> Importing 'six' through the bundled interpreter"
"$PYTHON" -c "
import sys
sys.path.insert(0, '$SCRATCH')
import six
print(f'OK: pip-installed six {six.__version__} imported successfully')
"
