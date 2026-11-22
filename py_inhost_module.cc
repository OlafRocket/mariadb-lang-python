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
  Built-in `inhost` Python module (Phase 1.6).

  `inhost` is the namespace through which Python bodies reach the hosting
  server's services.  It deliberately does NOT mimic a client library:
  there is nothing to connect to; the code already runs inside mariadbd.
  Current members:

      inhost.version       server version string
      inhost.sql           in-process database access:

      import inhost
      rows = inhost.sql.query("SELECT name FROM test.t WHERE id > %s", (x,))
      sets = inhost.sql.query_sets("CALL test.report()")
      n    = inhost.sql.execute("UPDATE test.t SET hits = hits + 1")
      cols = inhost.sql.query_array("SELECT x, y FROM test.t")  # numpy

  Future services (e.g. inhost.uid) are added as further submodules.

  Execution context of inhost.sql: every statement (query, execute,
  query_array) runs on a private internal THD through the row sink
  (py_inhost_rowsink.cc), never on the THD executing the outer statement,
  which would be unsafe to re-enter.  The plugin SQL service
  (mysql_real_connect_local) is not used here: it converts every value to
  text and buffers the whole result.  Consequences, by design (see
  ROADMAP Phase 1.6):

    - separate transaction: cannot see the outer statement's uncommitted
      changes; writes commit independently (autocommit);
    - no session state between calls (each call is a fresh THD);
    - no current database: qualify table names (test.t1);
    - statement text and returned strings are UTF-8 (utf8mb4);
    - a KILL of the calling statement is forwarded at each result row;
    - runs grants-free, like the rest of the plugin's SQL (the plugin
      already implies arbitrary-code trust, see LIMITATIONS.md).

  Parameters use %s placeholders (%% for a literal %); values are
  escaped: None -> NULL, bool -> 1/0, int/float -> numeric literal,
  str -> quoted string, bytes -> x'...' hex literal.

  query() returns inhost.sql.Rows: a list of row tuples with the
  attribute `columns` (tuple of column names).  Cells: integer types ->
  int, FLOAT/DOUBLE -> float, NULL -> None, binary strings -> bytes,
  everything else (strings, DECIMAL, temporal types) -> str.  A statement
  that returns several result sets (CALL) is an error in query();
  query_sets() returns a list with one Rows per result set.
  query_array() returns one numpy array per column (docs/BULK_DATA.md
  section 2).

  The GIL is released while the statement runs, so a long query does not
  stall other connections' Python.
*/

#include <Python.h>

#include <my_global.h>
#include <my_sys.h>                       /* escape_string_for_mysql */
#include <m_ctype.h>
#include <mysql_version.h>

#include "py_numpy.h"

#include <stdlib.h>
#include <string.h>

#include <string>

#include "lang_python.h"
#include "py_inhost_module.h"
#include "py_inhost_rowsink.h"
#include "py_columns.h"


static PyObject *SqlError;                /* inhost.sql.Error */
static PyObject *RowsType;                /* inhost.sql.Rows */


/* -------------------------------------------------------------------------
   Parameter substitution, GIL held.
   ------------------------------------------------------------------------- */

static bool append_param(std::string &q, PyObject *v)
{
  if (v == Py_None)
  {
    q+= "NULL";
    return true;
  }
  if (PyBool_Check(v))                    /* before PyLong: bool ⊂ int */
  {
    q+= (v == Py_True) ? "1" : "0";
    return true;
  }
  if (PyLong_Check(v) || PyFloat_Check(v))
  {
    /* repr() of int/float is a round-trippable numeric literal */
    PyObject *s= PyObject_Repr(v);
    if (!s)
      return false;
    const char *c= PyUnicode_AsUTF8(s);
    bool ok= c != nullptr;
    if (ok)
      q+= c;
    Py_DECREF(s);
    return ok;
  }
  if (PyUnicode_Check(v))
  {
    Py_ssize_t len;
    const char *s= PyUnicode_AsUTF8AndSize(v, &len);
    if (!s)
      return false;
    std::string buf(len * 2 + 1, '\0');
    /* The row-sink THD reads the statement as utf8mb4 with backslash
       escapes enabled (py_inhost_rowsink.cc) */
    my_bool overflow;
    size_t elen= escape_string_for_mysql(&my_charset_utf8mb4_general_ci,
                                         &buf[0], buf.size(), s,
                                         (size_t) len, &overflow);
    q+= '\'';
    q.append(buf.data(), elen);
    q+= '\'';
    return true;
  }
  if (PyBytes_Check(v))
  {
    char *b;
    Py_ssize_t len;
    if (PyBytes_AsStringAndSize(v, &b, &len) < 0)
      return false;
    static const char hex[]= "0123456789ABCDEF";
    q+= "x'";
    for (Py_ssize_t i= 0; i < len; i++)
    {
      q+= hex[(unsigned char) b[i] >> 4];
      q+= hex[(unsigned char) b[i] & 0x0f];
    }
    q+= '\'';
    return true;
  }
  PyErr_Format(PyExc_TypeError,
               "unsupported parameter type: %.100s", Py_TYPE(v)->tp_name);
  return false;
}

