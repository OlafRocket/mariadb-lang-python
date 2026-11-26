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
  Named-function catalog: persistence and compile-once cache.
  See py_func_catalog.h for the contract.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>            /* sql_print_warning */

#include <mysql.h>
#include <mysql/service_sql.h>

#include <ctype.h>

#include <mutex>
#include <shared_mutex>
#include <unordered_map>

#include "lang_python.h"
#include "py_func_catalog.h"
#include "py_local_session.h"


/* -------------------------------------------------------------------------
   State.

   mutate_lock serializes catalog mutations (boot, create, drop) so
   check-then-write races cannot happen.  cache_lock guards only the map
   itself: py_call's lookup takes it shared for the duration of a hash
   find, never across SQL or Python.
   ------------------------------------------------------------------------- */

static std::mutex        mutate_lock;
static std::shared_mutex cache_lock;
static std::unordered_map<std::string, PyFuncEntryPtr> cache;
static bool loaded= false;


PyFuncEntry::~PyFuncEntry()
{
  /*
    The last shared_ptr reference can be dropped from a thread that does
    not hold the GIL (cache eviction, Item destruction at statement end).
    Skip entirely after Py_Finalize: the code object died with the
    interpreter.
  */
  if ((code || !param_objs.empty()) && Py_IsInitialized())
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    Py_XDECREF(code);
    for (PyObject *p : param_objs)
      Py_XDECREF(p);
    PyGILState_Release(gs);
  }
}


/* -------------------------------------------------------------------------
   SQL helpers: everything goes through a local connection of the plugin
   SQL service on a session of its own (py_local_session.h), so a
   catalog change never joins the session or the transaction of the
   statement that calls py_create_function.  One short-lived connection
   per catalog operation; catalog operations are rare (py_call itself
   never runs SQL).
   ------------------------------------------------------------------------- */

static const char CATALOG_DDL[]=
  "CREATE TABLE IF NOT EXISTS mysql.lang_python_func ("
  "  name          VARCHAR(64)   NOT NULL PRIMARY KEY,"
  "  body          TEXT          NOT NULL,"
  "  return_type   VARCHAR(16)   NOT NULL,"
  "  param_names   VARCHAR(1024) NULL,"
  "  comment       VARCHAR(255)  NULL,"
  "  definer       VARCHAR(384)  NULL,"
  "  strict_errors TINYINT(1)    NOT NULL DEFAULT 1,"
  "  kind          ENUM('function','aggregate') NOT NULL DEFAULT 'function',"
  "  is_deterministic TINYINT(1) NOT NULL DEFAULT 0,"
  "  created       TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP,"
  "  modified      TIMESTAMP     NOT NULL DEFAULT CURRENT_TIMESTAMP"
  "                  ON UPDATE CURRENT_TIMESTAMP"
  ") ENGINE=Aria TRANSACTIONAL=1 "
  "COMMENT='lang_python named functions (py_create_function)'";

/* A table created before the column existed gets it on the first load.
   The name follows mysql.proc; DETERMINISTIC is a reserved word. */
static const char CATALOG_UPGRADE[]=
  "ALTER TABLE mysql.lang_python_func"
  " ADD COLUMN IF NOT EXISTS is_deterministic TINYINT(1) NOT NULL DEFAULT 0"
  " AFTER kind";

static const char CATALOG_LOAD[]=
  "SELECT name, body, return_type, param_names, strict_errors, kind,"
  " is_deterministic FROM mysql.lang_python_func";

static MYSQL *local_connect(char *errbuf, size_t errlen)
{
  MYSQL *m= mysql_init(nullptr);
  if (!m)
  {
    snprintf(errbuf, errlen, "out of memory (mysql_init)");
    return nullptr;
  }
  if (py_connect_own_session(m))
  {
    snprintf(errbuf, errlen, "internal connection failed: %s", mysql_error(m));
    mysql_close(m);
    return nullptr;
  }
  return m;
}

