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

#ifndef LANG_PYTHON_H
#define LANG_PYTHON_H

/* -------------------------------------------------------------------------
   Plugin runtime state: defined in lang_python.cc
   ------------------------------------------------------------------------- */

#include <cstddef>

class THD;
typedef struct _object PyObject;          /* Python.h (CPython) */

bool lang_python_use_numpy();
bool lang_python_strict_errors(THD *thd);
bool lang_python_db_access();
unsigned long long lang_python_agg_max_mem(THD *thd);
unsigned long long lang_python_agg_chunk_rows(THD *thd);

/* -------------------------------------------------------------------------
   Shared Python helpers: defined in item_func_python.cc.
   Both require the GIL to be held by the caller.
   ------------------------------------------------------------------------- */

/* textwrap.dedent() the source; new reference, or nullptr on failure
   (caller falls back to the raw source). */
PyObject *lang_python_dedent_source(const char *src);

/* Render the pending Python exception as "<prefix>: <Type>: <message>"
   into buf and clear the exception state. */
void lang_python_format_exc(const char *prefix, char *buf, size_t buflen);

int lang_python_init(void *);
int lang_python_deinit(void *);

struct st_mysql_sys_var;                  /* mysql/plugin.h */
st_mysql_sys_var **lang_python_sys_vars();

#endif /* LANG_PYTHON_H */
