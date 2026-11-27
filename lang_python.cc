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
  Python interpreter lifecycle, system variables, and runtime state.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>

#define LANG_PYTHON_IMPORT_ARRAY
#include "py_numpy.h"

#include <dlfcn.h>
#include <libgen.h>
#include <sys/stat.h>

#include "lang_python.h"
#include "py_inhost_module.h"
#include "py_func_catalog.h"
#include "lang_routine.h"

/* -------------------------------------------------------------------------
   State
   ------------------------------------------------------------------------- */

static PyThreadState *main_tstate= nullptr;
static bool numpy_available= false;

Lang_python_names py_name;

/* GIL held.  true = failure. */
static bool create_names()
{
  py_name.builtins=  PyUnicode_InternFromString("__builtins__");
  py_name.args=      PyUnicode_InternFromString("args");
  py_name.result=    PyUnicode_InternFromString("result");
  py_name.aggregate= PyUnicode_InternFromString("Aggregate");
  py_name.step=      PyUnicode_InternFromString("step");
  py_name.finalize=  PyUnicode_InternFromString("finalize");
  return !py_name.builtins || !py_name.args || !py_name.result ||
         !py_name.aggregate || !py_name.step || !py_name.finalize;
}

static void release_names()
{
  Py_CLEAR(py_name.builtins);
  Py_CLEAR(py_name.args);
  Py_CLEAR(py_name.result);
  Py_CLEAR(py_name.aggregate);
  Py_CLEAR(py_name.step);
  Py_CLEAR(py_name.finalize);
}


/* -------------------------------------------------------------------------
   System variables
   ------------------------------------------------------------------------- */

/*
  lang_python_vector_numpy: controls VECTOR column input marshaling.
    ON  (default): VECTOR → numpy.ndarray(float32) for ML/Pandas workflows
    OFF: VECTOR → bytes (raw packed floats, C connector compatible)
  Can be changed at runtime: SET GLOBAL lang_python_vector_numpy = 0;
*/
static my_bool vector_numpy_var= TRUE;

bool lang_python_use_numpy()
{
  return vector_numpy_var && numpy_available;
}

static MYSQL_SYSVAR_BOOL(vector_numpy, vector_numpy_var,
  PLUGIN_VAR_RQCMDARG,
  "Auto-convert VECTOR columns to numpy.ndarray(float32); "
  "OFF returns raw bytes (C connector compatible)",
  NULL, NULL, TRUE);

/*
  lang_python_strict_errors: controls how Python runtime failures surface.
    ON  (default): Python exceptions and result-conversion failures abort
                    the query with my_error.
    OFF: failures emit a MariaDB warning and return SQL NULL
                    (legacy behaviour; useful for exploratory queries).
  Session+global, settable at runtime: SET SESSION lang_python_strict_errors=0;
*/
static MYSQL_THDVAR_BOOL(strict_errors,
  PLUGIN_VAR_RQCMDARG,
  "Abort the query on Python exceptions and result-type conversion "
  "failures (ON, default); OFF returns SQL NULL with a warning instead",
  NULL, NULL, TRUE);

bool lang_python_strict_errors(THD *thd)
{
  return THDVAR(thd, strict_errors);
}

/*
  lang_python_db_access: kill switch for the built-in `inhost.sql` Python
  module (Phase 1.6).  ON (default): bodies may run SQL on an internal
  connection; OFF: inhost.sql.query()/execute() raise inhost.sql.Error.
  Global, settable at runtime: SET GLOBAL lang_python_db_access = 0;
*/
static my_bool db_access_var= TRUE;

bool lang_python_db_access()
{
  return db_access_var;
}

static MYSQL_SYSVAR_BOOL(db_access, db_access_var,
  PLUGIN_VAR_RQCMDARG,
  "Allow Python bodies to run SQL on the local server via the built-in "
  "inhost.sql module (query/execute on an internal connection)",
  NULL, NULL, TRUE);

/*
  lang_python_agg_max_mem: upper bound, in bytes, for the argument buffers
  one py_agg group may accumulate before its Python function is called.
  Exceeding it aborts the statement, so an unbounded GROUP BY cannot
  exhaust server memory.  Session+global.
*/
static MYSQL_THDVAR_ULONGLONG(agg_max_mem,
  PLUGIN_VAR_RQCMDARG,
  "Maximum bytes of argument data buffered per py_agg group; "
  "exceeding it aborts the statement",
  NULL, NULL, 256ULL * 1024 * 1024, 1024, ULONGLONG_MAX, 1);

