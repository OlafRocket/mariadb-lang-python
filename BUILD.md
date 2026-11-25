# Building lang_python

## Repo layout

This repo holds only the plugin. Version 0.5 is developed and tested
against a release of MariaDB 12.3, without server changes. The pin is
the `ARG MARIADB_REF=` default in the [Containerfile](Containerfile),
the only place the release tag is recorded.

The pin follows the 12.3 releases; see
[Moving the server pin](#moving-the-server-pin).

The plugin builds in one of two ways:

- **Container build** (self-contained): the Containerfile clones the
  MariaDB server tree itself and grafts this repo in as
  `plugin/lang_python` (see [Container build](#container-build)).
- **Local build** (in a server tree): clone the server from the official
  MariaDB GitHub repo, check out the pinned commit, then symlink this
  repo in. MariaDB's CMake auto-discovers any `plugin/*` subdirectory:

  ```bash
  git clone https://github.com/MariaDB/server.git
  cd server
  git checkout "$(sed -n 's/^ARG MARIADB_REF=//p' /path/to/lang_python/Containerfile)"
  git submodule update --init libmariadb wsrep-lib
  ln -s /path/to/lang_python plugin/lang_python
  echo plugin/lang_python >> .git/info/exclude
  ```

  (A different server version is any other branch, tag or commit in
  the `git checkout` line. The plugin uses internal server APIs, so
  compile breaks across server versions are expected.)

### Moving the server pin

1. In the server tree, fetch and check out the new 12.3 release tag:
   `git fetch origin --tags && git checkout mariadb-12.3.<n>`, then
   `git submodule update --init libmariadb wsrep-lib`.
2. Rebuild the server and the plugin, then run the suite
   (`--suite=lang_python`).
3. When the suite passes, set `ARG MARIADB_REF=` in the
   [Containerfile](Containerfile) to the new tag and commit it
   as `build: move MariaDB pin to <tag>`, together with any
   plugin fixes the new server required. When it fails, the pin stays
   until the fixes are ready.

## Prerequisites

- C/C++ compiler (GCC or Clang)
- CMake 3.x
- Standard MariaDB build dependencies
- Internet access (CMake downloads CPython and numpy automatically)

### Rocky Linux 9 / RHEL 9

```bash
dnf -y install epel-release
dnf -y groupinstall "Development Tools"
dnf -y install \
    cmake gcc gcc-c++ bison patchelf \
    ncurses-devel openssl-devel pcre2-devel \
    libaio-devel zlib-devel xz-devel bzip2-devel \
    snappy-devel lz4-devel libxml2-devel libcurl-devel \
    systemd-devel pam-devel
```

For running the test suite (MTR), also install:

```bash
dnf -y install \
    perl perl-XML-Parser perl-Data-Dumper perl-Getopt-Long \
    perl-Memoize perl-JSON-PP perl-Term-ANSIColor \
    perl-Test-Simple perl-Time-HiRes perl-FindBin
```

## Quick start

From the MariaDB source root:

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
make -j$(nproc) lang_python
```

The plugin `.so` and bundled Python runtime are in the build tree under
`plugin/lang_python/` and `extra/cpython/`.

## CMake options

### Plugin-specific options

| Option | Type | Default | Description |
|---|---|---|---|
| `PYTHON_SHA256` | STRING | *(empty)* | SHA-256 checksum of the CPython source tarball. When set, the download is verified. Obtain the hash from [python.org/downloads](https://www.python.org/downloads/). Without it, a warning is printed but the build proceeds. |

### Useful MariaDB options

| Option | Type | Default | Description |
|---|---|---|---|
| `CMAKE_BUILD_TYPE` | STRING | `RelWithDebInfo` | Build type: `Debug`, `Release`, `RelWithDebInfo`, `MinSizeRel` |
| `CMAKE_INSTALL_PREFIX` | PATH | `/usr/local` | Installation prefix. Use `/usr` for system installs or container builds. |
| `PLUGIN_LANG_PYTHON` | STRING | `DYNAMIC` | `DYNAMIC` (default), `NO` to skip, `YES` to link statically (not recommended) |

To speed up builds when working only on the plugin, disable unneeded plugins:

```bash
cmake .. \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DPLUGIN_TOKUDB=NO \
    -DPLUGIN_MROONGA=NO \
    -DPLUGIN_SPIDER=NO \
    -DPLUGIN_OQGRAPH=NO \
    -DPLUGIN_ROCKSDB=NO \
    -DPLUGIN_CONNECT=NO \
    -DPLUGIN_S3=NO \
    -DPLUGIN_COLUMNSTORE=NO \
    -DPLUGIN_DUCKDB=NO \
    -DWITH_UNIT_TESTS=OFF \
    -DWITH_EMBEDDED_SERVER=OFF
```

### Verified build (recommended for releases)

```bash
cmake .. -DPYTHON_SHA256=<hash-from-python.org>
```

## Installing

```bash
make install
# or with DESTDIR for packaging:
make install DESTDIR=/staging
```

### Installed layout

```
<prefix>/<plugindir>/
    lang_python.so                          # the plugin
    lang_python/
        lib/
            libpython3.14.so.1.0            # Python shared library
            python3.14/                     # Python stdlib + site-packages
                site-packages/
                    numpy/                  # bundled numpy
        bin/
            python3.14                      # interpreter for pip install
```

The `<plugindir>` depends on the install layout (set automatically or via
`-DINSTALL_LAYOUT=`): `lib64/mysql/plugin` (RPM), `lib/mysql/plugin` (DEB),
`lib/plugin` (STANDALONE), etc.

The plugin finds `libpython` relative to its own location via `$ORIGIN` RPATH,
so `LD_LIBRARY_PATH` is not needed.

## Development workflow

After the initial full build, rebuild just the plugin:

```bash
make -j$(nproc) lang_python
```

CPython and numpy are only rebuilt when their ExternalProject inputs change
(version bump, configure flags, etc.).

### Installing Python packages in the build tree

The bundled interpreter's RPATH is rewritten to `$ORIGIN/../lib` via
`patchelf` at build time, so it finds `libpython3.14.so` from its sibling
`lib/` directory with no `LD_LIBRARY_PATH` needed, both in the build tree
and after install:

```bash
# From the build directory:
extra/cpython/bin/python3.14 -m pip install torch --index-url https://download.pytorch.org/whl/cpu
extra/cpython/bin/python3.14 -m pip install pandas
```

Installed packages are immediately available to the plugin when running
MariaDB from the build tree (e.g., via the test runner).

### Smoke-testing pip + bundled interpreter

A small helper script verifies that the embedded CPython and pip work
end-to-end without depending on heavy packages.  It installs `six` (a
single-file pure-Python package) into a scratch directory, imports it
through the bundled interpreter, and exits non-zero on any failure.

```bash
# From the build tree:
plugin/lang_python/pip_smoketest.sh \
    build/extra/cpython/bin/python3.14

# After `make install` (paths depend on CMAKE_INSTALL_PREFIX):
/usr/local/mariadb/lib/plugin/lang_python/pip_smoketest.sh \
    /usr/local/mariadb/lib/plugin/lang_python/bin/python3.14

# Inside a container:
podman exec mdb \
    /usr/lib/plugin/lang_python/pip_smoketest.sh \
    /usr/lib/plugin/lang_python/bin/python3.14
```

## Running tests

### From the build tree

```bash
cd mysql-test
perl mysql-test-run.pl --suite=lang_python --force
```

### In a container

See the container build section below, or:

```bash
podman run --rm --user root mariadb-langpython \
  bash -c "cd /usr/mariadb-test && \
           perl mariadb-test-run.pl --suite=lang_python --force"
```

## Container build

A single [`Containerfile`](Containerfile) produces two images via
named build targets that share one builder stage:

- **`base-image`**: MariaDB server + bundled Python runtime
  (CPython 3.14 + numpy).  No `lang_python` plugin.
- **`full-image`** (default): `base-image` + the `lang_python.so`
  plugin and its test files.  This is the image for normal use.

When the images are pushed to a registry, consumers that have the base
image pull only the small plugin layer (~10 MB) on plugin updates.

### Build the full image (default)

Run from the root of this repo.  The builder stage clones the MariaDB
server tree itself (pinned via the `MARIADB_REF` build arg) and grafts
this repo in as `plugin/lang_python`, so the build context is only the
plugin sources:

```bash
podman build -t mariadb-langpython .
```

To build against a different server version (branch, tag, or commit):

```bash
podman build --build-arg MARIADB_REF=12.3 -t mariadb-langpython .
```

By default the build strips build-time-only files (headers, static
libs, pkg-config, man pages, support-files) and unused Python stdlib
modules (tkinter, idlelib, unittest, etc.) to reduce image size.  All
MariaDB binaries, debug symbols, pip, and numpy are preserved.

To keep all build-time files (useful for debugging):

```bash
podman build --build-arg STRIP=false -t mariadb-langpython .
```

### Build the base image only

```bash
podman build --target base-image -t mariadb-langpython-base .
```

Building both, in either order, reuses the builder stage from the
first invocation.  The second build is near-instant.

### Build performance

The builder stage uses a BuildKit cache mount for `/build`, so CPython
and unchanged objects survive across rebuilds:

| Scenario                                  | Time   |
|-------------------------------------------|--------|
| First build (cold)                        | ~20 min |
| Rebuild after plugin source change        | ~2 to 3 min |
| Rebuild with no source change             | seconds |
| Building the second target after the first | seconds |

The build context is only this repo (a few hundred KB): the server
tree is fetched inside the builder stage, and a [`.containerignore`](.containerignore)
keeps `.git/` out of the context.

### Running the container

```bash
# Start
podman run -d --name mdb -p 3306:3306 mariadb-langpython

# Connect
podman exec -it mdb mariadb -h 127.0.0.1 -u root

# Verify the plugin
SELECT py_eval('1 + 1', 'INT');
```

## Installing additional Python packages

The plugin ships with a bundled Python interpreter and pip.  Use it to
install packages into the bundled environment so they are available from
SQL functions.

### From the command line (bare-metal install)

Find the bundled interpreter under the plugin directory and use it directly:

```bash
# Standard RPM install:
/usr/lib/plugin/lang_python/bin/python3.14 -m pip install pandas

# Custom prefix install (adjust path to match your prefix):
/usr/local/lib/plugin/lang_python/bin/python3.14 -m pip install pandas
```

### From a build tree (development)

The bundled interpreter is in the CPython build prefix and has its
RPATH patched to `$ORIGIN/../lib`, so no env vars are needed:

```bash
# From the build directory:
extra/cpython/bin/python3.14 -m pip install pandas
```

### Inside a container

```bash
# Into a running container:
podman exec mdb \
    /usr/lib/plugin/lang_python/bin/python3.14 -m pip install pandas

# During image build (add to the Containerfile):
RUN /usr/lib/plugin/lang_python/bin/python3.14 -m pip install pandas
```

After installing, the package is immediately available:

```sql
SELECT py_exec('
import pandas as pd
df = pd.DataFrame({"a": [1, 2, 3]})
result = str(df.describe())
', 'VARCHAR');
```

A server restart is only needed if the module was previously imported and
failed (Python caches failed imports in `sys.modules`).