static bool build_sql(const char *sql, PyObject *params,
                      std::string &out)
{
  PyObject *fast= nullptr;
  Py_ssize_t nparams= 0, used= 0;

  if (params && params != Py_None)
  {
    if (PyUnicode_Check(params) || PyBytes_Check(params) ||
        !PySequence_Check(params))
    {
      PyErr_SetString(PyExc_TypeError, "params must be a tuple or list");
      return false;
    }
    if (!(fast= PySequence_Fast(params, "params must be a tuple or list")))
      return false;
    nparams= PySequence_Fast_GET_SIZE(fast);
  }

  bool ok= true;
  for (const char *p= sql; ok && *p; p++)
  {
    if (p[0] == '%' && p[1] == 's')
    {
      if (used >= nparams)
      {
        PyErr_SetString(PyExc_ValueError,
                        "not enough parameters for SQL placeholders");
        ok= false;
        break;
      }
      ok= append_param(out, PySequence_Fast_GET_ITEM(fast, used++));
      p++;
    }
    else if (p[0] == '%' && p[1] == '%')
    {
      out+= '%';
      p++;
    }
    else
      out+= *p;
  }
  if (ok && used != nparams)
  {
    PyErr_SetString(PyExc_ValueError,
                    "too many parameters for SQL placeholders");
    ok= false;
  }
  Py_XDECREF(fast);
  return ok;
}


/* -------------------------------------------------------------------------
   inhost.sql.query() / query_sets() / execute()

   The statement runs through the row sink in lossless mode with the GIL
   released; the rows are then turned into tuples.  Cell mapping:
   integer types -> int, FLOAT/DOUBLE -> float, NULL -> None, binary
   strings (BLOB, VARBINARY, BIT, VECTOR) -> bytes, everything else
   (strings, DECIMAL, temporal types) -> str.

   A result set is returned as inhost.sql.Rows: a list of row tuples with
   the attribute `columns`, the tuple of column names.  query() returns
   one Rows and fails when the statement produces several result sets;
   query_sets() returns a list of Rows, one per result set.
   ------------------------------------------------------------------------- */

static PyObject *rows_to_list(const std::vector<Rowsink_column> &cols,
                              size_t nrows);

/* Rows(list of tuples) with .columns set; new reference */
static PyObject *make_rows(const std::vector<Rowsink_column> &cols,
                           size_t nrows)
{
  PyObject *list= rows_to_list(cols, nrows);
  if (!list)
    return nullptr;
  PyObject *rows= PyObject_CallOneArg(RowsType, list);
  Py_DECREF(list);
  if (!rows)
    return nullptr;

  PyObject *names= PyTuple_New((Py_ssize_t) cols.size());
  for (size_t i= 0; names && i < cols.size(); i++)
  {
    PyObject *n= PyUnicode_DecodeUTF8(cols[i].name.data(),
                                      cols[i].name.size(), "replace");
    if (!n)
      Py_CLEAR(names);
    else
      PyTuple_SET_ITEM(names, i, n);
  }
  if (!names || PyObject_SetAttrString(rows, "columns", names) < 0)
    Py_CLEAR(rows);
  Py_XDECREF(names);
  return rows;
}