unsigned long long lang_python_agg_max_mem(THD *thd)
{
  return THDVAR(thd, agg_max_mem);
}

/*
  lang_python_agg_chunk_rows: rows py_agg collects before it calls step()
  of a Python aggregate (catalog kind 'aggregate').  Functions of kind
  'function' always receive the whole group.  Session+global.
*/
static MYSQL_THDVAR_ULONGLONG(agg_chunk_rows,
  PLUGIN_VAR_RQCMDARG,
  "Rows per step() call when py_agg runs a Python aggregate",
  NULL, NULL, 65536, 1, ULONGLONG_MAX, 1);

unsigned long long lang_python_agg_chunk_rows(THD *thd)
{
  return THDVAR(thd, agg_chunk_rows);
}

static struct st_mysql_sys_var *sys_vars[]= {
  MYSQL_SYSVAR(vector_numpy),
  MYSQL_SYSVAR(strict_errors),
  MYSQL_SYSVAR(db_access),
  MYSQL_SYSVAR(agg_max_mem),
  MYSQL_SYSVAR(agg_chunk_rows),
  NULL
};

struct st_mysql_sys_var **lang_python_sys_vars()
{
  return sys_vars;
}


/* -------------------------------------------------------------------------
   Python home discovery
   ---------------------------------------------------------------------- */

static bool find_python_home(char *home, size_t home_size)
{
  Dl_info info;
  if (dladdr((void *) find_python_home, &info) && info.dli_fname)
  {
    char buf[PATH_MAX];
    strmake(buf, info.dli_fname, sizeof(buf) - 1);
    char *dir= dirname(buf);
    snprintf(home, home_size, "%s/lang_python", dir);

    char probe[PATH_MAX];
    snprintf(probe, sizeof(probe), "%s/lib/python%d.%d/encodings",
             home, PY_MAJOR_VERSION, PY_MINOR_VERSION);
    struct stat st;
    if (stat(probe, &st) == 0 && S_ISDIR(st.st_mode))
      return false;
  }

#ifdef PYTHON_HOME_BUILDDIR
  snprintf(home, home_size, "%s", PYTHON_HOME_BUILDDIR);
  return false;
#endif

  return true;
}


/* -------------------------------------------------------------------------
   Plugin init / deinit
   ---------------------------------------------------------------------- */

