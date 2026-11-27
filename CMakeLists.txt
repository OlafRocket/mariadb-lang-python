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

INCLUDE(ExternalProject)
FIND_PACKAGE(Threads REQUIRED)

# patchelf is used to rewrite the bundled Python interpreter's RPATH so it
# finds libpython3.14.so via $ORIGIN/../lib regardless of install location.
FIND_PROGRAM(PATCHELF_EXECUTABLE patchelf)
IF(NOT PATCHELF_EXECUTABLE)
  MESSAGE(FATAL_ERROR
    "patchelf is required to build lang_python (install with: dnf install patchelf)")
ENDIF()

# ---------------------------------------------------------------------------
# Bundled CPython (shared build)
# ---------------------------------------------------------------------------
SET(PYTHON_VERSION "3.14.3")
SET(PYTHON_ABI     "3.14")
SET(PYTHON_DIR     "${CMAKE_BINARY_DIR}/extra/cpython")

# ExternalProject's <INSTALL_DIR> resolves to the PREFIX directory itself.
# After "make install --prefix=<INSTALL_DIR>", files land here:
#   <PYTHON_DIR>/lib/libpython3.14.so
#   <PYTHON_DIR>/include/python3.14/Python.h
#   <PYTHON_DIR>/lib/python3.14/  (stdlib)
SET(PYTHON_LIB
  "${PYTHON_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}python${PYTHON_ABI}${CMAKE_SHARED_LIBRARY_SUFFIX}")

SET(PYTHON_INCLUDE "${PYTHON_DIR}/include/python${PYTHON_ABI}")

# Where the shared lib and stdlib will be installed relative to the plugin.
# Layout:  <plugin_dir>/lang_python/libpython3.14.so
#          <plugin_dir>/lang_python/lib/python3.14/  (stdlib + site-packages)
SET(PYTHON_RUNTIME_SUBDIR "lang_python")

# ---------------------------------------------------------------------------
# Optional integrity check.
# Obtain the SHA-256 of Python-3.14.3.tar.xz from https://www.python.org/downloads/
# and pass it at configure time:  cmake -DPYTHON_SHA256=<hash> ...
# Without it the download still works but is not verified.
# ---------------------------------------------------------------------------
SET(PYTHON_SHA256 "" CACHE STRING
  "SHA256 checksum of Python-${PYTHON_VERSION}.tar.xz (from python.org/downloads)")

IF(PYTHON_SHA256)
  SET(_python_hash URL_HASH SHA256=${PYTHON_SHA256})
ELSE()
  MESSAGE(STATUS
    "PYTHON_SHA256 is not set; the Python tarball will not be verified. "
    "Pass -DPYTHON_SHA256=<hash> for a secure build.")
  SET(_python_hash)
ENDIF()

# Pre-create the include directory so that the IMPORTED target's
# INTERFACE_INCLUDE_DIRECTORIES is valid before the build runs.
MAKE_DIRECTORY("${PYTHON_INCLUDE}")

# ---------------------------------------------------------------------------
# Download → configure (autoconf) → make → make install
# ---------------------------------------------------------------------------
ExternalProject_Add(cpython
  PREFIX  "${PYTHON_DIR}"
  URL     "https://www.python.org/ftp/python/${PYTHON_VERSION}/Python-${PYTHON_VERSION}.tar.xz"
  ${_python_hash}

  # autoconf-based build; pass the same C compiler that the server uses so
  # the ABI is compatible.
  CONFIGURE_COMMAND
    <SOURCE_DIR>/configure
      "CC=${CMAKE_C_COMPILER}"
      "CFLAGS=${CMAKE_C_FLAGS} ${PIC_FLAG}"
      --prefix=<INSTALL_DIR>
      --enable-shared            # build libpython3.14.so
      --with-ensurepip=install   # bootstrap pip for installing packages
      --disable-test-modules     # skip test extension modules (Python >= 3.10)

  # Note: $(MAKE) only works under the Make generator: Ninja parses `$(` as
  # a bad $-escape. Use literal `make` with -j so CPython still builds in
  # parallel; we lose Make's jobserver coordination, but CPython only
  # rebuilds on a clean, so oversubscription is a non-issue.
  BUILD_COMMAND   make -j
  INSTALL_COMMAND make install

  # Declare the byproduct so Ninja/make know about it before it exists.
  BUILD_BYPRODUCTS "${PYTHON_LIB}"
)

# ---------------------------------------------------------------------------
# Install mandatory Python packages (numpy) into the bundled interpreter.
# This runs after CPython is built and installed.
# ---------------------------------------------------------------------------
SET(PYTHON_EXE "${PYTHON_DIR}/bin/python${PYTHON_ABI}")

# Rewrite the interpreter's RPATH to $ORIGIN/../lib so it finds libpython
# relative to its own location, both in the build tree and after install.
# CPython's autoconf build hardcodes the --prefix path as an absolute RPATH,
# which breaks the moment the interpreter is copied into the plugin dir.
ExternalProject_Add_Step(cpython patch_rpath
  COMMAND "${PATCHELF_EXECUTABLE}" --set-rpath "\$ORIGIN/../lib" "${PYTHON_EXE}"
  DEPENDEES install
  COMMENT "Patching bundled python RPATH to \$ORIGIN/../lib"
)

ExternalProject_Add_Step(cpython install_numpy
  COMMAND "${PYTHON_EXE}" -m pip install --no-warn-script-location numpy
  DEPENDEES patch_rpath
  COMMENT "Installing numpy into bundled Python"
)

