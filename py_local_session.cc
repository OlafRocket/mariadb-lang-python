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


#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>                    /* current_thd, set_current_thd */

#include <mysql.h>
#include <mysql/service_sql.h>

#include "py_local_session.h"


bool py_connect_own_session(MYSQL *m)
{
  /*
    THD::store_globals() of the THD that the service creates takes over
    the mysys thread id of this thread; it is restored here together
    with the current THD.
  */
  THD *caller= current_thd;
  my_thread_id caller_mysys_id= my_thread_var->id;
  set_current_thd(nullptr);
  bool failed= mysql_real_connect_local(m) == nullptr;
  set_current_thd(caller);
  my_thread_var->id= caller_mysys_id;
  return failed;
}
