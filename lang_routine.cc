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
  The routine language PYTHON: Language_handler and Language_routine of
  the server on one side, the interpreter on the other.  See
  lang_routine.h for the behaviour.
*/

#include <Python.h>

#define MYSQL_SERVER
#include <my_global.h>
#include <sql_class.h>
#include <sp_pcontext.h>
#include <sp_rcontext.h>
#include <sql_type_vector.h>
#include <mysql/plugin_language.h>

#include "py_numpy.h"

#include <atomic>
#include <cmath>
#include <new>
#include <string.h>
#include <vector>

#include "lang_python.h"
#include "lang_routine.h"


/* -------------------------------------------------------------------------
   The part written in Python: compiling a body, creating the function of
   a session, describing an exception.

   compile_routine
     Places the statements of the body in a function node of the syntax
     tree.  The text of the body is not wrapped in source text: a body
     cannot close the function, and line numbers are those of the body.
     The code objects are kept in a table of weak references: sessions
     that load the same routine share one code object, and it is freed
     with the last of them.

     A procedure gets one more parameter, a list.  The body runs inside
     try/finally, and the finally clause copies the OUT and INOUT
     parameters into the list: also after a return in the body.
   ------------------------------------------------------------------------- */

static const char helper_source[]= R"PY(
import ast
import builtins
import keyword
import textwrap
import unicodedata
import weakref
from decimal import Decimal

_codes = weakref.WeakValueDictionary()
_SCOPES = (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)
_OUT = '__mariadb_out__'
_FUNC = '__mariadb_routine__'


def _error(filename, message, node=None):
    error = SyntaxError(message)
    error.filename = filename
    error.lineno = getattr(node, 'lineno', None)
    return error


def _own_nodes(statements):
    # The nodes of the body without those of nested functions and classes
    todo = list(statements)
    while todo:
        node = todo.pop()
        yield node
        if not isinstance(node, _SCOPES):
            todo.extend(ast.iter_child_nodes(node))


def compile_routine(body, filename, params, out, is_procedure):
    key = (body, filename, params, out, is_procedure)
    code = _codes.get(key)
    if code is not None:
        return code

    names = []
    for param in params:
        name = unicodedata.normalize('NFKC', param)
        if not name.isidentifier() or keyword.iskeyword(name):
            raise _error(filename,
                         "the parameter name '%s' is not a Python name" % param)
        names.append(name)

    tree = ast.parse(textwrap.dedent(body), filename)
    for node in _own_nodes(tree.body):
        if isinstance(node, (ast.Yield, ast.YieldFrom)):
            raise _error(filename, "'yield' in the body of a routine", node)
        if (is_procedure and isinstance(node, ast.Return)
                and node.value is not None
                and not (isinstance(node.value, ast.Constant)
                         and node.value.value is None)):
            raise _error(filename, "'return' with a value in a procedure",
                         node)

    statements = tree.body or [ast.Pass()]
    if is_procedure:
        out_names = [name for name, is_out in zip(names, out) if is_out]
        copy_out = ast.Assign(
            targets=[ast.Subscript(value=ast.Name(id=_OUT, ctx=ast.Load()),
                                   slice=ast.Slice(), ctx=ast.Store())],
            value=ast.Tuple(elts=[ast.Name(id=name, ctx=ast.Load())
                                  for name in out_names], ctx=ast.Load()))
        statements = [ast.Try(body=statements, handlers=[], orelse=[],
                              finalbody=[copy_out])]
        names.append(_OUT)

    function = ast.FunctionDef(
        name=_FUNC,
        args=ast.arguments(posonlyargs=[],
                           args=[ast.arg(arg=name) for name in names],
                           vararg=None, kwonlyargs=[], kw_defaults=[],
                           kwarg=None, defaults=[]),
        body=statements, decorator_list=[], returns=None, type_params=[])
    module = ast.Module(body=[function], type_ignores=[])
    ast.fix_missing_locations(module)
    code = compile(module, filename, 'exec')
    _codes[key] = code
    return code


