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

#ifndef LANG_PYTHON_ITEM_FUNC_H
#define LANG_PYTHON_ITEM_FUNC_H

/*
  Item_func_python_base, Item_func_py_eval, Item_func_py_exec

  Base class and concrete subclasses implementing the SQL function
  interface for py_eval() and py_exec().  All Python execution, type
  negotiation, and result conversion lives here.

  py_eval(expr, type [, arg0, arg1, ...]) RETURNS <type>
  py_exec(code, type [, arg0, arg1, ...]) RETURNS <type>

  Two complementary functions for embedding Python in MariaDB SQL.

  Arguments:
    expr / code  VARCHAR   required  Python expression or code block
    type         VARCHAR   required  declared return type: INT, DOUBLE,
                                     VARCHAR, BLOB
    arg0, ...    any       optional  data parameters, available in Python
                                     as the tuple 'args'

  py_eval: evaluates a single Python expression.
    The expression value IS the result.

    Examples:
      SELECT py_eval('2 ** 32',          'INT');
      SELECT py_eval('args[0] * args[1]','DOUBLE', price, quantity);

  py_exec: executes a Python code block.
    The block must assign its answer to a variable named 'result'.
    If 'result' is absent or None, SQL NULL is returned.

    Examples:
      SELECT py_exec('result = 2 ** 32', 'INT');
      SELECT py_exec('
import math
result = math.sqrt(args[0] ** 2 + args[1] ** 2)
', 'DOUBLE', side_a, side_b);

  NULL handling:
    Level 1: code/expr arg is NULL         → SQL NULL returned silently
    Level 2: type arg is NULL or non-const → error at prepare time
    Level 3: data arg is NULL              → Python sees None in args tuple
    Level 4: Python raises an exception    → strict ON: query fails (default)
                                             strict OFF: SQL NULL + warning
    Level 5: result absent / None          → SQL NULL returned silently
    Level 6: result cannot convert to type → strict ON: query fails (default)
                                             strict OFF: SQL NULL + warning

    Strict mode is controlled by the lang_python_strict_errors session
    variable (default ON).

  Compilation:
    py_eval compiles its expression on every call.  py_exec compiles its
    block on the first call and reuses the code object for the following
    rows while the source text is unchanged (Py_code_cache).  Named
    functions (py_call) compile once, at create time.

  Isolation:
    Each call runs in a fresh, private namespace (new dict + builtins).
    No state leaks between calls or connections.

  Thread safety:
    Interpreter initialised once at plugin load; GIL released after init.
    Each call re-acquires via PyGILState_Ensure / PyGILState_Release.
    The Python thread state is created on the first call of the statement
    and kept until cleanup() (Py_thread_pin); the GIL is not held between
    calls.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>
#include <mysql/plugin_function.h>

#include "lang_python.h"
#include "marshal.h"
#include "unmarshal.h"
#include "py_thread_pin.h"

#include <string>

/* =========================================================================
   Return-type negotiation: shared by py_eval/py_exec (whose type comes
   from the SQL type argument) and py_call (whose type comes from the
   catalog row).
   ========================================================================= */

enum py_return_type
{
  PY_RET_BAD= 0,
  PY_RET_INT,        /* INT, INTEGER          */
  PY_RET_DOUBLE,     /* DOUBLE, FLOAT         */
  PY_RET_VARCHAR,    /* VARCHAR, TEXT, STRING */
  PY_RET_DECIMAL,    /* DECIMAL, NUMERIC      */
  PY_RET_BOOL,       /* BOOL, BOOLEAN         */
  PY_RET_BLOB        /* BLOB                  */
};

py_return_type lang_python_parse_return_type(const char *t);
const char    *lang_python_return_type_name(py_return_type t);

