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

# Read INSTALL_PLUGINDIR and INSTALL_MYSQLTESTDIR from the CMake build cache
# and write them as a shell-sourceable file.
#
# Usage:  cmake -DBUILD_DIR=/build -P install_paths.cmake
#
# Output: ${BUILD_DIR}/install_paths.sh

IF(NOT BUILD_DIR)
  MESSAGE(FATAL_ERROR "BUILD_DIR must be set: cmake -DBUILD_DIR=<path> -P install_paths.cmake")
ENDIF()

LOAD_CACHE(${BUILD_DIR} READ_WITH_PREFIX _
  INSTALL_PLUGINDIR
  INSTALL_MYSQLTESTDIR
  INSTALL_SCRIPTDIR
  CMAKE_INSTALL_PREFIX)

FILE(WRITE "${BUILD_DIR}/install_paths.sh"
  "PREFIX=${_CMAKE_INSTALL_PREFIX}\n"
  "PLUGINDIR=${_INSTALL_PLUGINDIR}\n"
  "TESTDIR=${_INSTALL_MYSQLTESTDIR}\n"
  "SCRIPTDIR=${_INSTALL_SCRIPTDIR}\n")
