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
  Row sink (see py_inhost_rowsink.h): runs every inhost.sql statement
  (query, query_sets, execute, query_array).

  The statement runs through the plugin SQL service
  (include/mysql/service_sql.h): mysql_real_connect_local(),
  mysql_real_query(), mysql_store_result(), mysql_fetch_row().  The
  service returns every value as text and stores the whole result before
  the first row can be read.  Each value is parsed here into the column
  buffer chosen from the result metadata.

  Execution context: a separate internal THD per call.  The service runs
  on the THD of the caller when that THD holds no table locks; to keep
  one behaviour for all callers (own session, own transaction), the local
  connection is opened while no THD is current, which makes the service
  create its own.  The session character sets are set to utf8mb4 and
  NO_BACKSLASH_ESCAPES is removed from sql_mode, as the parameter
  escaping of inhost.sql expects.

  Beyond the functions of the SQL service this file uses, from the client
  structures in mysql.h and sql_common.h:

    - MYSQL_RES::fields and field_count: column names and types;
    - MYSQL::methods->fetch_lengths: value lengths (the service has no
      mysql_fetch_lengths), needed for binary values;
    - MYSQL::methods->read_query_result and MYSQL::server_status: the
      following result sets of a CALL (the service has no
      mysql_next_result).
*/

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>                    /* current_thd, set_current_thd */

#include <mysql.h>
#include <sql_common.h>                   /* st_mysql_methods */
#include <mysql/service_sql.h>

#include <stdlib.h>
#include <string.h>

#include <limits>

#include "py_inhost_rowsink.h"


static const char SESSION_SETUP[]=
  "SET character_set_client=utf8mb4, character_set_results=utf8mb4,"
  " collation_connection=utf8mb4_general_ci,"
  " sql_mode=TRIM(BOTH ',' FROM REPLACE(CONCAT(',',@@sql_mode,','),"
  "',NO_BACKSLASH_ESCAPES,',','))";


/* How the text of one column is stored */
enum Text_as
{
  TEXT_INT,                               /* signed integer   -> ints    */
  TEXT_UINT,                              /* unsigned integer -> ints    */
  TEXT_DOUBLE,                            /* number           -> doubles */
  TEXT_STR,                               /* UTF-8 text       -> bytes   */
  TEXT_BYTES                              /* binary string    -> bytes   */
};


static void describe_column(const MYSQL_FIELD *f, bool exact,
                            Rowsink_column *c, Text_as *as)
{
  if (f->name)
    c->name.assign(f->name, f->name_length);

  switch (f->type)
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR:
    if (f->flags & UNSIGNED_FLAG)
    {
      c->kind= ROWSINK_UINT64;
      *as= TEXT_UINT;
    }
    else
    {
      c->kind= ROWSINK_INT64;
      *as= TEXT_INT;
    }
    return;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    c->kind= ROWSINK_FLOAT64;
    *as= TEXT_DOUBLE;
    return;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    if (exact)
    {
      c->kind= ROWSINK_OBJECT;
      *as= TEXT_STR;
    }
    else
    {
      c->kind= ROWSINK_FLOAT64;
      *as= TEXT_DOUBLE;
    }
    return;
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_TIME:
  case MYSQL_TYPE_TIME2:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    c->kind= ROWSINK_OBJECT;              /* ASCII, labelled binary */
    *as= TEXT_STR;
    return;
  default:
    c->kind= ROWSINK_OBJECT;
    *as= f->charsetnr == my_charset_bin.number ? TEXT_BYTES : TEXT_STR;
    return;
  }
}


static void put_null(Rowsink_column *c)
{
  switch (c->kind)
  {
  case ROWSINK_INT64:
  case ROWSINK_UINT64:
    c->null_rows.push_back(c->ints.size());
    c->ints.push_back(0);
    break;
  case ROWSINK_FLOAT64:
    c->doubles.push_back(std::numeric_limits<double>::quiet_NaN());
    break;
  case ROWSINK_VECTOR:                    /* not produced by this sink */
  case ROWSINK_OBJECT:
    c->ends.push_back(c->bytes.size());
    c->tags.push_back((char) ROWSINK_TAG_NULL);
    break;
  }
}