/*
  Apply a parsed return type to an Item with a hybrid type handler
  (handler, length, decimals, charset).  Shared by the scalar
  functions.  PY_RET_BAD raises the error and returns true.
*/
template <class ITEM>
bool lang_python_apply_return_type(ITEM *item, py_return_type t,
                                   const char *type_str, const char *func)
{
  switch (t)
  {
  case PY_RET_INT:
    item->set_handler(&type_handler_slonglong);
    item->max_length= MY_INT64_NUM_DECIMAL_DIGITS;
    break;
  case PY_RET_DOUBLE:
    item->set_handler(&type_handler_double);
    item->decimals= NOT_FIXED_DEC;
    break;
  case PY_RET_VARCHAR:
    item->set_handler(&type_handler_varchar);
    item->max_length= MAX_BLOB_WIDTH;
    /*
      Coercible, like the result of a built-in string function.  The
      hybrid function class defaults to the numeric derivation, which
      makes every comparison with a string of another character set an
      illegal mix of collations.
    */
    item->collation.set(system_charset_info, DERIVATION_COERCIBLE);
    break;
  case PY_RET_DECIMAL:
    item->set_handler(&type_handler_newdecimal);
    /*
      Use maximum DECIMAL precision.  The result is converted via its
      string representation and str2my_decimal(), so precision is
      preserved losslessly (no double round-trip).
    */
    item->decimals= NOT_FIXED_DEC;
    item->max_length= DECIMAL_MAX_STR_LENGTH;
    break;
  case PY_RET_BOOL:
    /*
      BOOL return type: Python truthy/falsy -> 1/0.  Uses the bool type
      handler so MariaDB displays TRUE/FALSE semantics.
    */
    item->set_handler(&type_handler_bool);
    break;
  case PY_RET_BLOB:
    item->set_handler(&type_handler_blob);
    item->max_length= MAX_BLOB_WIDTH;
    break;
  case PY_RET_BAD:
    my_error(ER_WRONG_VALUE_FOR_TYPE, MYF(0), "type", type_str, func);
    return true;
  }

  item->set_maybe_null();
  return false;
}


/* =========================================================================
   Compiled-code cache of one SQL function instance (py_exec).

   Holds the code object compiled from the source text of the last call.
   While the source stays the same (the usual case: a string constant),
   every further row reuses it instead of dedenting and compiling again.
   The cache belongs to one Item: no sharing between statements or
   connections, so no locking and no size policy.

   Copying an Item does not copy the cache (the copy compiles on its first
   call); the reference is released under the GIL.
   ========================================================================= */

struct Py_code_cache
{
  PyObject *code= nullptr;                /* strong ref */
  std::string source;                     /* text `code` was compiled from */

  Py_code_cache() = default;
  Py_code_cache(const Py_code_cache &) {}
  Py_code_cache &operator=(const Py_code_cache &) { return *this; }
  ~Py_code_cache() { clear(); }
  void clear();
};


/* =========================================================================
   Optimizer contract of every SQL function of the plugin.

   A Python body can have side effects, can return a different value on
   every call, and costs far more than a built-in function.  The server
   assumes the opposite of a function Item unless told otherwise.  With
   these overrides (the ones Item_func_sp and Item_func_sleep use):

     const_item(), used_tables()   the call is never a constant, also with
                                   constant arguments: it is not evaluated
                                   while the statement is optimized and not
                                   folded into one value for all rows
     is_expensive()                the call is not evaluated to estimate or
                                   simplify conditions; cheaper conditions
                                   are tested first
     limit_index_condition_pushdown_processor()
                                   the call is not pushed into a storage
                                   engine as an index condition, where it
                                   would run inside the engine's index scan

     check_vcol_func_processor()   the call is marked non-deterministic, as
                                   RAND() is: the server then refuses it
                                   where a value is stored or indexed
   ========================================================================= */

