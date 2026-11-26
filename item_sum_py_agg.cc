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
  py_agg: batching aggregate (see item_sum_py_agg.h).
*/

#include "item_sum_py_agg.h"
#include "py_columns.h"

#include <limits>


void Item_sum_py_agg::State::drop_result()
{
  if (result)
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    Py_DECREF(result);
    PyGILState_Release(gs);
    result= nullptr;
  }
  computed= false;
}

void Item_sum_py_agg::State::drop_instance()
{
  if (inst)
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    Py_DECREF(inst);
    PyGILState_Release(gs);
    inst= nullptr;
  }
}

void Item_sum_py_agg::State::reset_buffers()
{
  rows= 0;
  bytes= 0;
  for (Rowsink_column &c : cols)
  {
    c.ints.clear();
    c.null_rows.clear();
    c.doubles.clear();
    c.floats.clear();
    c.dim= 0;
    c.bytes.clear();
    c.ends.clear();
    c.tags.clear();
  }
}

Item_sum_py_agg::State::~State()
{
  drop_result();
  drop_instance();
  if (ns || cls)
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    Py_XDECREF(cls);
    Py_XDECREF(ns);
    PyGILState_Release(gs);
  }
}


/*
  Item_sum_plugin offers only single-argument constructors.  Construct
  with the first argument, then install the whole list the way
  Item_sum(THD *, List<Item> &) does.
*/
Item_sum_py_agg::Item_sum_py_agg(THD *thd, List<Item> &list)
  : Item_sum_plugin(thd, list.head()),
    Type_handler_hybrid_field_type(&type_handler_slonglong)
{
  set_arguments(thd, list);
  if (arg_count > 2 && !(orig_args= thd->alloc<Item *>(arg_count)))
    args= nullptr;
  list.empty();
}


Item *Item_sum_py_agg::Create_func::create_native(
    THD *thd, const LEX_CSTRING *name, List<Item> *item_list)
{
  uint n= item_list ? item_list->elements : 0;
  if (n < 2)                              /* function name + one column */
  {
    my_error(ER_WRONG_PARAMCOUNT_TO_NATIVE_FCT, MYF(0), name->str);
    return nullptr;
  }
  return new (thd->mem_root) Item_sum_py_agg(thd, *item_list);
}


bool Item_sum_py_agg::fix_length_and_dec(THD *)
{
  char err[512];
  null_value= true;

  if (!args[0]->const_item())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_agg: function name must be a string constant",
                    MYF(0));
    return true;
  }
  StringBuffer<64> name_buf;
  String *name= args[0]->val_str(&name_buf);
  if (!name || args[0]->null_value)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_agg: function name cannot be NULL", MYF(0));
    return true;
  }
  if (py_catalog_ensure(err, sizeof(err)))
  {
    my_printf_error(ER_UNKNOWN_ERROR, "py_agg: %s", MYF(0), err);
    return true;
  }
  if (!(m_entry= py_catalog_lookup(name->c_ptr_safe())))
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_agg: unknown Python function '%s'", MYF(0),
                    name->c_ptr_safe());
    return true;
  }
  if (m_entry->has_params && n_data() != m_entry->params.size())
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_agg: function '%s' expects %zu arguments, got %u",
                    MYF(0), m_entry->name.c_str(), m_entry->params.size(),
                    n_data());
    return true;
  }

  if (lang_python_apply_return_type(
          this, lang_python_parse_return_type(m_entry->return_type.c_str()),
          m_entry->return_type.c_str(), "py_agg"))
    return true;

  /*
    An aggregate's result field is created straight from its type handler
    (CREATE TABLE ... SELECT, temporary tables).  A VARCHAR handler with
    the unbounded length used for Python strings is not a valid column,
    so pick the TEXT handler that fits the length.
  */
  set_handler(type_handler()->
              type_handler_adjusted_to_max_octet_length(max_length,
                                                        collation.collation));
  return false;
}