def make_function(code, name):
    # The namespace of one session for one routine
    namespace = {'__builtins__': builtins, '__name__': name}
    exec(code, namespace)
    return namespace[_FUNC]


def describe(error, filename):
    # (text, line of the body or 0)
    line = 0
    traceback = error.__traceback__
    while traceback is not None:
        if traceback.tb_frame.f_code.co_filename == filename:
            line = traceback.tb_lineno
        traceback = traceback.tb_next
    if isinstance(error, SyntaxError):
        message = error.msg
        if not line and error.filename == filename and error.lineno:
            line = error.lineno
    else:
        message = str(error)
    text = type(error).__name__
    if message:
        text = text + ': ' + str(message)
    return text, line
)PY";


static PyObject *helper_compile= nullptr;
static PyObject *helper_make= nullptr;
static PyObject *helper_describe= nullptr;
static PyObject *decimal_class= nullptr;

/*
  Counts the initialisations of the interpreter.  A compiled routine
  belongs to the interpreter that compiled it: a session can keep a
  routine in its cache while the daemon plugin is uninstalled and
  installed again, and the Python objects of that routine are then gone.
*/
static std::atomic<unsigned long> interpreter_epoch{0};


bool lang_routine_init()
{
  interpreter_epoch++;

  PyObject *globals= PyDict_New();
  if (!globals)
  {
    PyErr_Clear();
    return true;
  }
  PyDict_SetItem(globals, py_name.builtins, PyEval_GetBuiltins());

  PyObject *done= PyRun_String(helper_source, Py_file_input, globals,
                               globals);
  if (done)
  {
    Py_DECREF(done);
    /* Borrowed from the dictionary; the functions keep it alive */
    helper_compile=  PyDict_GetItemString(globals, "compile_routine");
    helper_make=     PyDict_GetItemString(globals, "make_function");
    helper_describe= PyDict_GetItemString(globals, "describe");
    decimal_class=   PyDict_GetItemString(globals, "Decimal");
    Py_XINCREF(helper_compile);
    Py_XINCREF(helper_make);
    Py_XINCREF(helper_describe);
    Py_XINCREF(decimal_class);
  }
  Py_DECREF(globals);

  if (!helper_compile || !helper_make || !helper_describe || !decimal_class)
  {
    PyErr_Clear();
    lang_routine_deinit();
    return true;
  }
  return false;
}


void lang_routine_deinit()
{
  Py_CLEAR(helper_compile);
  Py_CLEAR(helper_make);
  Py_CLEAR(helper_describe);
  Py_CLEAR(decimal_class);
}


