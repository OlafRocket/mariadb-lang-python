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
  Columnar row sink (see py_inhost_rowsink.h): runs every inhost.sql
  statement (query, execute, query_array).

  Execution context: a fresh THD(0) set up like the one
  mysql_real_connect_local() creates for a thread that is mid-statement
  (sql/sql_prepare.cc): grants skipped, binlog off, query cache off.
  Additionally the session character sets are utf8mb4, so the statement
  text is read as UTF-8 regardless of the server defaults.

  The statement runs through the public
  Ed_connection::execute_direct(Protocol *, LEX_STRING), which accepts any
  Protocol.  Column_sink receives each value through the typed
  Protocol::store_*() calls and appends it to the column buffer.

  Server-internal behaviour this file depends on (re-check when the server
  pin moves):

    - select_send::send_data() calls prepare_for_resend() before every row
      and calls write() only when thd->vio_ok().  An internal THD has no
      vio, so the row boundary is taken from prepare_for_resend().
    - Field::send() / Type_handler::Item_send() dispatch to the typed
      store_*() methods for any Protocol that is not a Protocol_text.
      DECIMAL is the exception and arrives through store_str().
*/

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>
#include <sql_prepare.h>
#include <protocol.h>
#include <my_decimal.h>
#include <sql_type_vector.h>

#include <limits>

#include "py_inhost_rowsink.h"


Rowsink_kind rowsink_kind_for(const Type_handler *th, bool unsigned_flag)
{
  if (th == &type_handler_vector)
    return ROWSINK_VECTOR;
  switch (th->field_type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR:
    return unsigned_flag ? ROWSINK_UINT64 : ROWSINK_INT64;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return ROWSINK_FLOAT64;
  default:
    return ROWSINK_OBJECT;
  }
}


class Column_sink final : public Protocol
{
  Rowsink_result *m_out;
  THD *m_outer;                           /* THD of the calling statement */
  Rowsink_options m_opts;
  uint m_pos= 0;                          /* next column of the current row */
  bool m_discard= false;                  /* inside a discarded result set */
  std::vector<Rowsink_column> *m_cols;    /* columns of the current set */
  size_t *m_rows;                         /* row count of the current set */
  std::vector<char> m_float32;            /* exact mode: column is FLOAT */
  std::vector<char> m_text_number;        /* exact mode: column is DECIMAL */

  bool fail(const char *what)
  {
    const char *name= m_pos < m_cols->size()
                      ? (*m_cols)[m_pos].name.c_str() : "?";
    my_printf_error(ER_UNKNOWN_ERROR, "query_array: column '%s': %s", MYF(0),
                    name, what);
    return true;
  }
  Rowsink_column *next()
  {
    return m_pos < m_cols->size() ? &(*m_cols)[m_pos] : nullptr;
  }

  bool put_int(longlong v)
  {
    if (m_discard)
      return false;
    Rowsink_column *c= next();
    if (!c)
      return fail("value outside the result set metadata");
    switch (c->kind)
    {
    case ROWSINK_INT64:
    case ROWSINK_UINT64:
      c->ints.push_back(v);
      break;
    case ROWSINK_FLOAT64:
      c->doubles.push_back((double) v);
      break;
    default:
      return fail("unexpected integer value");
    }
    m_pos++;
    return false;
  }
  bool put_double(double v)
  {
    if (m_discard)
      return false;
    Rowsink_column *c= next();
    if (!c)
      return fail("value outside the result set metadata");
    if (c->kind != ROWSINK_FLOAT64)
      return fail("unexpected floating point value");
    c->doubles.push_back(v);
    m_pos++;
    return false;
  }
  bool put_object(const char *from, size_t length, Rowsink_object_tag tag)
  {
    if (m_discard)
      return false;
    Rowsink_column *c= next();
    if (!c)
      return fail("value outside the result set metadata");
    if (c->kind != ROWSINK_OBJECT)
      return fail("unexpected string value");
    c->bytes.append(from, length);
    c->ends.push_back(c->bytes.size());
    c->tags.push_back((char) tag);
    m_pos++;
    return false;
  }
  bool put_time(const MYSQL_TIME *t, int decimals)
  {
    char buf[MAX_DATE_STRING_REP_LENGTH];
    int len= my_TIME_to_str(t, buf, decimals);
    return put_object(buf, len, ROWSINK_TAG_STR);
  }

protected:
  /* The statement outcome is read from Ed_connection, not sent anywhere */
  bool send_ok(uint, uint, ulonglong, ulonglong, const char *) override
  { return false; }
  bool send_eof(uint, uint) override { return false; }
  bool send_error(uint, const char *, const char *) override { return false; }

public:
  Column_sink(THD *thd_arg, THD *outer, const Rowsink_options &opts,
              Rowsink_result *out)
    : Protocol(thd_arg), m_out(out), m_outer(outer), m_opts(opts),
      m_cols(&out->cols), m_rows(&out->rows) {}

