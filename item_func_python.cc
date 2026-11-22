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
  Item_func_python_base method implementations.
*/

#include "item_func_python.h"

#include <string.h>  /* strlen, strcasecmp */


/* -------------------------------------------------------------------------
   Return-type parsing: one string table shared by every consumer.
   ------------------------------------------------------------------------- */

py_return_type lang_python_parse_return_type(const char *t)
{
  if (strcasecmp(t, "INT")     == 0 || strcasecmp(t, "INTEGER") == 0)
    return PY_RET_INT;
  if (strcasecmp(t, "DOUBLE")  == 0 || strcasecmp(t, "FLOAT")   == 0)
    return PY_RET_DOUBLE;
  if (strcasecmp(t, "VARCHAR") == 0 || strcasecmp(t, "TEXT")    == 0 ||
      strcasecmp(t, "STRING")  == 0)
    return PY_RET_VARCHAR;
  if (strcasecmp(t, "DECIMAL") == 0 || strcasecmp(t, "NUMERIC") == 0)
    return PY_RET_DECIMAL;
  if (strcasecmp(t, "BOOL")    == 0 || strcasecmp(t, "BOOLEAN") == 0)
    return PY_RET_BOOL;
  if (strcasecmp(t, "BLOB")    == 0)
    return PY_RET_BLOB;
  return PY_RET_BAD;
}

const char *lang_python_return_type_name(py_return_type t)
{
  switch (t)
  {
  case PY_RET_INT:     return "INT";
  case PY_RET_DOUBLE:  return "DOUBLE";
  case PY_RET_VARCHAR: return "VARCHAR";
  case PY_RET_DECIMAL: return "DECIMAL";
  case PY_RET_BOOL:    return "BOOL";
  case PY_RET_BLOB:    return "BLOB";
  case PY_RET_BAD:     break;
  }
  return "BAD";
}


bool Item_func_python_base::apply_return_type(py_return_type t,
                                              const char *type_str)
{
  return lang_python_apply_return_type(this, t, type_str, python_func_name());
}


/* -------------------------------------------------------------------------
   fix_length_and_dec: reads args[1] type string at prepare time.

   Supported return types: INT (INTEGER), DOUBLE (FLOAT),
   DECIMAL (NUMERIC), BOOL (BOOLEAN), VARCHAR (TEXT, STRING), BLOB.
   ------------------------------------------------------------------------- */

bool Item_func_python_base::fix_length_and_dec(THD *)
{
  /*
    Level 2 NULL handling: type argument must be a compile-time
    string constant.  We reject NULL and non-constant expressions
    here so the executor never has to deal with an unknown type.
  */
  if (!args[1]->const_item())
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0),
             "type argument must be a string constant");
    return true;
  }

  StringBuffer<32> type_buf;
  String *type_str= args[1]->val_str(&type_buf);

  if (!type_str || args[1]->null_value)
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0),
             "type argument cannot be NULL");
    return true;
  }

  const char *t= type_str->c_ptr_safe();
  return apply_return_type(lang_python_parse_return_type(t), t);
}


LEX_CSTRING Item_func_python_base::func_name_cstring() const
{
  LEX_CSTRING n;
  n.str=    python_func_name();
  n.length= strlen(python_func_name());
  return n;
}


/* -------------------------------------------------------------------------
   lang_python_dedent_source: strip common leading whitespace from a
   Python body.

   Users indent the Python code to match the surrounding SQL, which would
   otherwise raise IndentationError at compile time.  We reuse CPython's
   own textwrap.dedent() so the whitespace semantics match exactly: only
   the prefix shared by every non-blank line is removed, blank lines are
   ignored, and nested indentation is preserved.  It is a no-op for code
   that is already flush-left.

   GIL must be held.  Returns a new reference to a str the caller owns, or
   nullptr on any failure, in which case the caller falls back to the raw
   source, so dedent can never turn a runnable body into an error.
   ------------------------------------------------------------------------- */

PyObject *lang_python_dedent_source(const char *src)
{
  /* textwrap is cached in sys.modules after the first import. */
  PyObject *textwrap= PyImport_ImportModule("textwrap");
  if (!textwrap)
  {
    PyErr_Clear();
    return nullptr;
  }

  PyObject *dedent_fn= PyObject_GetAttrString(textwrap, "dedent");
  Py_DECREF(textwrap);
  if (!dedent_fn)
  {
    PyErr_Clear();
    return nullptr;
  }

  PyObject *src_obj= PyUnicode_FromString(src);
  PyObject *result= src_obj ? PyObject_CallOneArg(dedent_fn, src_obj)
                            : nullptr;
  Py_DECREF(dedent_fn);
  Py_XDECREF(src_obj);

  if (!result)
    PyErr_Clear();
  return result;
}


