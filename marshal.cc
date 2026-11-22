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
  Argument marshaling: MariaDB Item → Python object

  GIL must be held by the caller.
  Always returns a new reference.  Never returns nullptr; falls back to
  Py_None (with INCREF) on any failure so the args tuple is always valid.
  NULL values in MariaDB (null_value == true) become Python None.

  MariaDB type → Python type mapping:
    INT_RESULT      → int        (integer literals, INT/BIGINT columns;
                                  unsigned types use unsigned conversion
                                  to handle values above 2^63-1)
    REAL_RESULT     → float      (CAST(x AS DOUBLE), scientific notation 2.5e0,
                                  DOUBLE/FLOAT columns)
    DECIMAL_RESULT  → decimal.Decimal  (bare decimal literals like 2.5, 3.14;
                                         DECIMAL/NUMERIC columns)
    STRING_RESULT   → str        (string literals, VARCHAR/TEXT columns,
                                  JSON columns and JSON expressions; MariaDB
                                  has no JSON_RESULT, JSON is STRING_RESULT)
                    → bytes      (BLOB columns, GEOMETRY, hex literals X'...',
                                  VARBINARY, UNHEX(), binary charset data)
                    → numpy.ndarray(float32)
                                 (VECTOR columns, auto-converted from the
                                  packed float32 buffer via the numpy C API
                                  (PyArray_SimpleNew + memcpy) for efficient
                                  ML/Pandas use with zero Python overhead)

  VECTOR columns are automatically converted to numpy.ndarray(float32) for
  efficient ML/Pandas use: no manual struct.unpack() needed:
    py_eval('float(args[0].dot(args[1]))', 'DOUBLE', v1, v2)
  To return vector data, pack with ndarray.tobytes() and use BLOB:
    py_exec('result = numpy.array([1,2,3], dtype=numpy.float32).tobytes()', 'BLOB')

  The user controls whether a numeric literal becomes float or Decimal:
    py_eval('...', 'DOUBLE', 2.5)           -- args[0] is decimal.Decimal('2.5')
    py_eval('...', 'DOUBLE', 2.5e0)         -- args[0] is float(2.5)
    py_eval('...', 'DOUBLE', CAST(2.5 AS DOUBLE))  -- args[0] is float(2.5)

  Note: the second argument (type) only controls the *return* type of the
  function, not the types of the data arguments.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>
#include <sql_type_vector.h>

#include "py_numpy.h"

#include <string.h>  /* memcpy */

#include "marshal.h"

PyObject *marshal_arg(Item *item, bool use_numpy)
{
  switch (item->result_type())
  {
  case INT_RESULT:
  {
    /* Integer literals (42), INT/BIGINT columns → Python int.
       Unsigned types (INT UNSIGNED, BIGINT UNSIGNED) use the unsigned
       path to correctly handle values above 2^63-1, matching the
       C connector's signed/unsigned distinction. */
    longlong v= item->val_int();
    if (item->null_value) { Py_INCREF(Py_None); return Py_None; }
    if (item->unsigned_flag)
      return PyLong_FromUnsignedLongLong((ulonglong) v);
    return PyLong_FromLongLong(v);
  }
  case REAL_RESULT:
  {
    /* CAST(x AS DOUBLE), scientific notation (2.5e0), DOUBLE/FLOAT columns
       → Python float */
    double v= item->val_real();
    if (item->null_value) { Py_INCREF(Py_None); return Py_None; }
    return PyFloat_FromDouble(v);
  }
  case STRING_RESULT:
  {
    StringBuffer<256> buf;
    String *s= item->val_str(&buf);
    if (!s || item->null_value) { Py_INCREF(Py_None); return Py_None; }

    /*
      VECTOR columns → numpy.ndarray(float32) via the numpy C API.
      Uses PyArray_SimpleNew + memcpy for optimal performance:
      one allocation and one memcpy, no intermediate Python objects.
      The buffer is copied because MariaDB owns the original memory
      and may free it before the ndarray is used in Python code.
    */
    if (use_numpy &&
        item->type_handler() == &type_handler_vector)
    {
      npy_intp dims[1]= {(npy_intp)(s->length() / sizeof(float))};
      PyObject *arr= PyArray_SimpleNew(1, dims, NPY_FLOAT32);
      if (!arr) { Py_INCREF(Py_None); return Py_None; }
      memcpy(PyArray_DATA((PyArrayObject *) arr), s->ptr(), s->length());
      return arr;
    }

    /* VARCHAR/TEXT/CHAR columns and string literals → Python str
       BLOB/GEOMETRY columns, hex literals (X'...'), VARBINARY,
       and anything with binary charset → Python bytes */
    if (item->field_type() == MYSQL_TYPE_BLOB ||
        item->field_type() == MYSQL_TYPE_GEOMETRY ||
        item->collation.collation == &my_charset_bin)
      return PyBytes_FromStringAndSize(s->ptr(), s->length());
    return PyUnicode_FromStringAndSize(s->ptr(), s->length());
  }
  case DECIMAL_RESULT:
  {
    /*
      Bare decimal literals (2.5, 3.14) and DECIMAL/NUMERIC columns
      → Python decimal.Decimal for lossless precision.

      PyFloat_AsDouble() works on Decimal objects (calls __float__()),
      so the output path needs no change when DOUBLE return is requested.
      Falls back to Python float if the decimal module is unavailable.
    */
    StringBuffer<64> buf;
    String *s= item->val_str(&buf);
    if (!s || item->null_value) { Py_INCREF(Py_None); return Py_None; }

    PyObject *dec_mod= PyImport_ImportModule("decimal");
    if (!dec_mod)
    {
      PyErr_Clear();
      /* Fallback: convert to float if decimal module unavailable */
      double v= item->val_real();
      if (item->null_value) { Py_INCREF(Py_None); return Py_None; }
      return PyFloat_FromDouble(v);
    }

    PyObject *dec_cls= PyObject_GetAttrString(dec_mod, "Decimal");
    Py_DECREF(dec_mod);
    if (!dec_cls)
    {
      PyErr_Clear();
      double v= item->val_real();
      if (item->null_value) { Py_INCREF(Py_None); return Py_None; }
      return PyFloat_FromDouble(v);
    }

    PyObject *str_arg= PyUnicode_FromStringAndSize(s->ptr(), s->length());
    PyObject *result= PyObject_CallOneArg(dec_cls, str_arg);
    Py_DECREF(dec_cls);
    Py_DECREF(str_arg);

    if (!result)
    {
      PyErr_Clear();
      double v= item->val_real();
      if (item->null_value) { Py_INCREF(Py_None); return Py_None; }
      return PyFloat_FromDouble(v);
    }

    return result;
  }
  default:
  {
    /* Anything else: convert via string */
    StringBuffer<64> buf;
    String *s= item->val_str(&buf);
    if (!s || item->null_value) { Py_INCREF(Py_None); return Py_None; }
    return PyUnicode_FromStringAndSize(s->ptr(), s->length());
  }
  }
}
