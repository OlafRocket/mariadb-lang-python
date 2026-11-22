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

/*
  Columnar buffer -> numpy array (see py_columns.h).
*/

#include <Python.h>

#include <string.h>

#include "py_numpy.h"
#include "py_columns.h"


PyObject *py_column_to_array(Rowsink_column &c, size_t rows, bool nulls_nan,
                             const char *who, const char *hint,
                             PyObject *exc)
{
  npy_intp n= (npy_intp) rows;

  switch (c.kind)
  {
  case ROWSINK_INT64:
  case ROWSINK_UINT64:
  {
    if (c.null_rows.empty())
    {
      PyObject *arr= PyArray_SimpleNew(1, &n, c.kind == ROWSINK_UINT64
                                              ? NPY_UINT64 : NPY_INT64);
      if (arr && rows)
        memcpy(PyArray_DATA((PyArrayObject *) arr), c.ints.data(),
               rows * sizeof(long long));
      return arr;
    }
    if (!nulls_nan)
    {
      PyErr_Format(exc, "%sNULL in integer column '%s'%s", who,
                   c.name.c_str(), hint);
      return nullptr;
    }
    PyObject *arr= PyArray_SimpleNew(1, &n, NPY_FLOAT64);
    if (!arr)
      return nullptr;
    double *d= (double *) PyArray_DATA((PyArrayObject *) arr);
    if (c.kind == ROWSINK_UINT64)
      for (size_t i= 0; i < rows; i++)
        d[i]= (double) (unsigned long long) c.ints[i];
    else
      for (size_t i= 0; i < rows; i++)
        d[i]= (double) c.ints[i];
    for (size_t r : c.null_rows)
      d[r]= Py_NAN;
    return arr;
  }
  case ROWSINK_FLOAT64:
  {
    PyObject *arr= PyArray_SimpleNew(1, &n, NPY_FLOAT64);
    if (arr && rows)
      memcpy(PyArray_DATA((PyArrayObject *) arr), c.doubles.data(),
             rows * sizeof(double));
    return arr;
  }
  case ROWSINK_VECTOR:
  {
    npy_intp dims[2]= { n, (npy_intp) c.dim };
    PyObject *arr= PyArray_SimpleNew(2, dims, NPY_FLOAT32);
    if (arr && !c.floats.empty())
      memcpy(PyArray_DATA((PyArrayObject *) arr), c.floats.data(),
             c.floats.size() * sizeof(float));
    return arr;
  }
  case ROWSINK_OBJECT:
    break;
  }

  /* Object array: PyArray_SimpleNew zero-fills, each slot takes one ref */
  PyObject *arr= PyArray_SimpleNew(1, &n, NPY_OBJECT);
  if (!arr)
    return nullptr;
  PyObject **slot= (PyObject **) PyArray_DATA((PyArrayObject *) arr);
  size_t start= 0;
  for (size_t r= 0; r < rows; r++)
  {
    const char *p= c.bytes.data() + start;
    size_t len= c.ends[r] - start;
    start= c.ends[r];
    PyObject *o;
    switch (c.tags[r])
    {
    case ROWSINK_TAG_NULL:  o= Py_NewRef(Py_None); break;
    case ROWSINK_TAG_BYTES: o= PyBytes_FromStringAndSize(p, len); break;
    default:                o= PyUnicode_DecodeUTF8(p, len, "replace");
    }
    if (!o)
    {
      Py_DECREF(arr);
      return nullptr;
    }
    slot[r]= o;
  }
  return arr;
}
