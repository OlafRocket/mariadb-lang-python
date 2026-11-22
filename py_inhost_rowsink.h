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

#ifndef LANG_PYTHON_PY_INHOST_ROWSINK_H
#define LANG_PYTHON_PY_INHOST_ROWSINK_H

/*
  Columnar row sink: the SQL execution path of inhost.sql (query,
  execute, query_array).

  rowsink_run() executes one SQL statement on a private internal THD and
  receives the result through a plugin-defined Protocol subclass: the
  executor's typed values (store_longlong, store_double, ...) are appended
  to per-column C buffers as each row is produced.  No text conversion, no
  MYSQL_ROW buffering, no Python objects.

  This header is free of server and Python types so both sides can include
  it.  rowsink_run() touches no Python state and runs with the GIL released.
*/

#include <stddef.h>

#include <string>
#include <vector>

enum Rowsink_kind
{
  ROWSINK_INT64,                          /* signed integer types, YEAR      */
  ROWSINK_UINT64,                         /* unsigned integer types          */
  ROWSINK_FLOAT64,                        /* FLOAT, DOUBLE, DECIMAL          */
  ROWSINK_VECTOR,                         /* VECTOR: rows x dim float32      */
  ROWSINK_OBJECT                          /* everything else: str / bytes    */
};

/* Per-row tag of a ROWSINK_OBJECT column */
enum Rowsink_object_tag
{
  ROWSINK_TAG_NULL= 0,
  ROWSINK_TAG_STR= 1,                     /* UTF-8 text                      */
  ROWSINK_TAG_BYTES= 2                    /* binary string                   */
};

struct Rowsink_column
{
  std::string name;
  Rowsink_kind kind= ROWSINK_OBJECT;

  std::vector<long long> ints;            /* INT64 / UINT64 (raw 64 bits)    */
  std::vector<size_t> null_rows;          /* INT64 / UINT64: rows with NULL  */
  std::vector<double> doubles;            /* FLOAT64, NULL stored as NaN     */
  std::vector<float> floats;              /* VECTOR payload, rows * dim      */
  size_t dim= 0;                          /* VECTOR dimension                */
  std::string bytes;                      /* OBJECT: concatenated values     */
  std::vector<size_t> ends;               /* OBJECT: end offset per row      */
  std::vector<char> tags;                 /* OBJECT: Rowsink_object_tag      */
};

struct Rowsink_options
{
  /*
    Lossless mode, for inhost.sql.query(): DECIMAL stays text (OBJECT
    column of str), FLOAT converts through its shortest decimal form
    (0.1f -> 0.1, not 0.100000001490116), VECTOR is an OBJECT column of
    bytes so it can be NULL.  Without it (query_array) DECIMAL is
    float64 and VECTOR a 2-D float32 block.
  */
  bool exact= false;
  /*
    A statement may return several result sets (CALL of a procedure with
    more than one SELECT).
      ROWSINK_MULTI_ERROR  fail the statement at the second result set,
                           with the message multi_error
      ROWSINK_MULTI_FIRST  keep the first, discard the rest (execute)
      ROWSINK_MULTI_ALL    keep all: the first in cols/rows, the others
                           in extra_sets (query_sets)
  */
  enum Multi { ROWSINK_MULTI_ERROR, ROWSINK_MULTI_FIRST, ROWSINK_MULTI_ALL };
  Multi multi= ROWSINK_MULTI_ERROR;
  const char *multi_error= "statement returns more than one result set";
};

struct Rowsink_set
{
  size_t rows= 0;
  std::vector<Rowsink_column> cols;
};

struct Rowsink_result
{
  bool has_result_set= false;
  size_t rows= 0;
  std::vector<Rowsink_set> extra_sets;    /* 2nd+ sets, ROWSINK_MULTI_ALL */
  unsigned long long affected_rows= 0;    /* statements without result set */
  std::vector<Rowsink_column> cols;       /* first result set */
  unsigned int err_no= 0;
  std::string err_msg;
};

/*
  Column kind for a value of the given server type; shared by the row sink
  (result metadata) and py_agg (argument types).
*/
class Type_handler;
Rowsink_kind rowsink_kind_for(const Type_handler *th, bool unsigned_flag);

/* Returns false on success; on failure err_no / err_msg are set. */
bool rowsink_run(const char *sql, size_t length,
                 const Rowsink_options &options, Rowsink_result *out);

#endif /* LANG_PYTHON_PY_INHOST_ROWSINK_H */
