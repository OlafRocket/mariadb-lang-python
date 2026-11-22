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

#include "py_thread_pin.h"

#include <my_global.h>
#include <my_dbug.h>


void Py_thread_pin::pin_slow()
{
  /*
    The thread has a thread state already: something further out keeps it
    alive (another pin, or Python code that called into SQL).  Ensure /
    Release are cheap there, and releasing the GIL below would take it
    away from a caller that holds it.
  */
  if (PyGILState_GetThisThreadState())
    return;

  m_gil_state= PyGILState_Ensure();
  m_thread_state= PyEval_SaveThread();
  m_thread= std::this_thread::get_id();
  m_pinned= true;
}


void Py_thread_pin::unpin()
{
  if (!m_pinned)
    return;
  m_pinned= false;

  /*
    The thread state belongs to the pinning thread and can only be
    destroyed there.  On another thread (not expected) it is left to
    Py_Finalize.  After Py_Finalize it is gone already.
  */
  DBUG_ASSERT(m_thread == std::this_thread::get_id());
  if (m_thread != std::this_thread::get_id() || !Py_IsInitialized())
    return;

  PyEval_RestoreThread(m_thread_state);
  PyGILState_Release(m_gil_state);
  m_thread_state= nullptr;
}
