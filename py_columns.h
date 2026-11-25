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

#ifndef LANG_PYTHON_PY_COLUMNS_H
#define LANG_PYTHON_PY_COLUMNS_H

/*
  Columnar buffer -> numpy array, for inhost.sql.query_array().  The row
  sink fills Rowsink_column buffers without the GIL; they are converted
  here, with the GIL held, by one copy into an exact-size array:

    ROWSINK_INT64 / UINT64  int64 / uint64; if the column holds NULLs,
                            float64 with NaN when nulls_nan, else an
                            exception of type exc ("<who>NULL in integer
                            column '<name>'<hint>")
    ROWSINK_FLOAT64         float64 (NULL is already NaN)
    ROWSINK_VECTOR          2-D float32, rows x dim
    ROWSINK_OBJECT          object array of str / bytes / None

  Returns a new reference, or nullptr with a Python exception set.
*/

#include "py_inhost_rowsink.h"

typedef struct _object PyObject;          /* Python.h (CPython) */

PyObject *py_column_to_array(Rowsink_column &c, size_t rows, bool nulls_nan,
                             const char *who, const char *hint,
                             PyObject *exc);

#endif /* LANG_PYTHON_PY_COLUMNS_H */
