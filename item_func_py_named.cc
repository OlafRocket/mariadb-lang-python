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
  Named Python functions: py_call and the management functions.
*/

#include "item_func_py_named.h"

#include <string.h>
#include <ctype.h>


/* -------------------------------------------------------------------------
   Input validation
   ------------------------------------------------------------------------- */

static bool valid_func_name(const char *s)
{
  if (!*s || strlen(s) > 64)
    return false;
  if (!isalpha((uchar) *s) && *s != '_')
    return false;
  for (const char *p= s + 1; *p; p++)
    if (!isalnum((uchar) *p) && *p != '_')
      return false;
  return true;
}

/*
  Only identifier characters, commas and spaces.  The definitive check is
  Py_CompileString on the generated "def __pyfn(<params>):" header, which
  rejects anything that is not a valid parameter list; this guard merely
  keeps code injection (e.g. a ')' that would close the def) out of the
  generated source.
*/
static bool valid_param_names(const char *s)
{
  for (const char *p= s; *p; p++)
    if (!isalnum((uchar) *p) && *p != '_' && *p != ',' && *p != ' ')
      return false;
  return true;
}


/* =========================================================================
   Item_func_py_call
   ========================================================================= */

bool Item_func_py_call::fix_length_and_dec(THD *)
{
  char err[512];

  /*
    The function name must be resolvable at prepare time: the catalog
    entry decides this Item's return type.
  */
  if (!args[0]->const_item())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_call: function name must be a string constant",
                    MYF(0));
    return true;
  }

  StringBuffer<64> name_buf;
  String *name= args[0]->val_str(&name_buf);
  if (!name || args[0]->null_value)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_call: function name cannot be NULL", MYF(0));
    return true;
  }

  if (py_catalog_ensure(err, sizeof(err)))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "py_call: %s", MYF(0), err);
    return true;
  }

  if (!(m_entry= py_catalog_lookup(name->c_ptr_safe())))
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_call: unknown Python function '%s'", MYF(0),
                    name->c_ptr_safe());
    return true;
  }

  if (m_entry->is_aggregate)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_call: '%s' is an aggregate; aggregates are not "
                    "supported by this plugin version", MYF(0),
                    m_entry->name.c_str());
    return true;
  }

  return apply_return_type(
      lang_python_parse_return_type(m_entry->return_type.c_str()),
      m_entry->return_type.c_str());
}


/* -------------------------------------------------------------------------
   Execution.  GIL must be held by the caller (the val_* methods of the
   base class take it).  Mirrors Item_func_python_base::execute(), but
   evaluates the cached code object instead of compiling args[0]: no
   per-row compile.  The body is plain module-level code with py_exec
   semantics (deliberately NOT def-wrapped): named parameters are
   injected into the namespace (or the `args` tuple in args[] mode) and
   the answer is read back from the `result` variable.
   ------------------------------------------------------------------------- */

PyObject *Item_func_py_call::execute(bool &out_owns)
{
  out_owns= false;

  PyFuncEntryPtr entry= m_entry;
  if (!entry)                                    /* defensive; fixed at prepare */
  {
    null_value= true;
    return nullptr;
  }

  /*
    Arity is structural in named-parameter mode: a mismatch is caller
    error, reported regardless of the strict_errors setting.
  */
  uint n_data= arg_count - 1;
  if (entry->has_params && n_data != entry->params.size())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_call: function '%s' expects %zu arguments, got %u",
                    MYF(0), entry->name.c_str(), entry->params.size(),
                    n_data);
    null_value= true;
    return nullptr;
  }

  /* Fresh isolated namespace: prevents leakage between calls */
  PyObject *globals= PyDict_New();
  PyDict_SetItem(globals, py_name.builtins, PyEval_GetBuiltins());

  /* Marshal SQL args (NULL → None): named globals or the args tuple */
  if (entry->has_params)
  {
    for (uint i= 0; i < n_data; i++)
    {
      PyObject *v= marshal_arg(args[i + 1], lang_python_use_numpy());
      PyDict_SetItem(globals, entry->param_objs[i], v);
      Py_DECREF(v);
    }
  }
  else
  {
    PyObject *tuple= PyTuple_New(n_data);
    for (uint i= 0; i < n_data; i++)
      PyTuple_SET_ITEM(tuple, i,
                       marshal_arg(args[i + 1], lang_python_use_numpy()));
    PyDict_SetItem(globals, py_name.args, tuple);
    Py_DECREF(tuple);
  }

  PyObject *result_obj= nullptr;
  PyObject *run_ret= PyEval_EvalCode(entry->code, globals, globals);
  if (run_ret)
  {
    Py_DECREF(run_ret);

    /* Level 5: result absent or None → SQL NULL, no warning */
    result_obj= PyDict_GetItem(globals, py_name.result);  /* borrowed */
    if (!result_obj || result_obj == Py_None)
    {
      result_obj= nullptr;
      null_value= true;
    }
    else
    {
      /* own it before globals dies below */
      Py_INCREF(result_obj);
      out_owns= true;
    }
  }
  else
  {
    /*
      Level 4: Python raised.  Unlike the inline functions, strictness
      was frozen into the catalog row at create time.
    */
    if (entry->strict_errors)
      raise_python_error();
    else
      emit_python_warning();
    null_value= true;
  }

  Py_DECREF(globals);
  return result_obj;
}