int lang_python_init(void * /* unused */)
{
  if (Py_IsInitialized())
    return 0;

  char home[PATH_MAX];
  if (find_python_home(home, sizeof(home)))
  {
    sql_print_error("lang_python: cannot determine PYTHONHOME");
    return 1;
  }

  /*
    Register the built-in `inhost` module (Phase 1.6).  Must happen before
    Py_Initialize; the guard keeps the inittab from accumulating
    duplicates across UNINSTALL/INSTALL cycles in one server process.
  */
  {
    static bool inittab_registered= false;
    if (!inittab_registered)
    {
      if (PyImport_AppendInittab("inhost", PyInit_lang_python_inhost) < 0)
      {
        sql_print_error("lang_python: PyImport_AppendInittab failed");
        return 1;
      }
      inittab_registered= true;
    }
  }

  PyConfig config;
  PyConfig_InitPythonConfig(&config);

  PyStatus status= PyConfig_SetBytesString(&config, &config.home, home);
  if (PyStatus_Exception(status))
  {
    sql_print_error("lang_python: PyConfig_SetBytesString failed: %s",
                    status.err_msg ? status.err_msg : "unknown error");
    PyConfig_Clear(&config);
    return 1;
  }

  status= Py_InitializeFromConfig(&config);
  PyConfig_Clear(&config);
  if (PyStatus_Exception(status))
  {
    sql_print_error("lang_python: Py_InitializeFromConfig failed: %s",
                    status.err_msg ? status.err_msg : "unknown error");
    return 1;
  }

  /*
    Promote libpython symbols to global visibility.

    MariaDB loads plugins via dlopen(path, RTLD_NOW) (sql_plugin.cc:780).
    Without an explicit RTLD_GLOBAL flag, dlopen() defaults to RTLD_LOCAL,
    which keeps all symbols from the loaded .so (and its dependencies)
    private.  This is intentional on MariaDB's side: RTLD_LOCAL prevents
    symbol collisions between unrelated plugins that might link different
    versions of the same library.

    However, Python's C extension modules (.so files in lib-dynload/ such
    as math, _json, _decimal, etc.) are loaded by the interpreter at
    runtime via their own dlopen() calls.  Those calls need to resolve
    symbols exported by libpython (e.g. PyFloat_Type, PyExc_ValueError).
    With RTLD_LOCAL in effect, those symbols are invisible and the import
    fails with "undefined symbol".

    The fix: re-dlopen the already-loaded libpython by its full path with
    RTLD_GLOBAL | RTLD_NOW.  Since the library is already in memory this
    does not reload it; it only promotes its symbols to global scope.
    RTLD_NOW (vs RTLD_LAZY) ensures any issues surface immediately here
    rather than causing a crash later during a user query.

    This is the same approach used by PostgreSQL's PL/Python.
  */
  {
    char libpath[PATH_MAX];
    snprintf(libpath, sizeof(libpath), "%s/lib/libpython%d.%d.so.1.0",
             home, PY_MAJOR_VERSION, PY_MINOR_VERSION);
    if (!dlopen(libpath, RTLD_GLOBAL | RTLD_NOW))
      sql_print_warning("lang_python: dlopen(%s, RTLD_GLOBAL) failed: %s",
                        libpath, dlerror());
  }

  /*
    Check mandatory Python packages.
    numpy must be importable; the reference is released immediately since
    sys.modules keeps numpy alive for the interpreter's lifetime.
  */
  {
    PyObject *np= PyImport_ImportModule("numpy");
    if (!np)
    {
      PyErr_Clear();
      sql_print_error("lang_python: mandatory package 'numpy' is not installed. "
                      "Install it with: <python_home>/bin/pip install numpy");
      Py_Finalize();
      return 1;
    }
    Py_DECREF(np);
  }

  /*
    Initialize the numpy C API (PyArray_SimpleNew, PyArray_DATA, etc.).
    _import_array() is the underlying function behind the import_array()
    macro.  We call it directly because the macro expands to
    "return NULL" on failure, which doesn't compile in an int-returning
    function.  Must be called after Py_Initialize and before any
    PyArray_* usage.
  */
  if (_import_array() < 0)
  {
    PyErr_Clear();
    sql_print_error("lang_python: failed to initialize numpy C API");
    Py_Finalize();
    return 1;
  }

  if (create_names())
  {
    PyErr_Clear();
    release_names();
    sql_print_error("lang_python: failed to create the interned names");
    Py_Finalize();
    return 1;
  }

  if (lang_routine_init())
  {
    release_names();
    sql_print_error("lang_python: failed to prepare the routine language");
    Py_Finalize();
    return 1;
  }

  numpy_available= true;

  sql_print_information("lang_python: Python %d.%d initialised, numpy available",
                        PY_MAJOR_VERSION, PY_MINOR_VERSION);

  main_tstate= PyEval_SaveThread();

  /*
    Load the named-function catalog (Phase 1.5).  Best-effort: during
    early server startup the SQL layer is not fully up (e.g. the Aria
    engine backing the catalog registers after this plugin), in which
    case every catalog consumer retries lazily.  A [Note], not a
    [Warning]: this is expected initialization ordering.
  */
  {
    char err[512];
    if (py_catalog_ensure(err, sizeof(err)))
      sql_print_information("lang_python: named-function catalog deferred "
                            "(%s); it will be loaded on first use", err);
  }
  return 0;
}

int lang_python_deinit(void * /* unused */)
{
  if (!Py_IsInitialized())
    return 0;

  /* Entry destructors take the GIL, so this must precede Py_Finalize */
  py_catalog_shutdown();

  if (main_tstate)
  {
    PyEval_RestoreThread(main_tstate);
    main_tstate= nullptr;
  }

  numpy_available= false;
  lang_routine_deinit();
  release_names();
  Py_Finalize();
  return 0;
}