/*
  Run a catalog INSERT/REPLACE/DELETE, self-healing if the table has
  been dropped since load (ER_NO_SUCH_TABLE): recreate it and retry
  once.  true = failure with message in errbuf.
*/
static bool run_catalog_write(MYSQL *m, const std::string &q,
                              char *errbuf, size_t errlen)
{
  if (!mysql_real_query(m, q.c_str(), (unsigned long) q.length()))
    return false;
  if (mysql_errno(m) == 1146 /* ER_NO_SUCH_TABLE */ &&
      !mysql_real_query(m, CATALOG_DDL, sizeof(CATALOG_DDL) - 1) &&
      !mysql_real_query(m, q.c_str(), (unsigned long) q.length()))
    return false;
  snprintf(errbuf, errlen, "catalog write failed: %s", mysql_error(m));
  return true;
}

static void append_quoted(std::string &q, MYSQL *m, const char *s)
{
  size_t len= strlen(s);
  std::string buf(len * 2 + 1, '\0');
  unsigned long elen=
    mysql_real_escape_string(m, &buf[0], s, (unsigned long) len);
  q+= '\'';
  q.append(buf.data(), elen);
  q+= '\'';
}


/* -------------------------------------------------------------------------
   Compilation.

   The body compiles UNMODIFIED as module-level code (deliberately no
   def-wrapping), so its semantics are identical to py_exec: assign the
   answer to `result` (absent/None → SQL NULL), `return` at top level is
   a create-time SyntaxError.  Named parameters are injected into the
   namespace by py_call before evaluation.  Compilation both validates
   the body at create time (design: errors surface then, not at first
   call) and produces the cached code object py_call evaluates.
   ------------------------------------------------------------------------- */

static PyObject *compile_named(const char *name, const char *body,
                               char *errbuf, size_t errlen)
{
  PyGILState_STATE gs= PyGILState_Ensure();

  PyObject *dedented= lang_python_dedent_source(body);
  const char *src= dedented ? PyUnicode_AsUTF8(dedented) : nullptr;
  if (!src)
  {
    PyErr_Clear();
    src= body;
  }

  PyObject *code= Py_CompileString(src, "<lang_python>", Py_file_input);
  Py_XDECREF(dedented);
  if (!code)
    lang_python_format_exc(name, errbuf, errlen);

  PyGILState_Release(gs);
  return code;
}

/*
  Split 'x, y' into validated identifiers.  nullptr (SQL NULL) or an
  empty string selects args[] mode.  true = failure.
*/
static bool split_params(const char *s, std::vector<std::string> &out,
                         char *errbuf, size_t errlen)
{
  if (!s)
    return false;
  const char *p= s;
  for (;;)
  {
    while (*p == ' ') p++;
    const char *start= p;
    while (isalnum((uchar) *p) || *p == '_') p++;
    if (p == start)
    {
      if (!*p && out.empty() && start == s)
        return false;                       /* empty string = args[] mode */
      snprintf(errbuf, errlen,
               "param_names must be a comma-separated list of identifiers");
      return true;
    }
    if (isdigit((uchar) *start))
    {
      snprintf(errbuf, errlen, "invalid parameter name '%.*s'",
               (int) (p - start), start);
      return true;
    }
    std::string pname(start, p - start);
    /* Names the call namespace already owns: `result` carries the return
       value out, `args` is the args[]-mode tuple, and `__builtins__` is
       the builtin table.  A parameter shadowing them would silently
       change the function's meaning. */
    if (pname == "result" || pname == "args" || pname == "__builtins__")
    {
      snprintf(errbuf, errlen, "parameter name '%s' is reserved",
               pname.c_str());
      return true;
    }
    out.push_back(std::move(pname));
    while (*p == ' ') p++;
    if (!*p)
      return false;
    if (*p != ',')
    {
      snprintf(errbuf, errlen,
               "param_names must be a comma-separated list of identifiers");
      return true;
    }
    p++;
  }
}