namespace
{

class Gil
{
  PyGILState_STATE m_state;
public:
  Gil(): m_state(PyGILState_Ensure()) { }
  ~Gil() { PyGILState_Release(m_state); }
  Gil(const Gil &)= delete;
  Gil &operator=(const Gil &)= delete;
};


bool is_utf8(CHARSET_INFO *cs)
{
  return my_charset_same(cs, &my_charset_utf8mb4_bin) ||
         my_charset_same(cs, &my_charset_utf8mb3_bin);
}


/*
  The value of a parameter as a Python object.  GIL held.

  @return A new reference, or nullptr: then a Python exception is set, or
          an SQL error is reported, or memory is exhausted
*/
PyObject *argument(Field *field)
{
  if (field->is_null())
    Py_RETURN_NONE;

  StringBuffer<256> buffer;

  if (field->type_handler() == &type_handler_vector)
  {
    String *bytes= field->val_str(&buffer);
    if (!lang_python_use_numpy())
      return PyBytes_FromStringAndSize(bytes->ptr(), bytes->length());
    npy_intp dims[1]= { (npy_intp) (bytes->length() / sizeof(float)) };
    PyObject *array= PyArray_SimpleNew(1, dims, NPY_FLOAT32);
    if (array)
      memcpy(PyArray_DATA((PyArrayObject *) array), bytes->ptr(),
             (size_t) dims[0] * sizeof(float));
    return array;
  }

  switch (field->cmp_type()) {
  case INT_RESULT:
    if (field->is_unsigned())
      return PyLong_FromUnsignedLongLong((ulonglong) field->val_int());
    return PyLong_FromLongLong(field->val_int());
  case REAL_RESULT:
    return PyFloat_FromDouble(field->val_real());
  case DECIMAL_RESULT:
  {
    String *digits= field->val_str(&buffer);
    PyObject *text= PyUnicode_FromStringAndSize(digits->ptr(),
                                                digits->length());
    if (!text)
      return nullptr;
    PyObject *decimal= PyObject_CallOneArg(decimal_class, text);
    Py_DECREF(text);
    return decimal;
  }
  case TIME_RESULT:
  {
    String *text= field->val_str(&buffer);
    return PyUnicode_FromStringAndSize(text->ptr(), text->length());
  }
  case STRING_RESULT:
  {
    String *text= field->val_str(&buffer);
    if (text->charset() == &my_charset_bin)
      return PyBytes_FromStringAndSize(text->ptr(), text->length());
    if (is_utf8(text->charset()))
      return PyUnicode_DecodeUTF8(text->ptr(), text->length(), nullptr);
    String utf8;
    uint errors;
    if (utf8.copy(text->ptr(), text->length(), text->charset(),
                  &my_charset_utf8mb4_bin, &errors))
      return nullptr;
    return PyUnicode_DecodeUTF8(utf8.ptr(), utf8.length(), nullptr);
  }
  case ROW_RESULT:
    break;
  }
  my_error(ER_NOT_SUPPORTED_YET, MYF(0), "A parameter of this type in Python");
  return nullptr;
}


class Language_routine_python: public Language_routine
{
  char *m_name;                           /* schema.name, for messages */
  PyObject *m_filename;                   /* "<schema.name>" */
  PyObject *m_code;                       /* Shared by the sessions */
  PyObject *m_function;                   /* Of this session, with its
                                             namespace; made at first call */
  unsigned long m_epoch;                  /* Interpreter of the objects */
  uint m_param_count;
  uint m_out_count;
  bool m_is_procedure;
  /* Not Sql_alloc types: the object outlives the statement */
  std::vector<sp_variable::enum_mode> m_param_mode;
  std::vector<PyObject *> m_args;

  bool interpreter_alive() const
  {
    return Py_IsInitialized() && m_epoch == interpreter_epoch;
  }

  /*
    Report the pending Python exception as an SQL error and clear it.
    GIL held.
  */
  void report(uint sql_error)
  {
    PyObject *error= PyErr_GetRaisedException();
    PyObject *described= nullptr;
    const char *text= nullptr;
    long line= 0;

    if (error && m_filename)
      described= PyObject_CallFunctionObjArgs(helper_describe, error,
                                              m_filename, nullptr);
    if (described && PyTuple_Check(described) &&
        PyTuple_GET_SIZE(described) == 2)
    {
      text= PyUnicode_AsUTF8(PyTuple_GET_ITEM(described, 0));
      line= PyLong_AsLong(PyTuple_GET_ITEM(described, 1));
    }
    PyErr_Clear();
    if (!text)
      text= error ? Py_TYPE(error)->tp_name : "error";

    if (line > 0)
      my_printf_error(sql_error, "Python: %s: %s at line %d", MYF(0),
                      m_name, text, (int) line);
    else
      my_printf_error(sql_error, "Python: %s: %s", MYF(0), m_name, text);

    Py_XDECREF(described);
    Py_XDECREF(error);
  }

  /* After a function of this file returned nullptr.  GIL held. */
  void report_failure(THD *thd)
  {
    if (PyErr_Occurred())
      report(ER_UNKNOWN_ERROR);
    else if (!thd->is_error())
      my_error(ER_OUTOFMEMORY, MYF(0), 0);
  }