Item_sum_py_agg::State *Item_sum_py_agg::state()
{
  if (!m_state)
  {
    m_state.reset(new State);
    m_state->pin.pin();
    m_state->max_bytes= lang_python_agg_max_mem(current_thd);
    m_state->chunk_rows= lang_python_agg_chunk_rows(current_thd);
    m_state->cols.resize(n_data());
    for (uint i= 0; i < n_data(); i++)
    {
      Rowsink_column &c= m_state->cols[i];
      Item *arg= args[i + 1];
      c.kind= rowsink_kind_for(arg->type_handler(), arg->unsigned_flag);
      if (m_entry && m_entry->has_params)
        c.name= m_entry->params[i];
      else
        c.name= "args[" + std::to_string(i) + "]";
    }
  }
  return m_state.get();
}


void Item_sum_py_agg::clear()
{
  null_value= true;
  if (!m_state)
    return;
  State *st= m_state.get();
  st->drop_result();
  st->drop_instance();
  st->reset_buffers();
  st->seen_rows= false;
  st->failed= false;
}


void Item_sum_py_agg::cleanup()
{
  m_state.reset();
  null_value= true;
  Item_sum_plugin::cleanup();
}


/* -------------------------------------------------------------------------
   add_row: no Python, no GIL.
   ------------------------------------------------------------------------- */

bool Item_sum_py_agg::add_row()
{
  State *st= state();
  if (st->computed)                       /* running frame: result is stale */
    st->drop_result();
  st->seen_rows= true;
  if (st->failed)                         /* error earlier in the group */
    return false;

  /*
    A row that cannot be added.  A window frame scan ignores the result of
    add() and reads on to the end of the frame before it checks for an
    error, so the group stops buffering here: the remaining rows are
    skipped, and no partly added row is left behind.
  */
  auto fail_group= [st]()
  {
    st->failed= true;
    st->reset_buffers();
    return true;
  };

  for (uint i= 0; i < n_data(); i++)
  {
    Rowsink_column &c= st->cols[i];
    Item *arg= aggregation_arg(i + 1);

    switch (c.kind)
    {
    case ROWSINK_INT64:
    case ROWSINK_UINT64:
    {
      longlong v= arg->val_int();
      if (arg->null_value)
      {
        c.null_rows.push_back(c.ints.size());
        v= 0;
      }
      c.ints.push_back(v);
      st->bytes+= sizeof(longlong);
      break;
    }
    case ROWSINK_FLOAT64:
    {
      double v= arg->val_real();
      c.doubles.push_back(arg->null_value
                          ? std::numeric_limits<double>::quiet_NaN() : v);
      st->bytes+= sizeof(double);
      break;
    }
    case ROWSINK_VECTOR:
    {
      StringBuffer<256> buf;
      String *s= arg->val_str(&buf);
      if (!s || arg->null_value)
      {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "py_agg: NULL in VECTOR argument '%s'", MYF(0),
                        c.name.c_str());
        return fail_group();
      }
      size_t dim= s->length() / sizeof(float);
      if (s->length() % sizeof(float) || dim == 0 ||
          (!c.floats.empty() && dim != c.dim))
      {
        my_printf_error(ER_UNKNOWN_ERROR,
                        "py_agg: VECTOR argument '%s' has values of "
                        "different dimensions", MYF(0), c.name.c_str());
        return fail_group();
      }
      c.dim= dim;
      size_t old= c.floats.size();
      c.floats.resize(old + dim);
      memcpy(&c.floats[old], s->ptr(), s->length());
      st->bytes+= s->length();
      break;
    }
    case ROWSINK_OBJECT:
    {
      StringBuffer<256> buf, conv;
      String *s= arg->val_str(&buf);
      Rowsink_object_tag tag= ROWSINK_TAG_NULL;
      if (s && !arg->null_value)
      {
        if (s->charset() == &my_charset_bin)
          tag= ROWSINK_TAG_BYTES;
        else
        {
          tag= ROWSINK_TAG_STR;
          if (!my_charset_same(s->charset(), &my_charset_utf8mb4_bin))
          {
            uint errors;
            if (conv.copy(s->ptr(), s->length(), s->charset(),
                          &my_charset_utf8mb4_bin, &errors))
              return fail_group();
            s= &conv;
          }
        }
        c.bytes.append(s->ptr(), s->length());
        st->bytes+= s->length();
      }
      c.ends.push_back(c.bytes.size());
      c.tags.push_back((char) tag);
      st->bytes+= sizeof(size_t) + 1;
      break;
    }
    }
  }
  st->rows++;

  if (st->bytes > st->max_bytes)
  {
    my_printf_error(ER_UNKNOWN_ERROR,
                    "py_agg: group exceeds lang_python_agg_max_mem "
                    "(%llu bytes)", MYF(0), st->max_bytes);
    return fail_group();
  }
  if (m_entry->is_aggregate && st->rows >= st->chunk_rows)
    return step_chunk(st);
  return false;
}


