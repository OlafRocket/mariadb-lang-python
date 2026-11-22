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
  lang_python plugin: entry point.

  This file contains only the maria_declare_plugin block that registers
  the daemon plugin (init/deinit, system variables) and the function
  plugins (py_eval, py_exec, and the Phase 1.5 named-function surface:
  py_create_function, py_replace_function, py_drop_function, py_call;
  and the Phase 2 aggregate py_agg).
*/

#include "item_func_python.h"
#include "item_func_py_named.h"
#include "item_sum_py_agg.h"
#include "lang_python.h"


/* -------------------------------------------------------------------------
   Plugin declaration
   ---------------------------------------------------------------------- */

static struct st_mysql_daemon lang_python_daemon_plugin=
{ MYSQL_DAEMON_INTERFACE_VERSION };

maria_declare_plugin(lang_python)
{
  /* Daemon plugin entry: owns init/deinit and system variables.
     Variables appear as @@lang_python_<name> (e.g. @@lang_python_vector_numpy).
     The function plugins below depend on this entry for Python init. */
  MYSQL_DAEMON_PLUGIN,
  &lang_python_daemon_plugin,
  "lang_python",
  "Olaf Razzoli",
  "Embedded Python interpreter for MariaDB (settings and lifecycle)",
  PLUGIN_LICENSE_GPL,
  lang_python_init,
  lang_python_deinit,
  0x0100,
  NULL,                      /* status variables */
  lang_python_sys_vars(),    /* system variables */
  "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_eval::plugin_descriptor(),
  "py_eval",
  "Olaf Razzoli",
  "py_eval(expr, type [, args...]) - evaluate Python expression",
  PLUGIN_LICENSE_GPL,
  NULL,                      /* init handled by lang_python daemon plugin */
  NULL,                      /* deinit handled by lang_python daemon plugin */
  0x0100,
  NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_exec::plugin_descriptor(),
  "py_exec",
  "Olaf Razzoli",
  "py_exec(code, type [, args...]) - execute Python block, return result",
  PLUGIN_LICENSE_GPL,
  NULL,                      /* init handled by lang_python daemon plugin */
  NULL,                      /* deinit handled by lang_python daemon plugin */
  0x0100,
  NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_manage::create_descriptor(),
  "py_create_function",
  "Olaf Razzoli",
  "py_create_function(name, type, params, body [, strict]) - "
  "create named Python function",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_manage::replace_descriptor(),
  "py_replace_function",
  "Olaf Razzoli",
  "py_replace_function(name, type, params, body [, strict]) - "
  "create or replace named Python function",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_manage::drop_descriptor(),
  "py_drop_function",
  "Olaf Razzoli",
  "py_drop_function(name [, if_exists]) - drop named Python function",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_call::plugin_descriptor(),
  "py_call",
  "Olaf Razzoli",
  "py_call(name [, args...]) - invoke named Python function",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_manage::create_aggregate_descriptor(),
  "py_create_aggregate",
  "Olaf Razzoli",
  "py_create_aggregate(name, type, params, body [, strict]) - "
  "create named Python aggregate (class Aggregate: step/finalize)",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_func_py_manage::replace_aggregate_descriptor(),
  "py_replace_aggregate",
  "Olaf Razzoli",
  "py_replace_aggregate(name, type, params, body [, strict]) - "
  "create or replace named Python aggregate",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
},
{
  MariaDB_FUNCTION_PLUGIN,
  Item_sum_py_agg::plugin_descriptor(),
  "py_agg",
  "Olaf Razzoli",
  "py_agg(name, cols...) - aggregate: pass each group to a named Python "
  "function or aggregate as numpy arrays",
  PLUGIN_LICENSE_GPL,
  NULL, NULL, 0x0100, NULL, NULL, "1.0",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