  bool no_sql_value(PyObject *value)
  {
    my_printf_error(ER_UNKNOWN_ERROR, "Python: %s: a value of type '%s' "
                    "has no SQL value", MYF(0), m_name,
                    Py_TYPE(value)->tp_name);
    return true;
  }

  bool store_text(THD *thd, PyObject *value, Field *field)
  {
    PyObject *text= PyObject_Str(value);
    Py_ssize_t length= 0;
    const char *utf8= text ? PyUnicode_AsUTF8AndSize(text, &length) : nullptr;
    if (!utf8)
    {
      Py_XDECREF(text);
      report(ER_UNKNOWN_ERROR);
      return true;
    }
    field->store(utf8, (size_t) length, &my_charset_utf8mb4_bin);
    Py_DECREF(text);
    return thd->is_error();
  }

  /*
    Store a Python object in a field of the server, which converts it to
    its type.  GIL held.

    @retval false  OK
    @retval true   Error, reported
  */
  bool store(THD *thd, PyObject *value, Field *field)
  {
    if (value == Py_None)
    {
      field->set_null();
      return false;
    }
    field->set_notnull();

    if (PyBool_Check(value))
    {
      field->store((longlong) (value == Py_True), false);
    }
    else if (PyLong_Check(value))
    {
      int overflow;
      longlong number= PyLong_AsLongLongAndOverflow(value, &overflow);
      if (!overflow)
        field->store(number, false);
      else
      {
        if (overflow > 0)
        {
          ulonglong big= PyLong_AsUnsignedLongLong(value);
          if (!PyErr_Occurred())
          {
            field->store((longlong) big, true);
            return thd->is_error();
          }
          PyErr_Clear();
        }
        /* Beyond 64 bits: the digits */
        return store_text(thd, value, field);
      }
    }
    else if (PyFloat_Check(value))
    {
      double number= PyFloat_AS_DOUBLE(value);
      if (!std::isfinite(number))
      {
        my_printf_error(ER_UNKNOWN_ERROR, "Python: %s: the value is %s, "
                        "which has no SQL value", MYF(0), m_name,
                        std::isnan(number) ? "NaN" : "an infinity");
        return true;
      }
      field->store(number);
    }
    else if (PyUnicode_Check(value))
    {
      Py_ssize_t length;
      const char *utf8= PyUnicode_AsUTF8AndSize(value, &length);
      if (!utf8)
      {
        report(ER_UNKNOWN_ERROR);
        return true;
      }
      field->store(utf8, (size_t) length, &my_charset_utf8mb4_bin);
    }
    else if (PyObject_CheckBuffer(value))
    {
      /* bytes, bytearray, memoryview, numpy.ndarray */
      Py_buffer view;
      if (PyObject_GetBuffer(value, &view, PyBUF_SIMPLE))
      {
        report(ER_UNKNOWN_ERROR);
        return true;
      }
      field->store((const char *) view.buf, (size_t) view.len,
                   &my_charset_bin);
      PyBuffer_Release(&view);
    }
    else if (PyDict_Check(value) || PyList_Check(value) ||
             PyTuple_Check(value) || PyAnySet_Check(value) ||
             PyCallable_Check(value) || PyGen_Check(value) ||
             PyCoro_CheckExact(value) || PyAsyncGen_CheckExact(value))
    {
      return no_sql_value(value);
    }
    else
    {
      /* decimal.Decimal, the classes of datetime, anything with a text */
      return store_text(thd, value, field);
    }
    return thd->is_error();
  }

