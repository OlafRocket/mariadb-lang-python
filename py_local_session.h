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

#ifndef LANG_PYTHON_PY_LOCAL_SESSION_H
#define LANG_PYTHON_PY_LOCAL_SESSION_H

/*
  Local connection of the plugin SQL service on a session of its own.

  mysql_real_connect_local() runs the statements of a connection on the
  THD of the calling thread when that THD holds no table locks, and on a
  THD of its own otherwise.  The plugin wants one behaviour for every
  caller: SQL that the plugin runs never shares the session or the
  transaction of the statement that triggered it.

  py_connect_own_session() opens the connection while no THD is current,
  which makes the service create its own THD.  It returns false on
  success; on failure the error is in mysql_error(m).  The caller closes
  the connection with mysql_close() in both cases.
*/

struct st_mysql;

bool py_connect_own_session(struct st_mysql *m);

#endif /* LANG_PYTHON_PY_LOCAL_SESSION_H */
