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

#ifndef LANG_PYTHON_ITEM_FUNC_PY_NAMED_H
#define LANG_PYTHON_ITEM_FUNC_PY_NAMED_H

/*
  Named Python functions (Phase 1.5): SQL surface.

  py_create_function(name, return_type, param_names, body
                     [, strict_errors [, deterministic]])
  py_replace_function(name, return_type, param_names, body
                      [, strict_errors [, deterministic]])
  py_drop_function(name [, if_exists])
  py_call(name, arg0, arg1, ...)
  py_create_aggregate(name, return_type, param_names, body [, strict_errors])
  py_replace_aggregate(name, return_type, param_names, body [, strict_errors])

  py_create_function: validates and compiles the body immediately
    (syntax errors surface at create time), persists it in
    mysql.lang_python_func, and publishes the compiled code object in the
    in-memory cache.  Errors if the name already exists.

    param_names: comma-separated ('x,y') binds arguments as named
    parameters; NULL exposes them as the tuple 'args' instead.  The body
    is module-level code with the py_exec convention: it assigns its
    answer to `result`; an absent or None result returns SQL NULL, and
    `return` at top level is a create-time SyntaxError.

    strict_errors is frozen into the function at create time (default 1):
    it decides whether a runtime Python exception in THIS function aborts
    the query or yields NULL + warning, regardless of the session
    lang_python_strict_errors setting.

    deterministic (default 0) declares that the function returns the
    same result for the same arguments and has no side effects.  It is a
    promise of the creator and is not checked.  py_call of a
    deterministic function with constant arguments is a constant for the
    optimizer: it can be evaluated once for the statement and used for
    an index lookup.  NULL for strict_errors or deterministic keeps the
    default.

  py_replace_function: same, but create-or-overwrite.

  py_drop_function: removes the function.  Errors if absent unless
    if_exists is given and true.

  py_create_aggregate / py_replace_aggregate: the arguments up to
    strict_errors (an aggregate has no deterministic declaration), but the
    entry has kind 'aggregate': its body defines a class Aggregate with
    step() and finalize(), which py_agg feeds one chunk of rows at a time
    (see item_sum_py_agg.h, docs/AGGREGATE_PROTOCOL.md).  Functions and
    aggregates share one namespace; py_drop_function drops either kind.

  py_call: invokes a named function.  The function name must be a
    string constant (it is resolved at prepare time, when the return
    type is taken from the catalog).  Arguments are marshaled exactly
    like py_eval data args; the return value is converted to the
    function's declared return type with the same NULL/error levels.

    Examples:
      SELECT py_create_function('multiply', 'INT', 'x,y', 'return x * y');
      SELECT py_call('multiply', 3, 4);              -- 12
      SELECT py_drop_function('multiply');
*/

#include "item_func_python.h"
#include "py_func_catalog.h"


/* =========================================================================
   Item_func_py_call: invoke a named function from the catalog cache
   ========================================================================= */

class Item_func_py_call : public Item_func_python_base
{
  using Self= Item_func_py_call;

  /* Resolved at prepare time; a private strong reference, so concurrent
     py_drop/py_replace cannot free the code object mid-execution. */
  PyFuncEntryPtr m_entry;

public:
  using Item_func_python_base::Item_func_python_base;

  const char *python_func_name() const override { return "py_call"; }
  int         compile_mode()     const override { return Py_file_input; }
  bool        owns_result()      const override { return true; }

  PyObject *extract_result(PyObject *run_ret, PyObject *) override
  { return run_ret; }

  bool fix_length_and_dec(THD *) override;

  /*
    A function declared deterministic follows the rules of a built-in
    function for constness: constant when its arguments are.  It stays
    expensive, so it is not evaluated while the statement is optimized,
    and it is not pushed into the storage engine.  As for
    Item_func_sp with a DETERMINISTIC routine.
  */
  bool const_item() const override
  { return deterministic() && Item_func::const_item(); }
  table_map used_tables() const override
  {
    return deterministic() ? Item_func::used_tables()
                           : Item_func_python_base::used_tables();
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }

  class Create_func : public Create_native_func
  {
  public:
    using Create_native_func::Create_native_func;
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override;
  };

  static Plugin_function *plugin_descriptor()
  {
    static Create_func     creator;
    static Plugin_function descriptor(&creator);
    return &descriptor;
  }

protected:
  PyObject *execute(bool &out_owns) override;

private:
  bool deterministic() const { return m_entry && m_entry->deterministic; }
};


/* =========================================================================
   Item_func_py_manage: py_create_function / py_replace_function /
   py_drop_function.  One Item class, three registered creators; returns
   1 on success, raises an error otherwise.
   ========================================================================= */

class Item_func_py_manage : public Item_int_func
{
public:
  enum Mode { FN_CREATE, FN_REPLACE, FN_DROP, AGG_CREATE, AGG_REPLACE };

  Item_func_py_manage(THD *thd, List<Item> &list, Mode mode)
    : Item_int_func(thd, list), m_mode(mode) {}

  longlong val_int() override;
  LEX_CSTRING func_name_cstring() const override;

  /* The call changes the catalog: never a constant, never folded */
  LANG_PYTHON_OPTIMIZER_CONTRACT

  const Type_handler *type_handler() const override
  { return &type_handler_slonglong; }

  bool fix_length_and_dec(THD *) override
  {
    max_length= 1;
    return false;
  }

  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Item_func_py_manage>(thd, this); }

  static Plugin_function *create_descriptor();
  static Plugin_function *replace_descriptor();
  static Plugin_function *drop_descriptor();
  static Plugin_function *create_aggregate_descriptor();
  static Plugin_function *replace_aggregate_descriptor();

private:
  Mode m_mode;

  template <Mode M, uint MIN_ARGS, uint MAX_ARGS>
  class Create_func_tmpl : public Create_native_func
  {
  public:
    using Create_native_func::Create_native_func;
    Item *create_native(THD *thd, const LEX_CSTRING *name,
                        List<Item> *item_list) override
    {
      uint n= item_list ? item_list->elements : 0;
      if (n < MIN_ARGS || n > MAX_ARGS)
      {
        my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
        return nullptr;
      }
      return new (thd->mem_root) Item_func_py_manage(thd, *item_list, M);
    }
  };
};

#endif /* LANG_PYTHON_ITEM_FUNC_PY_NAMED_H */
