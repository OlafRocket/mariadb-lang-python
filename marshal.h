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

#ifndef LANG_PYTHON_MARSHAL_H
#define LANG_PYTHON_MARSHAL_H

/*
  Argument marshaling: MariaDB Item → Python object.

  GIL must be held by the caller.
  Always returns a new reference.  Never returns nullptr; falls back to
  Py_None (with INCREF) on any failure so the args tuple is always valid.
  NULL values in MariaDB (null_value == true) become Python None.
*/

typedef struct _object PyObject;          /* Python.h (CPython) */

class Item;                               /* sql_class.h */

PyObject *marshal_arg(Item *item, bool use_numpy);

#endif /* LANG_PYTHON_MARSHAL_H */