  /*
    Call the function of the session.  GIL held.

    @param out  Procedure: the list that receives the OUT and INOUT
                parameters.  Function: nullptr
    @return The result (new reference), or nullptr after a reported error
  */
  PyObject *run(THD *thd, const Language_call &call, PyObject *out)
  {
    DBUG_ASSERT(call.params->argument_count() == m_param_count);

    if (!m_function)
    {
      PyObject *name= PyUnicode_FromString(m_name);
      if (name)
      {
        m_function= PyObject_CallFunctionObjArgs(helper_make, m_code, name,
                                                 nullptr);
        Py_DECREF(name);
      }
      if (!m_function)
      {
        report_failure(thd);
        return nullptr;
      }
    }

    uint count= 0;
    for (; count < m_param_count; count++)
    {
      if (m_param_mode[count] == sp_variable::MODE_OUT)
      {
        /* An OUT parameter starts as None, whatever the caller passed */
        m_args[count]= Py_NewRef(Py_None);
        continue;
      }
      if (!(m_args[count]= argument(call.params->get_parameter(count)->field)))
        break;
    }

    PyObject *result= nullptr;
    if (count < m_param_count)
      report_failure(thd);
    else
    {
      size_t nargs= m_param_count;
      if (out)
        m_args[nargs++]= out;             /* Borrowed */
      result= PyObject_Vectorcall(m_function, m_args.data(), nargs, nullptr);
      m_args[m_param_count]= nullptr;
      if (!result)
        report(ER_UNKNOWN_ERROR);
    }

    for (uint i= 0; i < count; i++)
      Py_CLEAR(m_args[i]);
    return result;
  }

  bool interpreter_gone()
  {
    if (interpreter_alive())
      return false;
    my_printf_error(ER_UNKNOWN_ERROR, "Python: %s: the interpreter that "
                    "compiled the routine was shut down; use a new "
                    "connection", MYF(0), m_name);
    return true;
  }

public:
  Language_routine_python():
    m_name(nullptr), m_filename(nullptr), m_code(nullptr),
    m_function(nullptr), m_epoch(0), m_param_count(0), m_out_count(0),
    m_is_procedure(false)
  { }

  ~Language_routine_python() override
  {
    /* Objects of an interpreter that is gone are gone with it */
    if ((m_filename || m_code || m_function) && interpreter_alive())
    {
      Gil gil;
      Py_CLEAR(m_function);
      Py_CLEAR(m_code);
      Py_CLEAR(m_filename);
    }
    my_free(m_name);
  }

  bool init(THD *thd, const Language_routine_def &def)
  {
    if (def.aggregate)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "An aggregate function in the language PYTHON");
      return true;
    }
    m_is_procedure= !def.returns;
    if (def.returns && def.returns->type_handler()->cmp_type() == ROW_RESULT)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "A return value of this type in Python");
      return true;
    }

    size_t name_length= def.db.length + 1 + def.name.length;
    if (!(m_name= (char *) my_malloc(PSI_NOT_INSTRUMENTED, name_length + 1,
                                     MYF(MY_WME))))
      return true;
    my_snprintf(m_name, name_length + 1, "%s.%s", def.db.str, def.name.str);

    m_param_count= def.params->context_var_count();
    m_param_mode.resize(m_param_count);
    m_args.assign(m_param_count + 1, nullptr);
    for (uint i= 0; i < m_param_count; i++)
    {
      const sp_variable *param= def.params->get_context_variable(i);
      if (param->type_handler()->cmp_type() == ROW_RESULT)
      {
        my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                 "A parameter of this type in Python");
        return true;
      }
      m_param_mode[i]= param->mode;
      if (param->mode != sp_variable::MODE_IN)
        m_out_count++;
    }

    /* The interpreter belongs to the daemon plugin of the library */
    if (!Py_IsInitialized() || !helper_compile)
    {
      my_error(ER_PLUGIN_IS_NOT_LOADED, MYF(0), "lang_python");
      return true;
    }

    /* The body is in the character set of the client that created it */
    String converted;
    const char *body= def.body.str;
    size_t body_length= def.body.length;
    if (!is_utf8(def.body_charset) && def.body_charset != &my_charset_bin)
    {
      uint errors;
      if (converted.copy(def.body.str, def.body.length, def.body_charset,
                         &my_charset_utf8mb4_bin, &errors))
        return true;
      body= converted.ptr();
      body_length= converted.length();
    }

    Gil gil;
    m_epoch= interpreter_epoch;

    PyObject *source= nullptr, *params= nullptr, *out= nullptr;
    bool failed= true;

    if (!(m_filename= PyUnicode_FromFormat("<%s>", m_name)) ||
        !(source= PyUnicode_DecodeUTF8(body, (Py_ssize_t) body_length,
                                       nullptr)) ||
        !(params= PyTuple_New(m_param_count)) ||
        !(out= PyTuple_New(m_param_count)))
      goto end;

    for (uint i= 0; i < m_param_count; i++)
    {
      /* The name of a parameter is in the character set of identifiers */
      const sp_variable *param= def.params->get_context_variable(i);
      PyObject *name= PyUnicode_DecodeUTF8(param->name.str,
                                           (Py_ssize_t) param->name.length,
                                           nullptr);
      if (!name)
        goto end;
      PyTuple_SET_ITEM(params, i, name);
      PyTuple_SET_ITEM(out, i,
                       PyBool_FromLong(param->mode != sp_variable::MODE_IN));
    }

    m_code= PyObject_CallFunctionObjArgs(helper_compile, source, m_filename,
                                         params, out,
                                         m_is_procedure ? Py_True : Py_False,
                                         nullptr);
    failed= !m_code;

