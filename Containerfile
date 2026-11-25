# syntax=docker/dockerfile:1.4
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

# Containerfile: MariaDB + lang_python plugin (works with Podman and Docker).
#
# Two build targets share a single builder stage:
#   base-image: MariaDB + bundled CPython runtime, no lang_python plugin
#   full-image: base-image + lang_python plugin (default target)
#
# Run from the root of the lang_python repo: the builder stage clones
# the MariaDB server tree itself (pinned via MARIADB_REF) and grafts
# this repo in as plugin/lang_python, so the build context is just the
# plugin sources.
#
# Build the full image (default):
#   podman build -t mariadb-langpython .
#
# Build the base only (no plugin .so / tests):
#   podman build --target base-image -t mariadb-langpython-base .
#
# Build both, in either order: the second invocation reuses the
# builder stage from the first via Podman's layer cache:
#   podman build -t mariadb-langpython-base --target base-image .
#   podman build -t mariadb-langpython .          # near-instant
#
# Build against a different MariaDB version (branch, tag, or commit SHA):
#   podman build --build-arg MARIADB_REF=12.3 ...
#
# Skip the strip step (keep build-time files for debugging):
#   podman build --build-arg STRIP=false ...
#
# Run:
#   podman run -d --name mdb -p 3306:3306 mariadb-langpython
#
# Connect:
#   podman exec -it mdb mariadb -h 127.0.0.1 -u root
#
# Test (full plugin suite):
#   podman run --rm --user root mariadb-langpython \
#     bash -c "cd /usr/mariadb-test && \
#              perl mariadb-test-run.pl --suite=lang_python --force"
#
# Build performance: the builder stage uses a BuildKit cache mount for
# /build, so CPython and unchanged objects survive across rebuilds.
# A clean first build takes ~20 minutes (full MariaDB + CPython
# compile); subsequent rebuilds with only plugin source changes finish
# in ~2-3 minutes because cmake reuses the cached build tree.

# ==========================================================================
# Stage 1: builder: compile MariaDB + plugin once into /staging
# ==========================================================================
FROM rockylinux:9 AS builder

RUN dnf -y install epel-release && \
    dnf -y groupinstall "Development Tools" && \
    dnf -y install \
        cmake gcc gcc-c++ bison patchelf git \
        ncurses-devel openssl-devel pcre2-devel \
        libaio-devel zlib-devel xz-devel bzip2-devel \
        snappy-devel lz4-devel libxml2-devel libcurl-devel \
        systemd-devel pam-devel \
        perl perl-XML-Parser perl-Data-Dumper perl-Getopt-Long \
        perl-Memoize perl-JSON-PP perl-Term-ANSIColor \
        perl-Test-Simple perl-Time-HiRes perl-FindBin \
    && dnf clean all

# Fetch the MariaDB server source at a pinned ref: branch, tag, or
# commit SHA all work (GitHub allows fetch-by-SHA).  init+fetch rather
# than `git clone --branch` so a bare SHA is accepted.  Only the two
# submodules this build needs are initialized.  This is its own layer:
# plugin-only edits below never re-download the server tree.
# Default = the version the plugin is currently tested against: a
# release tag of MariaDB 12.3.  This line is the only record of the
# pin; BUILD.md and docs/ read it from here.
ARG MARIADB_REF=mariadb-12.3.4
RUN git init -q /src && cd /src && \
    git remote add origin https://github.com/MariaDB/server.git && \
    git fetch -q --depth 1 origin ${MARIADB_REF} && \
    git checkout -q FETCH_HEAD && \
    git submodule update --init --depth 1 libmariadb wsrep-lib

WORKDIR /src
COPY . /src/plugin/lang_python

# Build into /build with a persistent BuildKit cache mount so CPython
# and unchanged objects survive across rebuilds.  Anything that needs
# to persist into later stages must end up in /staging (a real layer)
# before this RUN exits, since /build is unmounted afterwards.
RUN --mount=type=cache,target=/build,sharing=locked \
    cd /build && \
    cmake /src \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DPLUGIN_TOKUDB=NO -DPLUGIN_MROONGA=NO -DPLUGIN_SPIDER=NO \
        -DPLUGIN_OQGRAPH=NO -DPLUGIN_ROCKSDB=NO -DPLUGIN_CONNECT=NO \
        -DPLUGIN_S3=NO -DPLUGIN_COLUMNSTORE=NO -DPLUGIN_DUCKDB=NO \
        -DWITH_UNIT_TESTS=OFF -DWITH_EMBEDDED_SERVER=OFF \
        -DCMAKE_INSTALL_PREFIX=/usr \
    && make -j$(nproc) \
    && make install DESTDIR=/staging \
    && cmake -DBUILD_DIR=/build -P /src/plugin/lang_python/install_paths.cmake \
    && cp /build/install_paths.sh /staging/install_paths.sh

