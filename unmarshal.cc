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
  Output unmarshaling: Python object → MariaDB result.

  Each function converts a Python object to the corresponding MariaDB
  type.  On conversion failure (Level 6 in the NULL handling spec), the
  failure is reported as either a MariaDB error (strict mode) or a
  warning + SQL NULL (lax mode), depending on lang_python_strict_errors.

  GIL must be held by the caller.  The functions do NOT Py_DECREF obj;
  ownership stays with the caller.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>

#include <stdio.h>   /* snprintf */
#include <string.h>  /* strlen */

#include "lang_python.h"
#include "unmarshal.h"

/* -------------------------------------------------------------------------
   Helpers
   ------------------------------------------------------------------------- */

static void emit_conversion_warning(const char *func_name,
                                    const char *target_type)
{
  THD *thd= current_thd;
  if (lang_python_strict_errors(thd))
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "%s: cannot convert result to %s",
                    MYF(0), func_name, target_type);
    return;
  }
  push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_UNKNOWN_ERROR,
                      "%s: cannot convert result to %s, returning NULL",
                      func_name, target_type);
}


/* -------------------------------------------------------------------------
   unmarshal_real: Python object → double
   ------------------------------------------------------------------------- */

double unmarshal_real(PyObject *obj, const char *func_name, bool &null_value)
{
  double ret= PyFloat_AsDouble(obj);
  if (PyErr_Occurred())
  {
    emit_conversion_warning(func_name, "DOUBLE");
    PyErr_Clear();
    null_value= true;
    return 0.0;
  }
  return ret;
}


/* -------------------------------------------------------------------------
   unmarshal_int: Python object → longlong

   If the object is not an int but is a float, truncates with a note
   (matching SQL implicit conversion rules).
   ------------------------------------------------------------------------- */

longlong unmarshal_int(PyObject *obj, const char *func_name, bool &null_value)
{
  longlong ret= PyLong_AsLongLong(obj);
  if (!PyErr_Occurred())
    return ret;

  PyErr_Clear();

  /* Coerce: float → int truncation, matching SQL implicit rules. */
  double d= PyFloat_AsDouble(obj);
  if (!PyErr_Occurred())
  {
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_UNKNOWN_ERROR,
                        "%s: float result truncated to INT",
                        func_name);
    return (longlong) d;
  }

  PyErr_Clear();
  emit_conversion_warning(func_name, "INT");
  null_value= true;
  return 0;
}


/* -------------------------------------------------------------------------
   unmarshal_str: Python object → String

   bytes / bytearray → binary (BLOB/VARBINARY)
   str / other        → text via str(), using PyUnicode_AsUTF8AndSize
                         to handle embedded null bytes correctly
   ------------------------------------------------------------------------- */

String *unmarshal_str(PyObject *obj, String *str, const char *func_name,
                      bool &null_value)
{
  if (PyBytes_Check(obj))
  {
    char *buf;
    Py_ssize_t len;
    PyBytes_AsStringAndSize(obj, &buf, &len);
    if (!str->copy(buf, (uint32) len, &my_charset_bin))
      return str;
    null_value= true;
    return nullptr;
  }

  if (PyByteArray_Check(obj))
  {
    char *buf= PyByteArray_AS_STRING(obj);
    Py_ssize_t len= PyByteArray_GET_SIZE(obj);
    if (!str->copy(buf, (uint32) len, &my_charset_bin))
      return str;
    null_value= true;
    return nullptr;
  }

  /* str or other object → text via str() */
  PyObject *str_obj= PyObject_Str(obj);
  if (str_obj)
  {
    Py_ssize_t len;
    const char *cstr= PyUnicode_AsUTF8AndSize(str_obj, &len);
    if (cstr && !str->copy(cstr, (uint32) len, system_charset_info))
    {
      Py_DECREF(str_obj);
      return str;
    }
    Py_DECREF(str_obj);
    null_value= true;
    return nullptr;
  }

  emit_conversion_warning(func_name, "VARCHAR");
  PyErr_Clear();
  null_value= true;
  return nullptr;
}


/* -------------------------------------------------------------------------
   unmarshal_decimal: Python object → my_decimal

   Converts via the string representation to preserve precision.
   This matches how the C connector and Python DB-API connectors handle
   DECIMAL: always through str(), never through double.
   ------------------------------------------------------------------------- */

my_decimal *unmarshal_decimal(PyObject *obj, my_decimal *dec,
                              const char *func_name, bool &null_value)
{
  PyObject *str_obj= PyObject_Str(obj);
  if (!str_obj)
  {
    emit_conversion_warning(func_name, "DECIMAL");
    PyErr_Clear();
    null_value= true;
    return nullptr;
  }

  const char *cstr= PyUnicode_AsUTF8(str_obj);
  if (!cstr)
  {
    Py_DECREF(str_obj);
    emit_conversion_warning(func_name, "DECIMAL");
    PyErr_Clear();
    null_value= true;
    return nullptr;
  }

  int rc= str2my_decimal(E_DEC_FATAL_ERROR, cstr, strlen(cstr),
                         system_charset_info, dec);
  Py_DECREF(str_obj);

  if (rc)
  {
    emit_conversion_warning(func_name, "DECIMAL");
    null_value= true;
    return nullptr;
  }

  return dec;
}