  bool send_result_set_metadata(List<Item> *list, uint) override
  {
    if (m_out->has_result_set)
    {
      switch (m_opts.multi)
      {
      case Rowsink_options::ROWSINK_MULTI_FIRST:
        m_discard= true;
        return prepare_for_send(list->elements);
      case Rowsink_options::ROWSINK_MULTI_ALL:
        m_out->extra_sets.emplace_back();
        m_cols= &m_out->extra_sets.back().cols;
        m_rows= &m_out->extra_sets.back().rows;
        break;
      case Rowsink_options::ROWSINK_MULTI_ERROR:
        my_printf_error(ER_UNKNOWN_ERROR, "%s", MYF(0), m_opts.multi_error);
        return true;
      }
    }
    m_out->has_result_set= true;
    m_cols->resize(list->elements);
    m_float32.assign(list->elements, 0);
    m_text_number.assign(list->elements, 0);

    List_iterator_fast<Item> it(*list);
    Item *item;
    for (uint i= 0; (item= it++); i++)
    {
      Rowsink_column &c= (*m_cols)[i];
      if (item->name.str)
        c.name.assign(item->name.str, item->name.length);

      c.kind= rowsink_kind_for(item->type_handler(), item->unsigned_flag);
      if (m_opts.exact)
      {
        enum_field_types ft= item->type_handler()->field_type();
        if (ft == MYSQL_TYPE_DECIMAL || ft == MYSQL_TYPE_NEWDECIMAL)
        {
          c.kind= ROWSINK_OBJECT;
          m_text_number[i]= 1;
        }
        else if (c.kind == ROWSINK_VECTOR)
          c.kind= ROWSINK_OBJECT;
        else if (ft == MYSQL_TYPE_FLOAT)
          m_float32[i]= 1;
      }
    }
    return prepare_for_send(list->elements);
  }

  /* Start of a row (see the file comment for why not write()) */
  void prepare_for_resend() override
  {
    if (!m_discard)
      (*m_rows)++;
    m_pos= 0;
    IF_DBUG(field_pos= 0,);
    /*
      The internal THD is not visible to KILL.  Forward a kill of the
      calling statement so a long extraction stops at the next row.
    */
    if (m_outer && m_outer->killed && !thd->killed)
      thd->set_killed(KILL_QUERY);
  }
  bool write() override { return false; }
  bool flush() override { return false; }