# Optional: strip the staging area to reduce image size.
ARG STRIP=true
RUN if [ "$STRIP" = "true" ]; then \
      . /staging/install_paths.sh && S="/staging${PREFIX}" && \
      rm -rf \
        ${S}/include \
        ${S}/lib/pkgconfig ${S}/lib64/pkgconfig \
        ${S}/share/aclocal \
        ${S}/share/man ${S}/man \
        ${S}/support-files \
      && \
      find ${S} -name '*.a' -delete && \
      find ${S} -name '__pycache__' -type d -exec rm -rf {} + 2>/dev/null; \
      PY_STDLIB="${S}/${PLUGINDIR}/lang_python/lib/python3.14" && \
      rm -rf \
        ${PY_STDLIB}/tkinter \
        ${PY_STDLIB}/turtle*.py \
        ${PY_STDLIB}/idlelib \
        ${PY_STDLIB}/lib2to3 \
        ${PY_STDLIB}/turtledemo \
        ${PY_STDLIB}/unittest \
        ${PY_STDLIB}/doctest.py \
        ${PY_STDLIB}/pydoc.py \
        ${PY_STDLIB}/pydoc_data \
        ${PY_STDLIB}/distutils \
        ${PY_STDLIB}/venv \
        ${PY_STDLIB}/config-* \
      ; \
    fi

# Split: the plugin .so + its tests move to /staging-plugin; the
# base-image stage copies /staging, the full-image stage adds
# /staging-plugin on top.  This keeps the plugin layer small (~200 KB)
# so registry consumers who already have the base only pull the delta.
RUN . /staging/install_paths.sh && \
    mkdir -p "/staging-plugin${PREFIX}/${PLUGINDIR}" \
             "/staging-plugin${PREFIX}/${TESTDIR}/plugin" && \
    mv "/staging${PREFIX}/${PLUGINDIR}/lang_python.so" \
       "/staging-plugin${PREFIX}/${PLUGINDIR}/" && \
    mv "/staging${PREFIX}/${TESTDIR}/plugin/lang_python" \
       "/staging-plugin${PREFIX}/${TESTDIR}/plugin/"

# ==========================================================================
# Stage 2: base-image: MariaDB + bundled Python (no lang_python plugin)
# ==========================================================================
FROM rockylinux:9 AS base-image

RUN dnf -y --allowerasing install \
        ncurses-libs openssl-libs pcre2 libaio zlib xz-libs bzip2-libs \
        snappy lz4-libs libxml2 libcurl systemd-libs pam \
        perl perl-Data-Dumper perl-Getopt-Long perl-JSON-PP \
        perl-Term-ANSIColor perl-Test-Simple perl-Time-HiRes \
    && dnf clean all

COPY --from=builder /staging/usr /usr
COPY --from=builder /staging/install_paths.sh /tmp/install_paths.sh

# Create mysql user/group and data directory
RUN groupadd -r mysql && useradd -r -g mysql -s /sbin/nologin mysql && \
    mkdir -p /var/lib/mysql /var/run/mysqld /var/log/mariadb && \
    chown -R mysql:mysql /var/lib/mysql /var/run/mysqld /var/log/mariadb

# Initialize system tables
RUN . /tmp/install_paths.sh && \
    "${PREFIX}/${SCRIPTDIR}/mariadb-install-db" \
        --user=mysql \
        --basedir="${PREFIX}" \
        --datadir=/var/lib/mysql \
        --auth-root-authentication-method=normal && \
    rm -f /tmp/install_paths.sh

# Server configuration: load the plugin automatically (harmless if the
# .so isn't present; base-image consumers can drop the line, or layer
# the full-image variant which provides it).
RUN mkdir -p /etc/my.cnf.d && \
    printf '[mariadbd]\n\
datadir=/var/lib/mysql\n\
socket=/var/run/mysqld/mysqld.sock\n\
user=mysql\n\
plugin_load_add=lang_python.so\n\
plugin_maturity=experimental\n\
bind-address=0.0.0.0\n\
\n\
[client]\n\
socket=/var/run/mysqld/mysqld.sock\n' \
    > /etc/my.cnf.d/lang_python.cnf

EXPOSE 3306

USER mysql
CMD ["mariadbd", "--defaults-file=/etc/my.cnf.d/lang_python.cnf"]

# ==========================================================================
# Stage 3: full-image: base + lang_python plugin (default target)
# ==========================================================================
FROM base-image AS full-image
USER root
COPY --from=builder /staging-plugin/usr /usr
USER mysql