static PyObject *rows_to_list(const std::vector<Rowsink_column> &cols,
                              size_t nrows)
{
  size_t ncols= cols.size();
  std::vector<size_t> null_pos(ncols, 0);   /* next entry of null_rows */
  std::vector<size_t> obj_start(ncols, 0);  /* start offset in bytes */

  PyObject *list= PyList_New((Py_ssize_t) nrows);
  if (!list)
    return nullptr;

  for (size_t row= 0; row < nrows; row++)
  {
    PyObject *tup= PyTuple_New((Py_ssize_t) ncols);
    if (!tup)
    {
      Py_DECREF(list);
      return nullptr;
    }
    PyList_SET_ITEM(list, row, tup);        /* list owns it from here */

    for (size_t i= 0; i < ncols; i++)
    {
      const Rowsink_column &c= cols[i];
      PyObject *v= nullptr;
      switch (c.kind)
      {
      case ROWSINK_INT64:
      case ROWSINK_UINT64:
        if (null_pos[i] < c.null_rows.size() && c.null_rows[null_pos[i]] == row)
        {
          null_pos[i]++;
          v= Py_NewRef(Py_None);
        }
        else if (c.kind == ROWSINK_UINT64)
          v= PyLong_FromUnsignedLongLong((unsigned long long) c.ints[row]);
        else
          v= PyLong_FromLongLong(c.ints[row]);
        break;
      case ROWSINK_FLOAT64:
        /* SQL has no NaN: the sink stores NULL as NaN */
        v= c.doubles[row] != c.doubles[row] ? Py_NewRef(Py_None)
                                            : PyFloat_FromDouble(c.doubles[row]);
        break;
      case ROWSINK_VECTOR:                  /* not produced in exact mode */
      case ROWSINK_OBJECT:
      {
        const char *p= c.bytes.data() + obj_start[i];
        size_t len= c.ends[row] - obj_start[i];
        obj_start[i]= c.ends[row];
        switch (c.tags[row])
        {
        case ROWSINK_TAG_NULL:  v= Py_NewRef(Py_None); break;
        case ROWSINK_TAG_BYTES: v= PyBytes_FromStringAndSize(p, len); break;
        default:                v= PyUnicode_DecodeUTF8(p, len, "replace");
        }
        break;
      }
      }
      if (!v)
      {
        Py_DECREF(list);
        return nullptr;
      }
      PyTuple_SET_ITEM(tup, i, v);
    }
  }
  return list;
}

enum Run_mode { RUN_QUERY, RUN_QUERY_SETS, RUN_EXECUTE };

static PyObject *db_run(PyObject *args, Run_mode mode)
{
  const char *sql;
  PyObject *params= nullptr;
  if (!PyArg_ParseTuple(args, "s|O", &sql, &params))
    return nullptr;

  if (!lang_python_db_access())
  {
    PyErr_SetString(SqlError,
                    "database access is disabled "
                    "(@@lang_python_db_access = OFF)");
    return nullptr;
  }

  std::string q;
  if (!build_sql(sql, params, q))
    return nullptr;

  Rowsink_options opts;
  opts.exact= true;
  switch (mode)
  {
  case RUN_QUERY:
    opts.multi= Rowsink_options::ROWSINK_MULTI_ERROR;
    opts.multi_error= "query: statement returns more than one result set "
                      "(use query_sets)";
    break;
  case RUN_QUERY_SETS:
    opts.multi= Rowsink_options::ROWSINK_MULTI_ALL;
    break;
  case RUN_EXECUTE:
    opts.multi= Rowsink_options::ROWSINK_MULTI_FIRST;
    break;
  }
  Rowsink_result r;
  bool failed;
  Py_BEGIN_ALLOW_THREADS
  failed= rowsink_run(q.data(), q.length(), opts, &r);
  Py_END_ALLOW_THREADS
  if (failed)
  {
    PyErr_Format(SqlError, "%s", r.err_msg.c_str());
    return nullptr;
  }

  if (mode == RUN_EXECUTE)
    return PyLong_FromUnsignedLongLong(r.has_result_set
                                       ? (unsigned long long) r.rows
                                       : r.affected_rows);
  if (mode == RUN_QUERY)                    /* no result set: empty Rows */
    return make_rows(r.cols, r.rows);

  PyObject *sets= PyList_New(0);
  if (!sets || !r.has_result_set)
    return sets;
  bool ok= true;
  for (size_t i= 0; ok && i <= r.extra_sets.size(); i++)
  {
    PyObject *rows= i == 0 ? make_rows(r.cols, r.rows)
                           : make_rows(r.extra_sets[i - 1].cols,
                                       r.extra_sets[i - 1].rows);
    ok= rows && PyList_Append(sets, rows) == 0;
    Py_XDECREF(rows);
  }
  if (!ok)
    Py_CLEAR(sets);
  return sets;
}