  bool store_null() override
  {
    if (m_discard)
      return false;
    Rowsink_column *c= next();
    if (!c)
      return fail("value outside the result set metadata");
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
    case ROWSINK_VECTOR:
      return fail("NULL in a VECTOR column");
    case ROWSINK_OBJECT:
      c->ends.push_back(c->bytes.size());
      c->tags.push_back((char) ROWSINK_TAG_NULL);
      break;
    }
    m_pos++;
    return false;
  }
  bool store_tiny(longlong from) override { return put_int(from); }
  bool store_short(longlong from) override { return put_int(from); }
  bool store_long(longlong from) override { return put_int(from); }
  bool store_longlong(longlong from, bool) override { return put_int(from); }
  bool store_float(float from, uint32) override
  {
    if (!m_discard && m_pos < m_float32.size() && m_float32[m_pos])
    {
      /* Shortest decimal form of the float, as the text protocol prints */
      char buf[FLOATING_POINT_BUFFER];
      size_t len= my_gcvt(from, MY_GCVT_ARG_FLOAT, sizeof(buf) - 1, buf, NULL);
      char *end= buf + len;
      int err;
      return put_double(my_strtod(buf, &end, &err));
    }
    return put_double(from);
  }
  bool store_double(double from, uint32) override { return put_double(from); }
  bool store_decimal(const my_decimal *d) override
  {
    if (!m_discard && m_pos < m_text_number.size() && m_text_number[m_pos])
    {
      StringBuffer<DECIMAL_MAX_STR_LENGTH + 1> str;
      d->to_string(&str);
      return put_object(str.ptr(), str.length(), ROWSINK_TAG_STR);
    }
    double v;
    my_decimal2double(E_DEC_FATAL_ERROR, d, &v);
    return put_double(v);
  }
  bool store_str(const char *from, size_t length,
                 CHARSET_INFO *fromcs, CHARSET_INFO *) override
  {
    if (m_discard)
      return false;
    Rowsink_column *c= next();
    if (!c)
      return fail("value outside the result set metadata");
    switch (c->kind)
    {
    case ROWSINK_FLOAT64:                 /* DECIMAL arrives as text */
    {
      char *end= (char *) from + length;
      int err;
      c->doubles.push_back(my_strtod(from, &end, &err));
      m_pos++;
      return false;
    }
    case ROWSINK_INT64:
    case ROWSINK_UINT64:
    {
      char *end= (char *) from + length;
      int err;
      c->ints.push_back(my_strtoll10(from, &end, &err));
      m_pos++;
      return false;
    }
    case ROWSINK_VECTOR:
    {
      size_t dim= length / sizeof(float);
      if (length % sizeof(float) || dim == 0)
        return fail("malformed VECTOR value");
      if (c->floats.empty())
        c->dim= dim;
      else if (dim != c->dim)
        return fail("VECTOR values of different dimensions");
      size_t old= c->floats.size();
      c->floats.resize(old + dim);
      memcpy(&c->floats[old], from, length);
      m_pos++;
      return false;
    }
    case ROWSINK_OBJECT:
      break;
    }

    /* DECIMAL text is ASCII digits whatever charset the server labels it */
    if (m_pos < m_text_number.size() && m_text_number[m_pos])
      return put_object(from, length, ROWSINK_TAG_STR);
    if (fromcs == &my_charset_bin)
      return put_object(from, length, ROWSINK_TAG_BYTES);
    if (my_charset_same(fromcs, &my_charset_utf8mb4_bin))
      return put_object(from, length, ROWSINK_TAG_STR);

    StringBuffer<256> conv;
    uint errors;
    if (conv.copy(from, length, fromcs, &my_charset_utf8mb4_bin, &errors))
      return fail("out of memory");
    return put_object(conv.ptr(), conv.length(), ROWSINK_TAG_STR);
  }
  bool store_datetime(MYSQL_TIME *t, int decimals) override
  { return put_time(t, decimals); }
  bool store_date(MYSQL_TIME *t) override { return put_time(t, 0); }
  bool store_time(MYSQL_TIME *t, int decimals) override
  { return put_time(t, decimals); }
  bool store(Field *field) override
  {
    if (field->is_null())
      return store_null();
    return field->send(this);
  }
  bool send_out_parameters(List<Item_param> *) override { return false; }

  enum enum_protocol_type type() override { return PROTOCOL_LOCAL; }
};


bool rowsink_run(const char *sql, size_t length,
                 const Rowsink_options &options, Rowsink_result *out)
{
  THD *thd_orig= current_thd;
  my_thread_id orig_mysys_id= my_thread_var->id;

  THD *thd= new THD(0);
  thd->store_globals();
  thd->security_ctx->skip_grants();
  thd->query_cache_is_applicable= 0;
  thd->variables.wsrep_on= 0;
  thd->client_capabilities= CLIENT_MULTI_RESULTS;
  thd->variables.sql_log_bin= 0;
  thd->affected_rows= 0;
  thd->set_binlog_bit();
  bzero((char *) &thd->net, sizeof(thd->net));
  /* Parameter escaping in inhost.sql relies on backslash escapes */
  thd->variables.sql_mode&= ~MODE_NO_BACKSLASH_ESCAPES;
  thd->variables.character_set_client= &my_charset_utf8mb4_general_ci;
  thd->variables.collation_connection= &my_charset_utf8mb4_general_ci;
  thd->variables.character_set_results= &my_charset_utf8mb4_general_ci;
  thd->update_charset();
  thd->set_time();

  std::string text(sql, length);
  bool failed;
  {
    Column_sink sink(thd, thd_orig, options, out);
    Ed_connection con(thd);
    LEX_STRING sql_text= { &text[0], text.length() };
    failed= con.execute_direct(&sink, sql_text);
    if (failed)
    {
      out->err_no= con.get_last_errno();
      out->err_msg= con.get_last_error();
    }
    else if (!out->has_result_set)
      out->affected_rows= con.get_affected_rows();
  }

  delete thd;
  set_current_thd(thd_orig);
  my_thread_var->id= orig_mysys_id;
  return failed;
}