#define LANG_PYTHON_OPTIMIZER_CONTRACT                                      \
  bool const_item() const override { return false; }                        \
  table_map used_tables() const override                                    \
  { return used_tables_cache | RAND_TABLE_BIT; }                            \
  bool is_expensive() override { return true; }                             \
  bool is_expensive_processor(void *) override { return true; }             \
  bool limit_index_condition_pushdown_processor(void *) override            \
  { return true; }                                                          \
  bool check_vcol_func_processor(void *arg) override                        \
  {                                                                         \
    return mark_unsupported_function(func_name(), "()", arg,                \
                                     VCOL_NON_DETERMINISTIC);               \
  }


/* =========================================================================
   Item_func_python_base
   ========================================================================= */

class Item_func_python_base : public Item_func_hybrid_field_type
{
public:
  using Item_func_hybrid_field_type::Item_func_hybrid_field_type;

  LANG_PYTHON_OPTIMIZER_CONTRACT

  /* Subclass interface */
  virtual const char *python_func_name() const = 0;
  virtual int         compile_mode()     const = 0;
  virtual PyObject   *extract_result(PyObject *run_ret,
                                     PyObject *globals) = 0;
  virtual bool        owns_result()  const = 0;
  /* Keep the compiled code between calls.  py_exec: yes.  py_eval: no,
     it is the one-off form; named functions are the form for reuse. */
  virtual bool        cache_code()   const { return false; }

  bool fix_length_and_dec(THD *) override;
  LEX_CSTRING func_name_cstring() const override;
  void cleanup() override;

  double      real_op()    override;
  longlong    int_op()     override;
  String     *str_op(String *str) override;
  my_decimal *decimal_op(my_decimal *decimal_value) override;

  bool date_op(THD *, MYSQL_TIME *, date_mode_t) override
  { null_value= true; return true; }

  bool time_op(THD *, MYSQL_TIME *) override
  { null_value= true; return true; }

  bool native_op(THD *, Native *) override
  { null_value= true; return true; }

  Type_ref_null ref_op(THD *) override
  { return Type_ref_null(); }

protected:
  /* py_call overrides this to evaluate a cached code object instead of
     compiling args[0]; everything in the val_* methods is shared. */
  virtual PyObject *execute(bool &out_owns);

  /* Apply a parsed return type to this Item (handler, length, charset).
     PY_RET_BAD raises the error and returns true. */
  bool apply_return_type(py_return_type t, const char *type_str);

  /*
    Format the currently-raised Python exception ("type: message") into
    buf, clearing the exception state.  Shared by the warning and error
    code paths below.
  */
  void format_python_exception(char *buf, size_t buflen);
  void emit_python_warning();
  void raise_python_error();

private:
  Py_thread_pin m_pin;                    /* thread state of this statement */
  Py_code_cache m_cache;
  /* Compile (or reuse) the code for src; nullptr with a Python exception
     set on a compile error.  Borrowed reference, valid until the next
     call.  GIL held. */
  PyObject *cached_code(const String *src);
};


/* =========================================================================
   Item_func_py_eval: single expression
   ========================================================================= */

class Item_func_py_eval : public Item_func_python_base
{
  using Self= Item_func_py_eval;
public:
  using Item_func_python_base::Item_func_python_base;

  const char *python_func_name() const override { return "py_eval"; }
  int         compile_mode()     const override { return Py_eval_input; }
  bool        owns_result()      const override { return true; }

  PyObject *extract_result(PyObject *run_ret, PyObject *) override
  { return run_ret; }

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
};


/* =========================================================================
   Item_func_py_exec: code block, result via "result" variable
   ========================================================================= */

class Item_func_py_exec : public Item_func_python_base
{
  using Self= Item_func_py_exec;
public:
  using Item_func_python_base::Item_func_python_base;

  const char *python_func_name() const override { return "py_exec"; }
  int         compile_mode()     const override { return Py_file_input; }
  bool        owns_result()      const override { return false; }
  bool        cache_code()       const override { return true; }

  PyObject *extract_result(PyObject *run_ret, PyObject *globals) override
  {
    Py_DECREF(run_ret);
    return PyDict_GetItem(globals, py_name.result);
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
};

#endif /* LANG_PYTHON_ITEM_FUNC_H */