/* The service terminates every value with a zero byte */
static void put_value(Rowsink_column *c, Text_as as, const char *v,
                      unsigned long length)
{
  switch (as)
  {
  case TEXT_INT:
    c->ints.push_back(strtoll(v, nullptr, 10));
    break;
  case TEXT_UINT:
    c->ints.push_back((long long) strtoull(v, nullptr, 10));
    break;
  case TEXT_DOUBLE:
  {
    char *end= (char *) v + length;
    int err;
    c->doubles.push_back(my_strtod(v, &end, &err));
    break;
  }
  case TEXT_STR:
  case TEXT_BYTES:
    c->bytes.append(v, length);
    c->ends.push_back(c->bytes.size());
    c->tags.push_back((char) (as == TEXT_STR ? ROWSINK_TAG_STR
                                              : ROWSINK_TAG_BYTES));
    break;
  }
}


static void read_set(MYSQL *m, MYSQL_RES *res, bool exact,
                     std::vector<Rowsink_column> *cols, size_t *rows)
{
  unsigned int n= res->field_count;
  std::vector<Text_as> as(n, TEXT_STR);
  std::vector<unsigned long> lengths(n, 0);

  cols->resize(n);
  for (unsigned int i= 0; i < n; i++)
    describe_column(&res->fields[i], exact, &(*cols)[i], &as[i]);

  while (MYSQL_ROW row= mysql_fetch_row(res))
  {
    m->methods->fetch_lengths(lengths.data(), row, n);
    for (unsigned int i= 0; i < n; i++)
    {
      if (row[i])
        put_value(&(*cols)[i], as[i], row[i], lengths[i]);
      else
        put_null(&(*cols)[i]);
    }
    (*rows)++;
  }
}


static void set_error(Rowsink_result *out, MYSQL *m)
{
  out->err_no= mysql_errno(m);
  out->err_msg= mysql_error(m);
  if (!out->err_no)
    out->err_no= ER_UNKNOWN_ERROR;
}


/*
  Reads every result of the statement just executed.  true = failure.

  All results are read also when a second result set is refused
  (ROWSINK_MULTI_ERROR): the service does not free results that are left
  unread when the connection is closed.
*/
static bool read_results(MYSQL *m, const Rowsink_options &options,
                         Rowsink_result *out)
{
  bool refused= false;
  for (;;)
  {
    if (m->field_count)
    {
      MYSQL_RES *res= mysql_store_result(m);
      if (!res)
      {
        set_error(out, m);
        return true;
      }
      if (!out->has_result_set)
      {
        out->has_result_set= true;
        read_set(m, res, options.exact, &out->cols, &out->rows);
      }
      else if (options.multi == Rowsink_options::ROWSINK_MULTI_ALL)
      {
        out->extra_sets.emplace_back();
        read_set(m, res, options.exact, &out->extra_sets.back().cols,
                 &out->extra_sets.back().rows);
      }
      else if (options.multi == Rowsink_options::ROWSINK_MULTI_ERROR)
        refused= true;
      mysql_free_result(res);
    }
    else if (!out->has_result_set)
      out->affected_rows= mysql_affected_rows(m);

    if (!(m->server_status & SERVER_MORE_RESULTS_EXISTS))
    {
      if (refused)
      {
        out->err_no= ER_UNKNOWN_ERROR;
        out->err_msg= options.multi_error;
      }
      return refused;
    }
    if (m->methods->read_query_result(m))
    {
      set_error(out, m);
      return true;
    }
  }
}


bool rowsink_run(const char *sql, size_t length,
                 const Rowsink_options &options, Rowsink_result *out)
{
  MYSQL *m= mysql_init(nullptr);
  if (!m)
  {
    out->err_no= ER_OUT_OF_RESOURCES;
    out->err_msg= "out of memory (mysql_init)";
    return true;
  }

  /*
    With no current THD the service creates a THD of its own.
    THD::store_globals() of that THD takes over the mysys thread id of
    this thread; both are restored here.
  */
  THD *caller= current_thd;
  my_thread_id caller_mysys_id= my_thread_var->id;
  set_current_thd(nullptr);
  bool connected= mysql_real_connect_local(m) != nullptr;
  set_current_thd(caller);
  my_thread_var->id= caller_mysys_id;

  bool failed= true;
  if (!connected ||
      mysql_real_query(m, SESSION_SETUP, sizeof(SESSION_SETUP) - 1) ||
      mysql_real_query(m, sql, (unsigned long) length))
    set_error(out, m);
  else
    failed= read_results(m, options, out);

  mysql_close(m);
  return failed;
}
