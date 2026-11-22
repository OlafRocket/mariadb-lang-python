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

#ifndef LANG_PYTHON_PY_MDB_MODULE_H
#define LANG_PYTHON_PY_MDB_MODULE_H

/*
  Built-in `inhost` Python module (Phase 1.6): the namespace through which
  Python bodies reach the hosting MariaDB server's services.  Today:
  inhost.sql (in-process database access); future services (e.g. inhost.uid)
  get their own submodules.  See py_inhost_module.cc.

  The init function must be registered with
  PyImport_AppendInittab("inhost", ...) BEFORE Py_Initialize.
*/

typedef struct _object PyObject;          /* Python.h (CPython) */

extern "C" PyObject *PyInit_lang_python_inhost(void);

#endif /* LANG_PYTHON_PY_MDB_MODULE_H */