/* -------------------------------------------------------------------------
   Result.  call_python() runs with the GIL held: copy the buffers into
   arrays, evaluate the cached code object in a fresh namespace (py_exec
   semantics, as py_call), read `result`.
   ------------------------------------------------------------------------- */

PyObject *Item_sum_py_agg::build_args(State *st)
{
  PyObject *tuple= PyTuple_New(n_data());
  for (uint i= 0; tuple && i < n_data(); i++)
  {
    PyObject *arr= py_column_to_array(st->cols[i], st->rows, false,
                                      "", "", PyExc_ValueError);
    if (!arr)
      Py_CLEAR(tuple);
    else
      PyTuple_SET_ITEM(tuple, i, arr);    /* steals the reference */
  }
  return tuple;
}


void Item_sum_py_agg::report_python_error()
{
  char buf[512];
  lang_python_format_exc("py_agg", buf, sizeof(buf));
  if (m_entry->strict_errors)
    my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), buf);
  else
    push_warning_printf(current_thd, Sql_condition::WARN_LEVEL_WARN,
                        ER_UNKNOWN_ERROR, "%s", buf);
}


/* Whole-group mode (catalog kind 'function') */

PyObject *Item_sum_py_agg::call_python(State *st)
{
  PyObject *globals= PyDict_New();
  if (!globals)
    return nullptr;
  PyDict_SetItem(globals, py_name.builtins, PyEval_GetBuiltins());

  PyObject *tuple= build_args(st);
  bool ok= tuple != nullptr;
  if (ok && m_entry->has_params)
  {
    for (uint i= 0; ok && i < n_data(); i++)
      ok= PyDict_SetItem(globals, m_entry->param_objs[i],
                         PyTuple_GET_ITEM(tuple, i)) == 0;
  }
  else if (ok)
    ok= PyDict_SetItem(globals, py_name.args, tuple) == 0;
  Py_XDECREF(tuple);

  PyObject *result= nullptr;
  if (ok)
  {
    PyObject *run_ret= PyEval_EvalCode(m_entry->code, globals, globals);
    if (run_ret)
    {
      Py_DECREF(run_ret);
      result= PyDict_GetItem(globals, py_name.result);   /* borrowed */
      if (result == Py_None)
        result= nullptr;
      Py_XINCREF(result);
    }
  }

  if (PyErr_Occurred())
  {
    report_python_error();
    Py_CLEAR(result);
  }
  Py_DECREF(globals);
  return result;
}


/* -------------------------------------------------------------------------
   Chunked mode (catalog kind 'aggregate').
   ------------------------------------------------------------------------- */