Item *Item_func_py_call::Create_func::create_native(
    THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
{
  uint n= item_list ? item_list->elements : 0;
  if (n < 1)   /* function name required; data args optional */
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return nullptr;
  }
  return new (thd->mem_root) Item_func_py_call(thd, *item_list);
}


/* =========================================================================
   Item_func_py_manage
   ========================================================================= */

LEX_CSTRING Item_func_py_manage::func_name_cstring() const
{
  const char *n;
  switch (m_mode)
  {
  case FN_CREATE:   n= "py_create_function";   break;
  case FN_REPLACE:  n= "py_replace_function";  break;
  default:          n= "py_drop_function";     break;
  }
  LEX_CSTRING ret;
  ret.str=    n;
  ret.length= strlen(n);
  return ret;
}


longlong Item_func_py_manage::val_int()
{
  char err[512];
  null_value= false;

  StringBuffer<64> name_buf;
  String *name= args[0]->val_str(&name_buf);
  if (!name || args[0]->null_value ||
      !valid_func_name(name->c_ptr_safe()))
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "%s: function name must be an identifier of at most "
                    "64 characters [A-Za-z0-9_]", MYF(0), func_name());
    return 0;
  }
  const char *fname= name->c_ptr_safe();

  if (m_mode == FN_DROP)
  {
    bool if_exists= false;
    if (arg_count > 1)
    {
      longlong v= args[1]->val_int();
      if_exists= !args[1]->null_value && v != 0;
    }
    if (py_catalog_drop(fname, if_exists, err, sizeof(err)))
    {
      my_printf_error(ER_UNKNOWN_ERROR, "%s: %s", MYF(0), func_name(), err);
      return 0;
    }
    return 1;
  }

  /* create / replace: (name, return_type, param_names, body [, strict]) */

  StringBuffer<32> type_buf;
  String *type_str= args[1]->val_str(&type_buf);
  if (!type_str || args[1]->null_value)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "%s: return type cannot be NULL", MYF(0), func_name());
    return 0;
  }
  py_return_type rtype=
    lang_python_parse_return_type(type_str->c_ptr_safe());
  if (rtype == PY_RET_BAD)
  {
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0),
             "type", type_str->c_ptr_safe(), func_name());
    return 0;
  }

  StringBuffer<256> params_buf;
  String *params= args[2]->val_str(&params_buf);
  const char *params_str=
    (params && !args[2]->null_value) ? params->c_ptr_safe() : nullptr;
  if (params_str && !valid_param_names(params_str))
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "%s: param_names must be a comma-separated list of "
                    "identifiers", MYF(0), func_name());
    return 0;
  }

  StringBuffer<1024> body_buf;
  String *body= args[3]->val_str(&body_buf);
  if (!body || args[3]->null_value)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "%s: body cannot be NULL", MYF(0), func_name());
    return 0;
  }

  bool strict= true;
  if (arg_count > 4)
  {
    longlong v= args[4]->val_int();
    if (!args[4]->null_value)
      strict= v != 0;
  }

  bool deterministic= false;
  if (arg_count > 5)
  {
    longlong v= args[5]->val_int();
    if (!args[5]->null_value)
      deterministic= v != 0;
  }

  if (py_catalog_create(fname, lang_python_return_type_name(rtype),
                        params_str, body->c_ptr_safe(), strict,
                        false, deterministic, m_mode == FN_REPLACE,
                        err, sizeof(err)))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "%s: %s", MYF(0), func_name(), err);
    return 0;
  }
  return 1;
}


Plugin_function *Item_func_py_manage::create_descriptor()
{
  static Create_func_tmpl<FN_CREATE, 4, 6> creator;
  static Plugin_function descriptor(&creator);
  return &descriptor;
}

Plugin_function *Item_func_py_manage::replace_descriptor()
{
  static Create_func_tmpl<FN_REPLACE, 4, 6> creator;
  static Plugin_function descriptor(&creator);
  return &descriptor;
}

Plugin_function *Item_func_py_manage::drop_descriptor()
{
  static Create_func_tmpl<FN_DROP, 1, 2> creator;
  static Plugin_function descriptor(&creator);
  return &descriptor;
}