/* -------------------------------------------------------------------------
   Compiled-code cache (py_exec).  GIL held by the caller, except clear(),
   which takes it: an Item can be destroyed on a thread without the GIL.
   ------------------------------------------------------------------------- */

void Py_code_cache::clear()
{
  if (code && Py_IsInitialized())
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    Py_DECREF(code);
    PyGILState_Release(gs);
  }
  code= nullptr;
  source.clear();
}

PyObject *Item_func_python_base::cached_code(const String *src)
{
  if (m_cache.code && m_cache.source.size() == src->length() &&
      !memcmp(m_cache.source.data(), src->ptr(), src->length()))
    return m_cache.code;

  Py_CLEAR(m_cache.code);
  m_cache.source.assign(src->ptr(), src->length());

  /* Same preparation as the uncached path: dedent, fall back to the raw
     text; "<string>" is the file name PyRun_String reports. */
  PyObject *dedented= lang_python_dedent_source(m_cache.source.c_str());
  const char *text= dedented ? PyUnicode_AsUTF8(dedented) : nullptr;
  if (!text)
  {
    PyErr_Clear();
    text= m_cache.source.c_str();
  }
  m_cache.code= Py_CompileString(text, "<string>", compile_mode());
  Py_XDECREF(dedented);
  return m_cache.code;
}


/* -------------------------------------------------------------------------
   Core execution helper.
   GIL must be held by the caller.
   Runs Python, extracts the result object, handles all NULL levels
   except the final type conversion (done in each val_* method).

   Returns the result PyObject* or nullptr (null_value already set).
   out_owns: true → caller must Py_DECREF the returned pointer.
   ------------------------------------------------------------------------- */

PyObject *Item_func_python_base::execute(bool &out_owns)
{
  out_owns= false;

  /* Level 1: code/expr arg is NULL → SQL NULL, no warning */
  StringBuffer<256> code_buf;
  String *code= args[0]->val_str(&code_buf);
  if (!code || args[0]->null_value)
  {
    null_value= true;
    return nullptr;
  }

  /* Fresh isolated namespace: prevents leakage between calls */
  PyObject *globals= PyDict_New();
  PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins());

  /*
    Level 3: data args (args[2+]): NULL becomes Python None.
    The args tuple is always created even when empty so user code
    can safely do len(args) == 0 checks.
  */
  uint n_data= arg_count > 2 ? arg_count - 2 : 0;
  PyObject *tuple= PyTuple_New(n_data);
  for (uint i= 0; i < n_data; i++)
    PyTuple_SET_ITEM(tuple, i,
                     marshal_arg(args[i + 2],
                                 lang_python_use_numpy()));
  PyDict_SetItemString(globals, "args", tuple);
  Py_DECREF(tuple);

  /*
    Run Python.  Auto-dedent first so a body indented to match the
    surrounding SQL doesn't IndentationError; fall back to the raw source
    if dedent fails for any reason.
  */
  PyObject *run_ret;
  if (cache_code())
  {
    PyObject *compiled= cached_code(code);
    run_ret= compiled ? PyEval_EvalCode(compiled, globals, globals) : nullptr;
  }
  else
  {
    PyObject *dedented= lang_python_dedent_source(code->c_ptr_safe());
    const char *src= dedented ? PyUnicode_AsUTF8(dedented) : nullptr;
    if (!src)
    {
      PyErr_Clear();
      src= code->c_ptr_safe();
    }
    run_ret= PyRun_String(src, compile_mode(), globals, globals);
    Py_XDECREF(dedented);
  }
  PyObject *result_obj= nullptr;

  if (run_ret)
  {
    result_obj= extract_result(run_ret, globals);
    out_owns=   owns_result();

    /* Level 5: result absent or explicitly None → SQL NULL, no warning */
    if (!result_obj || result_obj == Py_None)
    {
      if (out_owns && result_obj)
        Py_DECREF(result_obj);
      result_obj= nullptr;
      out_owns=   false;
      null_value= true;
    }
    else if (!out_owns)
    {
      /*
        For py_exec, extract_result() returns a borrowed reference from
        the globals dict.  We must take our own reference before globals
        is destroyed below, otherwise we'd return a dangling pointer.
      */
      Py_INCREF(result_obj);
      out_owns= true;
    }
  }
  else
  {
    /*
      Level 4: Python raised an exception.
        strict_errors ON  → my_error, query aborts
        strict_errors OFF → push_warning, SQL NULL
    */
    if (lang_python_strict_errors(current_thd))
      raise_python_error();
    else
      emit_python_warning();
    null_value= true;
  }

  Py_DECREF(globals);
  return result_obj;
}


