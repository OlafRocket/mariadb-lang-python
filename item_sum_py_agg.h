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

#ifndef LANG_PYTHON_ITEM_SUM_PY_AGG_H
#define LANG_PYTHON_ITEM_SUM_PY_AGG_H

/*
  py_agg(name, col0 [, col1, ...]): batching aggregate (Phase 2).

  The catalog kind of `name` selects one of two modes.

  Kind 'function' (py_create_function), whole-group mode: calls the named
  Python function (catalog entry, see py_func_catalog.h)
  ONCE PER GROUP, passing every argument as a numpy array that covers all
  rows of the group.  This is the SQL-native shape for batched inference
  and training: the server scans, filters and groups; Python sees columnar
  batches.

    SELECT py_create_function('mean_abs', 'DOUBLE', 'x',
                              'result = float(abs(x).mean())');
    SELECT grp, py_agg('mean_abs', val) FROM t GROUP BY grp;

  Registration: a native aggregate function plugin (MDEV-40672, server
  13.3+).  The class derives from Item_sum_plugin; the server supplies
  GROUP BY, DISTINCT replay, FILTER (WHERE ...) and OVER (...).

  Per row (add_row) no Python runs and the GIL is not taken: each
  argument's binary value is appended to a per-column C buffer.  The
  buffers use the same layout and the same type mapping as
  inhost.sql.query_array() (py_inhost_rowsink.h, py_columns.h):

    integer types           int64 / uint64 array
    FLOAT, DOUBLE, DECIMAL  float64 array, NULL -> NaN
    VECTOR                  2-D float32 array (rows x dim)
    anything else           object array of str / bytes / None

  A NULL in an integer or VECTOR column is an error.

  The result is computed lazily by the first val_*() call of a group and
  cached until the next row or clear().  The function's declared return
  type is the aggregate's type; `result` is converted exactly like a
  py_call result.  A group without rows yields SQL NULL without calling
  Python.

  The function name must be a string constant: the catalog entry decides
  the result type at prepare time.  In named-parameter mode the arrays are
  bound to the declared parameter names and the argument count must match;
  otherwise the function sees the tuple `args` of arrays.

  @@lang_python_agg_max_mem bounds the bytes one group may buffer.

  Window use: supports_removal() is false, so the server recomputes every
  frame from clear() + add_row(); correct, but one Python call per output
  row over the whole frame.

  Kind 'aggregate' (py_create_aggregate), chunked mode
  (docs/AGGREGATE_PROTOCOL.md): the body defines a class

    class Aggregate:
        def __init__(self): ...
        def step(self, x, y): ...      # arrays for one chunk of rows
        def finalize(self): ...        # the group's result

  Rows are buffered exactly as above, but every
  @@lang_python_agg_chunk_rows rows the buffers are handed to step() and
  emptied, so a group of any size is processed in bounded memory.

    - The body is executed once per statement item, in a fresh namespace,
      when the first chunk is due; `Aggregate` is looked up there.
    - One instance per group, created on the group's first chunk.
    - The first val_*() of a group flushes the pending partial chunk and
      calls finalize().  finalize() may be called again after further
      rows (running window frames) and must not reset the state.
    - With parameter names the argument count must match; step() always
      receives one positional array per argument.
    - A Python exception follows the entry's strict_errors: abort, or a
      warning with the group's result NULL and its remaining rows skipped.
    - A chunk boundary carries no meaning: results must not depend on
      the chunk size.
*/

#include "item_func_python.h"
#include "py_func_catalog.h"
#include "py_inhost_rowsink.h"

#include <item_sum.h>

#include <memory>


class Item_sum_py_agg : public Item_sum_plugin,
                        public Type_handler_hybrid_field_type
{
  using Self= Item_sum_py_agg;

  /*
    Group state.  Heap-allocated (not on the statement mem_root): the
    buffers grow with the data, and the cached result is a Python
    reference that must be released under the GIL.
  */
  struct State
  {
    /*
      Thread state of this statement; see py_thread_pin.h.  First member:
      destroyed last, after ~State() released the Python references.
    */
    Py_thread_pin pin;
    std::vector<Rowsink_column> cols;
    size_t rows= 0;
    unsigned long long bytes= 0;
    unsigned long long max_bytes= 0;
    bool computed= false;
    PyObject *result= nullptr;            /* strong ref; nullptr = SQL NULL */

    /* Chunked mode (catalog kind 'aggregate') */
    unsigned long long chunk_rows= 0;
    bool seen_rows= false;                /* the group has at least one row */
    /*
      An error in this group: its remaining rows are skipped.  After a lax
      Python error the group's result is NULL; after any other error the
      statement fails.
    */
    bool failed= false;
    PyObject *ns= nullptr;                /* namespace the body ran in */
    PyObject *cls= nullptr;               /* Aggregate; per statement item */
    PyObject *inst= nullptr;              /* Aggregate(); per group */

    ~State();
    void drop_result();
    void drop_instance();
    void reset_buffers();
  };

  /* Resolved at prepare time; a private strong reference, as in py_call */
  PyFuncEntryPtr m_entry;
  std::unique_ptr<State> m_state;

  uint n_data() const { return arg_count - 1; }
  State *state();
  PyObject *compute();                    /* borrowed ref or nullptr */
  PyObject *call_python(State *st);       /* new ref or nullptr; GIL held */
  PyObject *build_args(State *st);        /* tuple of arrays; GIL held */
  void report_python_error();             /* GIL held, exception pending */
  bool step_chunk(State *st);             /* true = statement must abort */
  PyObject *finalize_group(State *st);    /* new ref or nullptr; GIL held */

public:
  Item_sum_py_agg(THD *thd, List<Item> &list);
  Item_sum_py_agg(THD *thd, Self *item)
    : Item_sum_plugin(thd, item), Type_handler_hybrid_field_type(item),
      m_entry(item->m_entry)
  {}
  Item_sum_py_agg(const Self &item)
    : Item_sum_plugin(item), Type_handler_hybrid_field_type(item),
      m_entry(item.m_entry)
  {}

  void clear() override;
  bool add_row() override;

  double      val_real() override;
  longlong    val_int() override;
  String     *val_str(String *str) override;
  my_decimal *val_decimal(my_decimal *to) override;
  bool get_date(THD *thd, MYSQL_TIME *ltime, date_mode_t fuzzydate) override
  { return type_handler()->Item_get_date_with_warn(thd, this, ltime,
                                                   fuzzydate); }
  bool is_null() override
  {
    null_value= compute() == nullptr;
    return null_value;
  }

  const Type_handler *type_handler() const override
  { return Type_handler_hybrid_field_type::type_handler(); }

  bool fix_length_and_dec(THD *thd) override;
  void cleanup() override;

  LEX_CSTRING func_name_cstring() const override
  { return "py_agg("_LEX_CSTRING; }

  Item *copy_or_same(THD *thd) override
  { return new (thd->mem_root) Self(thd, this); }

  class Create_func : public Create_native_func
  {
  public:
    bool is_aggregate() const override { return true; }
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
  Item *shallow_copy(THD *thd) const override
  { return get_item_copy<Self>(thd, this); }
};

#endif /* LANG_PYTHON_ITEM_SUM_PY_AGG_H */