/*
  Hand the buffered rows to Aggregate.step() and empty the buffers.
  Returns true when the statement must abort (strict error); a lax error
  marks the group failed and returns false.
*/
bool Item_sum_py_agg::step_chunk(State *st)
{
  PyGILState_STATE gs= PyGILState_Ensure();
  bool ok= true;

  if (!st->cls)
  {
    /*
      First chunk of the statement: run the body, find the class.  After a
      lax error in the body the next group arrives here again; release the
      namespace of the failed run.
    */
    Py_XDECREF(st->ns);
    st->ns= PyDict_New();
    ok= st->ns != nullptr;
    if (ok)
    {
      PyDict_SetItem(st->ns, py_name.builtins, PyEval_GetBuiltins());
      PyObject *run_ret= PyEval_EvalCode(m_entry->code, st->ns, st->ns);
      ok= run_ret != nullptr;
      Py_XDECREF(run_ret);
    }
    if (ok)
    {
      st->cls= PyDict_GetItem(st->ns, py_name.aggregate);   /* borrowed */
      if (!st->cls || !PyCallable_Check(st->cls))
      {
        st->cls= nullptr;
        PyErr_Format(PyExc_TypeError,
                     "aggregate '%s' does not define a class named Aggregate",
                     m_entry->name.c_str());
        ok= false;
      }
      else
        Py_INCREF(st->cls);
    }
  }
  if (ok && !st->inst)
    ok= (st->inst= PyObject_CallNoArgs(st->cls)) != nullptr;

  if (ok)
  {
    PyObject *tuple= build_args(st);
    PyObject *step= tuple ? PyObject_GetAttr(st->inst, py_name.step) : nullptr;
    PyObject *ret= step ? PyObject_CallObject(step, tuple) : nullptr;
    ok= ret != nullptr;
    Py_XDECREF(ret);
    Py_XDECREF(step);
    Py_XDECREF(tuple);
  }

  bool abort_stmt= false;
  if (!ok)
  {
    report_python_error();
    st->failed= true;
    abort_stmt= m_entry->strict_errors;
  }
  PyGILState_Release(gs);

  st->reset_buffers();
  return abort_stmt;
}


/* The group's result: flush the pending partial chunk, call finalize() */
PyObject *Item_sum_py_agg::finalize_group(State *st)
{
  if (st->rows && step_chunk(st))
    return nullptr;
  if (st->failed || !st->inst)
    return nullptr;

  PyObject *result= PyObject_CallMethodNoArgs(st->inst, py_name.finalize);
  if (!result)
  {
    report_python_error();
    st->failed= true;
    return nullptr;
  }
  if (result == Py_None)
    Py_CLEAR(result);
  return result;
}


PyObject *Item_sum_py_agg::compute()
{
  if (aggr)
    aggr->endup();                        /* DISTINCT: replay unique rows */

  State *st= state();
  if (!st->computed)
  {
    st->computed= true;
    if (m_entry && m_entry->is_aggregate)
    {
      if (st->seen_rows && !st->failed)
      {
        PyGILState_STATE gs= PyGILState_Ensure();
        st->result= finalize_group(st);
        PyGILState_Release(gs);
      }
    }
    else if (st->rows && m_entry)
    {
      PyGILState_STATE gs= PyGILState_Ensure();
      st->result= call_python(st);
      PyGILState_Release(gs);
    }
  }
  return st->result;
}


double Item_sum_py_agg::val_real()
{
  PyObject *obj= compute();
  if ((null_value= !obj))
    return 0.0;
  PyGILState_STATE gs= PyGILState_Ensure();
  double ret= unmarshal_real(obj, "py_agg", null_value);
  PyGILState_Release(gs);
  return ret;
}

longlong Item_sum_py_agg::val_int()
{
  PyObject *obj= compute();
  if ((null_value= !obj))
    return 0;
  PyGILState_STATE gs= PyGILState_Ensure();
  longlong ret= unmarshal_int(obj, "py_agg", null_value);
  PyGILState_Release(gs);
  return ret;
}

String *Item_sum_py_agg::val_str(String *str)
{
  PyObject *obj= compute();
  if ((null_value= !obj))
    return nullptr;
  PyGILState_STATE gs= PyGILState_Ensure();
  String *ret= unmarshal_str(obj, str, "py_agg", null_value);
  PyGILState_Release(gs);
  return ret;
}

my_decimal *Item_sum_py_agg::val_decimal(my_decimal *to)
{
  PyObject *obj= compute();
  if ((null_value= !obj))
    return nullptr;
  PyGILState_STATE gs= PyGILState_Ensure();
  my_decimal *ret= unmarshal_decimal(obj, to, "py_agg", null_value);
  PyGILState_Release(gs);
  return ret;
}
