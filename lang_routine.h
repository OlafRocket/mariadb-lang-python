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

#ifndef LANG_PYTHON_LANG_ROUTINE_H
#define LANG_PYTHON_LANG_ROUTINE_H

/*
  The routine language PYTHON: stored functions and procedures whose body
  is Python.

    CREATE FUNCTION multiply(x INT, y INT) RETURNS INT
      LANGUAGE PYTHON AS $$
        return x * y
    $$;

    CREATE PROCEDURE split(IN s VARCHAR(100), OUT head VARCHAR(100),
                           OUT tail VARCHAR(100))
      LANGUAGE PYTHON AS $$
        head, _, tail = s.partition(' ')
    $$;

  Body
    The body is the body of a Python function whose parameters are the
    SQL parameters, by name.  Common leading whitespace is removed before
    the body is compiled (textwrap.dedent), so a body can be indented to
    match the SQL around it.  Line numbers in errors are lines of the
    body as written.

    Function: `return` gives the result.  A body that ends without
    `return` returns NULL.

    Procedure: an OUT or INOUT parameter is assigned by its name.  The
    caller receives what the name holds when the body ends, also after a
    `return`.  An OUT parameter starts as None.  `return` with a value is
    an error at CREATE.

  Arguments (SQL to Python)
    NULL                      None
    integer types             int
    FLOAT, DOUBLE             float
    DECIMAL                   decimal.Decimal
    CHAR, VARCHAR, TEXT, JSON str
    BINARY, VARBINARY, BLOB   bytes
    VECTOR                    numpy.ndarray(float32), or bytes with
                              lang_python_vector_numpy=OFF
    DATE, TIME, DATETIME,
    TIMESTAMP                 str

  Result, OUT and INOUT parameters (Python to SQL)
    None                      NULL
    bool, int                 integer
    float                     double; NaN and the infinities are an error
    decimal.Decimal           its digits
    str                       text
    bytes, bytearray, any
    object with a buffer
    (numpy.ndarray)           bytes
    dict, list, tuple, set,
    callables, generators     an error
    any other object          str(object)

    The server converts the stored value to the declared type under the
    sql_mode of the routine: in strict mode a value that does not fit is
    an error.

  Errors
    A body that does not compile is refused at CREATE (ER_PARSE_ERROR).
    A Python exception at the call is an SQL error (ER_UNKNOWN_ERROR) and
    ends the statement.  lang_python_strict_errors is not read.  Text:
      Python: <schema>.<name>: <Exception>: <message> at line <N>

  Namespace
    The compiled code of a routine is shared by all sessions.  Each
    session has its own namespace (the globals of the function) per
    routine, created at the first call and released with the routine
    cache entry of the session.  Nothing is shared between sessions.

  Threads
    Each call takes the GIL with PyGILState_Ensure and gives it back with
    PyGILState_Release.  The thread state is not kept between calls.
*/

struct st_mariadb_language;               /* mysql/plugin_language.h */

/* The descriptor of the language plugin PYTHON */
extern struct st_mariadb_language lang_python_language;

/*
  Called by the daemon plugin with the GIL held: after the interpreter
  is initialised, and before it is finalised.  init returns true on
  failure, with the Python exception cleared.
*/
bool lang_routine_init();
void lang_routine_deinit();

#endif /* LANG_PYTHON_LANG_ROUTINE_H */
