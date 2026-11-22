/*
   Copyright (c) 2026, Olaf Razzoli

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA */

#ifndef LANG_PYTHON_PY_NUMPY_H
#define LANG_PYTHON_PY_NUMPY_H

/*
  The numpy C API, shared by every source file of the plugin.

  numpy reaches its functions through a table of pointers (PyArray_API).
  Without PY_ARRAY_UNIQUE_SYMBOL each source file gets its own static,
  uninitialised copy of that table.  The table is defined and filled once,
  in lang_python.cc (which defines LANG_PYTHON_IMPORT_ARRAY and calls
  _import_array()); every other file refers to that one instance.

  Include after Python.h.
*/

#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION
#define PY_ARRAY_UNIQUE_SYMBOL lang_python_ARRAY_API
#ifndef LANG_PYTHON_IMPORT_ARRAY
#define NO_IMPORT_ARRAY
#endif
#include <numpy/arrayobject.h>

#endif /* LANG_PYTHON_PY_NUMPY_H */