static PyFuncEntryPtr make_entry(const char *name, const char *return_type,
                                 const char *param_names,
                                 std::vector<std::string> &&params,
                                 bool strict, bool aggregate,
                                 bool deterministic, PyObject *code)
{
  auto e= std::make_shared<PyFuncEntry>();
  e->name= name;
  e->return_type= return_type;
  e->param_names= param_names ? param_names : "";
  e->params= std::move(params);
  e->has_params= !e->params.empty();
  e->strict_errors= strict;
  e->is_aggregate= aggregate;
  e->deterministic= deterministic;
  e->code= code;

  /* The names as interned strings, for the per-call namespace setup */
  if (e->has_params)
  {
    PyGILState_STATE gs= PyGILState_Ensure();
    for (const std::string &p : e->params)
    {
      PyObject *o= PyUnicode_InternFromString(p.c_str());
      if (!o)
        PyErr_Clear();
      e->param_objs.push_back(o);
    }
    PyGILState_Release(gs);
    for (PyObject *o : e->param_objs)
      if (!o)
        return nullptr;                   /* out of memory */
  }
  return e;
}


/* -------------------------------------------------------------------------
   Boot / ensure: mutate_lock must be held.
   ------------------------------------------------------------------------- */

static bool ensure_loaded_locked(char *errbuf, size_t errlen)
{
  if (loaded)
    return false;

  if (!Py_IsInitialized())
  {
    snprintf(errbuf, errlen, "Python interpreter is not initialised");
    return true;
  }

  MYSQL *m= local_connect(errbuf, errlen);
  if (!m)
    return true;

  if (mysql_real_query(m, CATALOG_DDL, sizeof(CATALOG_DDL) - 1))
  {
    snprintf(errbuf, errlen, "cannot create mysql.lang_python_func: %s",
             mysql_error(m));
    mysql_close(m);
    return true;
  }
  if (mysql_real_query(m, CATALOG_UPGRADE, sizeof(CATALOG_UPGRADE) - 1))
  {
    snprintf(errbuf, errlen, "cannot upgrade mysql.lang_python_func: %s",
             mysql_error(m));
    mysql_close(m);
    return true;
  }

  if (mysql_real_query(m, CATALOG_LOAD, sizeof(CATALOG_LOAD) - 1))
  {
    snprintf(errbuf, errlen, "cannot read mysql.lang_python_func: %s",
             mysql_error(m));
    mysql_close(m);
    return true;
  }

  MYSQL_RES *res= mysql_store_result(m);
  if (!res)
  {
    snprintf(errbuf, errlen, "cannot fetch mysql.lang_python_func rows: %s",
             mysql_error(m));
    mysql_close(m);
    return true;
  }

  std::unordered_map<std::string, PyFuncEntryPtr> fresh;
  while (MYSQL_ROW row= mysql_fetch_row(res))
  {
    const char *nm= row[0], *body= row[1], *rtype= row[2];
    const char *params= row[3];                      /* NULL = args[] mode */
    bool strict= !row[4] || atoi(row[4]) != 0;
    bool aggregate= row[5] && !strcmp(row[5], "aggregate");
    bool deterministic= row[6] && atoi(row[6]) != 0;

    /* A broken row must not prevent the server (or the rest of the
       catalog) from coming up. */
    char cerr[512];
    std::vector<std::string> plist;
    if (split_params(params, plist, cerr, sizeof(cerr)))
    {
      sql_print_warning("lang_python: skipping named function '%s': %s",
                        nm, cerr);
      continue;
    }
    PyObject *code= compile_named(nm, body, cerr, sizeof(cerr));
    if (!code)
    {
      sql_print_warning("lang_python: skipping named function '%s': %s",
                        nm, cerr);
      continue;
    }
    PyFuncEntryPtr entry= make_entry(nm, rtype, params, std::move(plist),
                                     strict, aggregate, deterministic, code);
    if (!entry)
    {
      sql_print_warning("lang_python: skipping named function '%s': "
                        "out of memory", nm);
      continue;
    }
    fresh.emplace(nm, std::move(entry));
  }
  mysql_free_result(res);
  mysql_close(m);

  {
    std::unique_lock<std::shared_mutex> l(cache_lock);
    cache.swap(fresh);
  }
  loaded= true;
  return false;
}


/* -------------------------------------------------------------------------
   Public interface
   ------------------------------------------------------------------------- */