end:
    if (failed)
      report(ER_PARSE_ERROR);
    Py_XDECREF(source);
    Py_XDECREF(params);
    Py_XDECREF(out);
    return failed;
  }

  bool call_function(THD *thd, const Language_call &call) override
  {
    if (interpreter_gone())
      return true;
    Gil gil;
    PyObject *result= run(thd, call, nullptr);
    if (!result)
      return true;
    bool failed= store(thd, result, call.return_value);
    Py_DECREF(result);
    return failed;
  }

  bool call_procedure(THD *thd, const Language_call &call) override
  {
    if (interpreter_gone())
      return true;
    Gil gil;
    PyObject *out= PyList_New(0);
    if (!out)
    {
      report_failure(thd);
      return true;
    }
    PyObject *result= run(thd, call, out);
    bool failed= !result;
    Py_XDECREF(result);                   /* What the body returns: unused */

    if (!failed && PyList_GET_SIZE(out) != (Py_ssize_t) m_out_count)
    {
      my_printf_error(ER_UNKNOWN_ERROR, "Python: %s: the OUT parameters "
                      "were not returned", MYF(0), m_name);
      failed= true;
    }
    if (!failed)
    {
      /*
        The values go to the fields of the parameters; the server passes
        them on to the caller.  They are stored with the rules of an
        assignment in a routine, as the result of a function is.
      */
      Sp_eval_expr_state state(thd);
      for (uint i= 0, n= 0; i < m_param_count && !failed; i++)
      {
        if (m_param_mode[i] == sp_variable::MODE_IN)
          continue;
        failed= store(thd, PyList_GET_ITEM(out, n++),
                      call.params->get_parameter(i)->field);
      }
    }
    Py_DECREF(out);
    return failed;
  }
};


class Language_handler_python: public Language_handler
{
public:
  Language_routine *compile(THD *thd, const Language_routine_def &def) override
  {
    Language_routine_python *routine=
      new (std::nothrow) Language_routine_python();
    if (!routine)
    {
      my_error(ER_OUTOFMEMORY, MYF(0), (int) sizeof(Language_routine_python));
      return nullptr;
    }
    if (routine->init(thd, def))
    {
      delete routine;
      return nullptr;
    }
    return routine;
  }
};

Language_handler_python language_handler_python;

} // namespace


struct st_mariadb_language lang_python_language=
{
  MariaDB_LANGUAGE_INTERFACE_VERSION,
  &language_handler_python
};