# ---------------------------------------------------------------------------
# Imported target that plugin.cc can link against
# ---------------------------------------------------------------------------
ADD_LIBRARY(python3_bundled SHARED IMPORTED GLOBAL)
ADD_DEPENDENCIES(python3_bundled cpython)
SET_TARGET_PROPERTIES(python3_bundled PROPERTIES
  IMPORTED_LOCATION "${PYTHON_LIB}"
  IMPORTED_NO_SONAME TRUE)
# numpy C API headers (numpy/arrayobject.h) installed by pip into site-packages.
# numpy 2.x moved headers from numpy/core/include to numpy/_core/include.
SET(NUMPY_INCLUDE
  "${PYTHON_DIR}/lib/python${PYTHON_ABI}/site-packages/numpy/_core/include")
# Pre-create the numpy include directory so the IMPORTED target's
# INTERFACE_INCLUDE_DIRECTORIES is valid before numpy is installed.
MAKE_DIRECTORY("${NUMPY_INCLUDE}")

TARGET_INCLUDE_DIRECTORIES(python3_bundled INTERFACE
  "${PYTHON_INCLUDE}" "${NUMPY_INCLUDE}")

# ---------------------------------------------------------------------------
# The plugin itself
# ---------------------------------------------------------------------------
MYSQL_ADD_PLUGIN(lang_python plugin.cc lang_python.cc item_func_python.cc
  item_func_py_named.cc py_func_catalog.cc py_inhost_module.cc
  py_inhost_rowsink.cc py_columns.cc item_sum_py_agg.cc py_thread_pin.cc
  marshal.cc unmarshal.cc lang_routine.cc
  RECOMPILE_FOR_EMBEDDED MODULE_ONLY COMPONENT Server
  LINK_LIBRARIES python3_bundled ${CMAKE_DL_LIBS} m Threads::Threads
  DEPENDS cpython)

# Pass the build-tree Python prefix so the plugin can find the stdlib
# during development (before "make install" copies it next to the plugin).
IF(TARGET lang_python)
  TARGET_COMPILE_DEFINITIONS(lang_python PRIVATE
    "PYTHON_HOME_BUILDDIR=\"${PYTHON_DIR}\"")
ENDIF()

# ---------------------------------------------------------------------------
# RPATH: make the plugin find libpython3.14.so relative to its own location
# via $ORIGIN, so LD_LIBRARY_PATH is not needed at runtime.
#
# Runtime layout:
#   <plugin_dir>/lang_python.so
#   <plugin_dir>/lang_python/lib/libpython3.14.so
#   <plugin_dir>/lang_python/lib/python3.14/   (stdlib + site-packages)
# ---------------------------------------------------------------------------
IF(TARGET lang_python)
  SET_TARGET_PROPERTIES(lang_python PROPERTIES
    INSTALL_RPATH "\$ORIGIN/${PYTHON_RUNTIME_SUBDIR}/lib"
    BUILD_RPATH   "${PYTHON_DIR}/lib"
    BUILD_WITH_INSTALL_RPATH FALSE)

  # At install time, copy the Python shared libs (versioned + symlink) next to
  # the plugin.  The linker records the versioned name (libpython3.14.so.1.0)
  # as NEEDED, so both must be present.
  # Use install(CODE) instead of FILE(GLOB) + INSTALL(FILES) because the
  # ExternalProject builds cpython after cmake configure, so a configure-time
  # glob would find nothing.
  INSTALL(CODE "
    FILE(GLOB _python_libs
      \"${PYTHON_DIR}/lib/${CMAKE_SHARED_LIBRARY_PREFIX}python${PYTHON_ABI}*${CMAKE_SHARED_LIBRARY_SUFFIX}*\")
    FOREACH(_lib \${_python_libs})
      FILE(INSTALL \${_lib}
        DESTINATION \"\${CMAKE_INSTALL_PREFIX}/${INSTALL_PLUGINDIR}/${PYTHON_RUNTIME_SUBDIR}/lib\")
    ENDFOREACH()
  " COMPONENT Server)
  INSTALL(DIRECTORY "${PYTHON_DIR}/lib/python${PYTHON_ABI}/"
    DESTINATION ${INSTALL_PLUGINDIR}/${PYTHON_RUNTIME_SUBDIR}/lib/python${PYTHON_ABI}
    COMPONENT Server
    PATTERN "__pycache__" EXCLUDE
    PATTERN "test"        EXCLUDE
    PATTERN "tests"       EXCLUDE)

  # Install the Python interpreter so users can pip install packages at runtime.
  # Only the main binary is needed; pip3 etc. are accessible via "python3.14 -m pip".
  # RPATH has already been rewritten to $ORIGIN/../lib by the patch_rpath step,
  # so it finds libpython3.14.so from the sibling lib/ directory with no
  # LD_LIBRARY_PATH needed.
  INSTALL(PROGRAMS "${PYTHON_DIR}/bin/python${PYTHON_ABI}"
    DESTINATION ${INSTALL_PLUGINDIR}/${PYTHON_RUNTIME_SUBDIR}/bin
    COMPONENT Server)

  # Smoke test for pip + bundled interpreter; runnable from the install tree
  # (or container) to verify package installation works end-to-end.
  INSTALL(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/pip_smoketest.sh"
    DESTINATION ${INSTALL_PLUGINDIR}/${PYTHON_RUNTIME_SUBDIR}
    COMPONENT Server)
ENDIF()