bool py_catalog_ensure(char *errbuf, size_t errlen)
{
  std::lock_guard<std::mutex> g(mutate_lock);
  return ensure_loaded_locked(errbuf, errlen);
}

void py_catalog_shutdown()
{
  std::lock_guard<std::mutex> g(mutate_lock);
  std::unordered_map<std::string, PyFuncEntryPtr> victims;
  {
    std::unique_lock<std::shared_mutex> l(cache_lock);
    cache.swap(victims);
    loaded= false;
  }
  /* victims destructs here, outside cache_lock (entry dtors take the GIL) */
}

PyFuncEntryPtr py_catalog_lookup(const char *name)
{
  std::shared_lock<std::shared_mutex> l(cache_lock);
  auto it= cache.find(name);
  return it == cache.end() ? nullptr : it->second;
}

bool py_catalog_create(const char *name, const char *return_type,
                       const char *param_names, const char *body,
                       bool strict_errors, bool aggregate, bool deterministic,
                       bool replace, char *errbuf, size_t errlen)
{
  std::lock_guard<std::mutex> g(mutate_lock);
  if (ensure_loaded_locked(errbuf, errlen))
    return true;

  {
    std::shared_lock<std::shared_mutex> l(cache_lock);
    if (!replace && cache.count(name))
    {
      snprintf(errbuf, errlen,
               "function '%s' already exists (use %s)", name,
               aggregate ? "py_replace_aggregate" : "py_replace_function");
      return true;
    }
  }

  std::vector<std::string> plist;
  if (split_params(param_names, plist, errbuf, errlen))
    return true;

  PyObject *code= compile_named(name, body, errbuf, errlen);
  if (!code)
    return true;
  PyFuncEntryPtr entry= make_entry(name, return_type, param_names,
                                   std::move(plist), strict_errors,
                                   aggregate, deterministic, code);
  if (!entry)
  {
    snprintf(errbuf, errlen, "out of memory");
    return true;
  }

  MYSQL *m= local_connect(errbuf, errlen);
  if (!m)
    return true;

  std::string q(replace ? "REPLACE" : "INSERT");
  q+= " INTO mysql.lang_python_func"
      " (name, body, return_type, param_names, strict_errors, kind,"
      " is_deterministic) VALUES (";
  append_quoted(q, m, name);
  q+= ", ";
  append_quoted(q, m, body);
  q+= ", ";
  append_quoted(q, m, return_type);
  q+= ", ";
  if (param_names)
    append_quoted(q, m, param_names);
  else
    q+= "NULL";
  q+= ", ";
  q+= strict_errors ? "1, " : "0, ";
  q+= aggregate ? "'aggregate', " : "'function', ";
  q+= deterministic ? "1)" : "0)";

  if (run_catalog_write(m, q, errbuf, errlen))
  {
    mysql_close(m);
    return true;
  }
  mysql_close(m);

  {
    std::unique_lock<std::shared_mutex> l(cache_lock);
    cache[entry->name]= std::move(entry);
  }
  return false;
}

bool py_catalog_drop(const char *name, bool if_exists,
                     char *errbuf, size_t errlen)
{
  std::lock_guard<std::mutex> g(mutate_lock);
  if (ensure_loaded_locked(errbuf, errlen))
    return true;

  {
    std::shared_lock<std::shared_mutex> l(cache_lock);
    if (!cache.count(name))
    {
      if (if_exists)
        return false;
      snprintf(errbuf, errlen, "unknown function '%s'", name);
      return true;
    }
  }

  MYSQL *m= local_connect(errbuf, errlen);
  if (!m)
    return true;
  std::string q("DELETE FROM mysql.lang_python_func WHERE name= ");
  append_quoted(q, m, name);
  if (run_catalog_write(m, q, errbuf, errlen))
  {
    mysql_close(m);
    return true;
  }
  mysql_close(m);

  PyFuncEntryPtr victim;                /* dtor runs after the lock drops */
  {
    std::unique_lock<std::shared_mutex> l(cache_lock);
    auto it= cache.find(name);
    if (it != cache.end())
    {
      victim= std::move(it->second);
      cache.erase(it);
    }
  }
  return false;
}
