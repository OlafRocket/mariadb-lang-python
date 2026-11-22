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

#ifndef LANG_PYTHON_PY_THREAD_PIN_H
#define LANG_PYTHON_PY_THREAD_PIN_H

/*
  Py_thread_pin: keeps the Python thread state of a server thread alive
  between the Python calls of one statement.

  A server thread has no Python thread state.  PyGILState_Ensure() then
  creates one and the matching PyGILState_Release() destroys it, on every
  call: one allocation, one stack-limit query and one teardown per row.

  pin() creates the thread state once and gives the GIL back
  (PyGILState_Ensure + PyEval_SaveThread).  The PyGILState_Ensure /
  PyGILState_Release pairs around each Python call stay as they are; they
  nest inside the pin and only take and release the GIL.  unpin() destroys
  the thread state.

  The GIL is not held between calls: the pin changes the lifetime of the
  thread state, not the locking.

  A thread that already has a thread state (a statement run from Python
  through inhost.sql, or a second Python item of the same statement) is
  not pinned again; pin() is then a no-op.

  pin() and unpin() run on the same thread: a statement does not change
  threads while it executes, and the owning Item unpins in cleanup().
  A copied pin starts unpinned.
*/

#include <Python.h>

#include <thread>

class Py_thread_pin
{
  bool m_pinned= false;
  PyGILState_STATE m_gil_state= PyGILState_UNLOCKED;
  PyThreadState *m_thread_state= nullptr;
  std::thread::id m_thread;

  void pin_slow();

public:
  Py_thread_pin() = default;
  Py_thread_pin(const Py_thread_pin &) {}
  Py_thread_pin &operator=(const Py_thread_pin &) { return *this; }
  ~Py_thread_pin() { unpin(); }

  /* Call without the GIL, before the first PyGILState_Ensure of a call */
  void pin()
  {
    if (!m_pinned)
      pin_slow();
  }
  void unpin();
};

#endif /* LANG_PYTHON_PY_THREAD_PIN_H */