static PyObject *db_query(PyObject *, PyObject *args)
{
  return db_run(args, RUN_QUERY);
}

static PyObject *db_query_sets(PyObject *, PyObject *args)
{
  return db_run(args, RUN_QUERY_SETS);
}

static PyObject *db_execute(PyObject *, PyObject *args)
{
  return db_run(args, RUN_EXECUTE);
}


/* -------------------------------------------------------------------------
   inhost.sql.query_array()

   The statement runs through the columnar row sink (py_inhost_rowsink.cc)
   with the GIL released; the sink fills per-column C buffers from the
   executor's typed values.  Here, with the GIL held again, each buffer is
   copied once into an exact-size numpy array.
   ------------------------------------------------------------------------- */

static PyObject *db_query_array(PyObject *, PyObject *args, PyObject *kwargs)
{
  static const char *kwlist[]= { "sql", "params", "nulls", nullptr };
  const char *sql;
  PyObject *params= nullptr;
  const char *nulls= "error";
  if (!PyArg_ParseTupleAndKeywords(args, kwargs, "s|Os",
                                   const_cast<char **>(kwlist),
                                   &sql, &params, &nulls))
    return nullptr;

  bool nulls_nan= !strcmp(nulls, "nan");
  if (!nulls_nan && strcmp(nulls, "error"))
  {
    PyErr_SetString(PyExc_ValueError, "nulls must be 'error' or 'nan'");
    return nullptr;
  }
  if (!lang_python_db_access())
  {
    PyErr_SetString(SqlError,
                    "database access is disabled "
                    "(@@lang_python_db_access = OFF)");
    return nullptr;
  }

  std::string q;
  if (!build_sql(sql, params, q))
    return nullptr;

  Rowsink_result r;
  bool failed;
  Py_BEGIN_ALLOW_THREADS
  failed= rowsink_run(q.data(), q.length(), Rowsink_options(), &r);
  Py_END_ALLOW_THREADS
  if (failed)
  {
    PyErr_Format(SqlError, "%s", r.err_msg.c_str());
    return nullptr;
  }

  PyObject *ret= PyDict_New();
  if (!ret)
    return nullptr;
  for (Rowsink_column &c : r.cols)
  {
    PyObject *key= PyUnicode_DecodeUTF8(c.name.data(), c.name.size(),
                                        "replace");
    if (!key)
      goto err;
    int dup= PyDict_Contains(ret, key);
    if (dup)
    {
      if (dup > 0)
        PyErr_Format(SqlError,
                     "query_array: duplicate column name '%s' "
                     "(use column aliases)", c.name.c_str());
      Py_DECREF(key);
      goto err;
    }
    PyObject *arr= py_column_to_array(c, r.rows, nulls_nan, "query_array: ",
                                      " (pass nulls='nan' to get float64 "
                                      "with NaN)", SqlError);
    int rc= arr ? PyDict_SetItem(ret, key, arr) : -1;
    Py_DECREF(key);
    Py_XDECREF(arr);
    if (rc < 0)
      goto err;
    /* Release the C buffer now: peak memory stays near one column extra */
    c= Rowsink_column();
  }
  return ret;

err:
  Py_DECREF(ret);
  return nullptr;
}


/* -------------------------------------------------------------------------
   Module definitions: inhost (namespace) and inhost.sql (DB access)
   ------------------------------------------------------------------------- */

