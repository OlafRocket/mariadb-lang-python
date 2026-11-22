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

#ifndef LANG_PYTHON_PY_FUNC_CATALOG_H
#define LANG_PYTHON_PY_FUNC_CATALOG_H

/*
  Named-function catalog (Phase 1.5).

  Persistent store: mysql.lang_python_func, auto-created on first use and
  accessed exclusively through the plugin SQL service (an internal local
  connection: no THD juggling in plugin code).

  In-memory mirror: a name → shared_ptr<const PyFuncEntry> map.  Bodies
  compile once, unmodified (module-level code, same semantics as py_exec:
  the body assigns `result`), and the code object is reused by every
  py_call.  Lookups copy the shared_ptr under a read lock and evaluate on
  the private reference, so a concurrent drop or replace never frees code
  out from under a running call, and no lock is ever held across user
  Python.

  All functions returning bool use true = failure, with a message in
  errbuf; raising the SQL error is the caller's job.
*/

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

typedef struct _object PyObject;          /* Python.h (CPython) */

struct PyFuncEntry
{
  std::string name;
  std::string return_type;     /* canonical: INT|DOUBLE|VARCHAR|DECIMAL|BOOL|BLOB */
  std::string param_names;     /* raw comma list; empty in args[] mode */
  std::vector<std::string> params;  /* parsed names; empty in args[] mode */
  bool        has_params= false;
  bool        strict_errors= true;
  bool        is_aggregate= false;  /* kind 'aggregate': body defines class
                                       Aggregate (step/finalize), used by
                                       py_agg in chunks; see
                                       docs/AGGREGATE_PROTOCOL.md */
  PyObject   *code= nullptr;   /* compiled body (module code); strong ref */

  PyFuncEntry() = default;
  ~PyFuncEntry();              /* GIL-safe Py_DECREF of code */
  PyFuncEntry(const PyFuncEntry &) = delete;
  PyFuncEntry &operator=(const PyFuncEntry &) = delete;
};

typedef std::shared_ptr<const PyFuncEntry> PyFuncEntryPtr;

/*
  Create the catalog table if needed and load+compile every row into the
  cache.  Idempotent and cheap once loaded.  Called at plugin init
  (best-effort) and again lazily from every catalog consumer, so a failure
  during server startup only defers availability, it doesn't wedge it.
*/
bool py_catalog_ensure(char *errbuf, size_t errlen);

/* Drop the cache (entries decref their code).  Call before Py_Finalize. */
void py_catalog_shutdown();

/* nullptr if absent.  Caller keeps the returned shared_ptr private. */
PyFuncEntryPtr py_catalog_lookup(const char *name);

/*
  Validate+compile, INSERT (or REPLACE) into the catalog, publish in the
  cache.  param_names == nullptr selects args[] mode.  With replace ==
  false an existing name is an error, whatever its kind; with replace ==
  true the kind may change.  aggregate selects the kind.
*/
bool py_catalog_create(const char *name, const char *return_type,
                       const char *param_names, const char *body,
                       bool strict_errors, bool aggregate, bool replace,
                       char *errbuf, size_t errlen);

/* DELETE from the catalog and evict from the cache. */
bool py_catalog_drop(const char *name, bool if_exists,
                     char *errbuf, size_t errlen);

#endif /* LANG_PYTHON_PY_FUNC_CATALOG_H */