/* -------------------------------------------------------------------------
   *_op() methods: called by Item_func_hybrid_field_type's val_*().
   Each acquires the GIL, calls execute(), converts.
   Level 6 (type conversion failure) is handled per-method.
   ------------------------------------------------------------------------- */

double Item_func_python_base::real_op()
{
  PyGILState_STATE gs= PyGILState_Ensure();
  null_value= false;
  bool owns;
  PyObject *obj= execute(owns);
  double ret= obj ? unmarshal_real(obj, python_func_name(), null_value) : 0.0;
  if (owns) Py_DECREF(obj);
  PyGILState_Release(gs);
  return ret;
}

longlong Item_func_python_base::int_op()
{
  PyGILState_STATE gs= PyGILState_Ensure();
  null_value= false;
  bool owns;
  PyObject *obj= execute(owns);
  longlong ret= obj ? unmarshal_int(obj, python_func_name(), null_value) : 0;
  if (owns) Py_DECREF(obj);
  PyGILState_Release(gs);
  return ret;
}

String *Item_func_python_base::str_op(String *str)
{
  PyGILState_STATE gs= PyGILState_Ensure();
  null_value= false;
  bool owns;
  PyObject *obj= execute(owns);
  String *ret= obj ? unmarshal_str(obj, str, python_func_name(), null_value)
                   : nullptr;
  if (owns) Py_DECREF(obj);
  PyGILState_Release(gs);
  return ret;
}

my_decimal *Item_func_python_base::decimal_op(my_decimal *decimal_value)
{
  PyGILState_STATE gs= PyGILState_Ensure();
  null_value= false;
  bool owns;
  PyObject *obj= execute(owns);
  my_decimal *ret= obj ? unmarshal_decimal(obj, decimal_value,
                                           python_func_name(), null_value)
                       : nullptr;
  if (owns) Py_DECREF(obj);
  PyGILState_Release(gs);
  return ret;
}


/* -------------------------------------------------------------------------
   Python exception extraction.

   format_python_exception fetches the current Python exception and
   renders "<func>: <ExcType>: <message>" into buf.  Both the warning
   and error paths use it; after this returns, the Python exception
   state is cleared.
   ------------------------------------------------------------------------- */

void lang_python_format_exc(const char *prefix, char *buf, size_t buflen)
{
  PyObject *ptype, *pvalue, *ptraceback;
  PyErr_Fetch(&ptype, &pvalue, &ptraceback);

  const char *type_name= "UnknownError";
  PyObject   *tname_obj= nullptr;
  if (ptype)
  {
    tname_obj= PyObject_GetAttrString(ptype, "__name__");
    if (tname_obj) type_name= PyUnicode_AsUTF8(tname_obj);
  }

  PyObject   *pstr= pvalue ? PyObject_Str(pvalue) : nullptr;
  const char *msg=  pstr   ? PyUnicode_AsUTF8(pstr) : nullptr;

  snprintf(buf, buflen, "%s: %s: %s",
           prefix, type_name, msg ? msg : "(no message)");

  Py_XDECREF(pstr);
  Py_XDECREF(tname_obj);
  Py_XDECREF(ptype);
  Py_XDECREF(pvalue);
  Py_XDECREF(ptraceback);
}

void Item_func_python_base::format_python_exception(char *buf, size_t buflen)
{
  lang_python_format_exc(python_func_name(), buf, buflen);
}

void Item_func_python_base::emit_python_warning()
{
  char buf[512];
  format_python_exception(buf, sizeof(buf));
  push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                      ER_UNKNOWN_ERROR, "%s", buf);
}

void Item_func_python_base::raise_python_error()
{
  char buf[512];
  format_python_exception(buf, sizeof(buf));
  my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), buf);
}


/* -------------------------------------------------------------------------
   Create_func implementations
   ------------------------------------------------------------------------- */

Item *Item_func_py_eval::Create_func::create_native(
    THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
{
  uint n= item_list ? item_list->elements : 0;
  if (n < 2)   /* expr + type required; data args optional */
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return nullptr;
  }
  return new (thd->mem_root) Item_func_py_eval(thd, *item_list);
}

Item *Item_func_py_exec::Create_func::create_native(
    THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
{
  uint n= item_list ? item_list->elements : 0;
  if (n < 2)   /* code + type required; data args optional */
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return nullptr;
  }
  return new (thd->mem_root) Item_func_py_exec(thd, *item_list);
}
