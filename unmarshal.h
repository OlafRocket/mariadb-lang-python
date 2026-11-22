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

#ifndef LANG_PYTHON_UNMARSHAL_H
#define LANG_PYTHON_UNMARSHAL_H

/*
  Output unmarshaling: Python object → MariaDB result.

  Each function converts a Python object to the corresponding MariaDB
  type.  On conversion failure, sets null_value= true and emits a
  MariaDB warning (Level 6 in the NULL handling spec).

  GIL must be held by the caller.  The functions do NOT Py_DECREF obj;
  ownership stays with the caller.
*/

typedef struct _object PyObject;          /* Python.h (CPython) */

class String;                             /* sql_string.h */
struct my_decimal;                        /* my_decimal.h */

double      unmarshal_real(PyObject *obj, const char *func_name,
                           bool &null_value);
long long   unmarshal_int(PyObject *obj, const char *func_name,
                          bool &null_value);
String     *unmarshal_str(PyObject *obj, String *str, const char *func_name,
                          bool &null_value);
my_decimal *unmarshal_decimal(PyObject *obj, my_decimal *dec,
                              const char *func_name, bool &null_value);

#endif /* LANG_PYTHON_UNMARSHAL_H */