static PyMethodDef sql_methods[]=
{
  { "query", db_query, METH_VARARGS,
    "query(sql, params=None) -> Rows\n\n"
    "Run SQL on the hosting server in an internal session and return\n"
    "the rows: a list of tuples with the attribute `columns` (tuple of\n"
    "column names).  Statements without a result set return an empty\n"
    "list.  A statement that returns several result sets is an error;\n"
    "use query_sets().  Use %s placeholders with a params tuple/list;\n"
    "%% is a literal %.  Qualify table names (no current database)." },
  { "query_sets", db_query_sets, METH_VARARGS,
    "query_sets(sql, params=None) -> list[Rows]\n\n"
    "Like query(), for statements that return several result sets\n"
    "(CALL of a procedure with more than one SELECT): one Rows per\n"
    "result set, in order.  [] for a statement without a result set." },
  { "execute", db_execute, METH_VARARGS,
    "execute(sql, params=None) -> int\n\n"
    "Run SQL on the hosting server over an internal connection and\n"
    "return the affected-row count.  Autocommit; a separate transaction\n"
    "from the statement that invoked the Python body." },
  { "query_array", (PyCFunction) (void (*)(void)) db_query_array,
    METH_VARARGS | METH_KEYWORDS,
    "query_array(sql, params=None, nulls='error') -> dict[str, ndarray]\n\n"
    "Run SQL like query() and return one numpy array per column, keyed\n"
    "by column name in select order.  Integers -> int64/uint64,\n"
    "FLOAT/DOUBLE/DECIMAL -> float64 (NULL -> NaN), VECTOR -> 2-D\n"
    "float32, anything else -> object array of str/bytes/None.\n"
    "A NULL in an integer column is an error unless nulls='nan', which\n"
    "returns that column as float64 with NaN.  Values arrive in binary\n"
    "form with the GIL released; no per-cell Python objects are built\n"
    "for numeric columns." },
  { nullptr, nullptr, 0, nullptr }
};

static struct PyModuleDef sql_moduledef=
{
  PyModuleDef_HEAD_INIT,
  "inhost.sql",
  "In-process database access (lang_python).\n\n"
  "Queries run on a separate internal connection: a separate\n"
  "(autocommit) transaction, no current database, no session state\n"
  "between calls.",
  -1,
  sql_methods,
  nullptr, nullptr, nullptr, nullptr
};

static struct PyModuleDef inhost_moduledef=
{
  PyModuleDef_HEAD_INIT,
  "inhost",
  "Services of the hosting MariaDB server (lang_python).\n\n"
  "Not a client library: the code already runs inside the server.\n"
  "Members: version (server version string), sql (database access).",
  -1,
  nullptr,
  nullptr, nullptr, nullptr, nullptr
};

extern "C" PyObject *PyInit_lang_python_inhost(void)
{
  PyObject *sql= PyModule_Create(&sql_moduledef);
  if (!sql)
    return nullptr;

  SqlError= PyErr_NewException("inhost.sql.Error", nullptr, nullptr);
  if (!SqlError || PyModule_AddObject(sql, "Error", SqlError) < 0)
  {
    Py_XDECREF(SqlError);
    Py_DECREF(sql);
    return nullptr;
  }
  Py_INCREF(SqlError);                    /* module owns one, we keep one */

  /* class Rows(list): a result set; instances get a `columns` attribute */
  RowsType= PyObject_CallFunction((PyObject *) &PyType_Type, "s(O){ss}",
                                  "Rows", (PyObject *) &PyList_Type,
                                  "__module__", "inhost.sql");
  if (!RowsType || PyModule_AddObject(sql, "Rows", RowsType) < 0)
  {
    Py_XDECREF(RowsType);
    Py_DECREF(sql);
    return nullptr;
  }
  Py_INCREF(RowsType);

  PyObject *inhost= PyModule_Create(&inhost_moduledef);
  if (!inhost)
  {
    Py_DECREF(sql);
    return nullptr;
  }

  if (PyModule_AddStringConstant(inhost, "version", MYSQL_SERVER_VERSION) < 0 ||
      PyModule_AddObject(inhost, "sql", sql) < 0)
  {
    /* AddObject only steals the reference on success */
    Py_DECREF(sql);
    Py_DECREF(inhost);
    return nullptr;
  }

  /*
    Pre-register the submodule so `import inhost.sql` and
    `from inhost.sql import query` resolve (the parent is not a package,
    so the import system relies on finding "inhost.sql" in sys.modules).
  */
  if (PyDict_SetItemString(PyImport_GetModuleDict(), "inhost.sql", sql) < 0)
  {
    Py_DECREF(inhost);
    return nullptr;
  }

  return inhost;
}
