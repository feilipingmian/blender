/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup pythonintern
 *
 * This file defines `_bpy.ops`, an internal python module which gives Python
 * the ability to inspect and call operators (defined by C or Python).
 *
 * \note
 * This C module is private, it should only be used by `scripts/modules/bpy/ops.py` which
 * exposes operators as dynamically defined modules & callable objects to access all operators.
 */

#include <Python.h>

#include "RNA_types.h"

#include "BLI_listbase.h"
#include "BLI_utildefines.h"

#include "../generic/py_capi_rna.h"
#include "../generic/py_capi_utils.h"
#include "../generic/python_utildefines.h"
#include "BPY_extern.h"
#include "bpy_capi_utils.h"
#include "bpy_operator.h"
#include "bpy_operator_wrap.h"
#include "bpy_rna.h" /* for setting argument properties & type method `get_rna_type`. */

#include "RNA_access.h"
#include "RNA_enum_types.h"
#include "RNA_prototypes.h"

#include "WM_api.h"
#include "WM_types.h"

#include "MEM_guardedalloc.h"

#include "BLI_ghash.h"

#include "BKE_context.h"
#include "BKE_report.h"

/* so operators called can spawn threads which acquire the GIL */
#define BPY_RELEASE_GIL

static wmOperatorType *ot_lookup_from_py_string(PyObject *value, const char *py_fn_id)
{
  const char *opname = PyUnicode_AsUTF8(value);
  if (opname == NULL) {
    PyErr_Format(PyExc_TypeError, "%s() expects a string argument", py_fn_id);
    return NULL;
  }

  wmOperatorType *ot = WM_operatortype_find(opname, true);
  if (ot == NULL) {
    PyErr_Format(PyExc_KeyError, "%s(\"%s\") not found", py_fn_id, opname);
    return NULL;
  }
  return ot;
}

static PyObject *pyop_poll(PyObject *UNUSED(self), PyObject *args)
{
  wmOperatorType *ot;
  const char *opname;
  const char *context_str = NULL;
  PyObject *ret;

  wmOperatorCallContext context = WM_OP_EXEC_DEFAULT;

  /* XXX TODO: work out a better solution for passing on context,
   * could make a tuple from self and pack the name and Context into it. */
  bContext *C = BPY_context_get();

  if (C == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "Context is None, can't poll any operators");
    return NULL;
  }

  /* All arguments are positional. */
  static const char *_keywords[] = {"", "", NULL};
  static _PyArg_Parser _parser = {
      "s" /* `opname` */
      "|" /* Optional arguments. */
      "s" /* `context_str` */
      ":_bpy.ops.poll",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args, NULL, &_parser, &opname, &context_str)) {
    return NULL;
  }

  ot = WM_operatortype_find(opname, true);

  if (ot == NULL) {
    PyErr_Format(PyExc_AttributeError,
                 "Polling operator \"bpy.ops.%s\" error, "
                 "could not be found",
                 opname);
    return NULL;
  }

  if (context_str) {
    int context_int = context;

    if (RNA_enum_value_from_id(rna_enum_operator_context_items, context_str, &context_int) == 0) {
      char *enum_str = pyrna_enum_repr(rna_enum_operator_context_items);
      PyErr_Format(PyExc_TypeError,
                   "Calling operator \"bpy.ops.%s.poll\" error, "
                   "expected a string enum in (%s)",
                   opname,
                   enum_str);
      MEM_freeN(enum_str);
      return NULL;
    }
    /* Copy back to the properly typed enum. */
    context = context_int;
  }

  /* main purpose of this function */
  ret = WM_operator_poll_context((bContext *)C, ot, context) ? Py_True : Py_False;

  return Py_INCREF_RET(ret);
}

static PyObject *pyop_call(PyObject *UNUSED(self), PyObject *args)
{
  wmOperatorType *ot;
  int error_val = 0;
  PointerRNA ptr;
  int operator_ret = OPERATOR_CANCELLED;

  const char *opname;
  const char *context_str = NULL;
  PyObject *kw = NULL; /* optional args */

  wmOperatorCallContext context = WM_OP_EXEC_DEFAULT;
  int is_undo = false;

  /* XXX TODO: work out a better solution for passing on context,
   * could make a tuple from self and pack the name and Context into it. */
  bContext *C = BPY_context_get();

  if (C == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "Context is None, can't poll any operators");
    return NULL;
  }

  /* All arguments are positional. */
  static const char *_keywords[] = {"", "", "", "", NULL};
  static _PyArg_Parser _parser = {
      "s"  /* `opname` */
      "|"  /* Optional arguments. */
      "O!" /* `kw` */
      "s"  /* `context_str` */
      "i"  /* `is_undo` */
      ":_bpy.ops.call",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(
          args, NULL, &_parser, &opname, &PyDict_Type, &kw, &context_str, &is_undo))
  {
    return NULL;
  }

  ot = WM_operatortype_find(opname, true);

  if (ot == NULL) {
    PyErr_Format(PyExc_AttributeError,
                 "Calling operator \"bpy.ops.%s\" error, "
                 "could not be found",
                 opname);
    return NULL;
  }

  if (!pyrna_write_check()) {
    PyErr_Format(PyExc_RuntimeError,
                 "Calling operator \"bpy.ops.%s\" error, "
                 "can't modify blend data in this state (drawing/rendering)",
                 opname);
    return NULL;
  }

  if (context_str) {
    int context_int = context;

    if (RNA_enum_value_from_id(rna_enum_operator_context_items, context_str, &context_int) == 0) {
      char *enum_str = pyrna_enum_repr(rna_enum_operator_context_items);
      PyErr_Format(PyExc_TypeError,
                   "Calling operator \"bpy.ops.%s\" error, "
                   "expected a string enum in (%s)",
                   opname,
                   enum_str);
      MEM_freeN(enum_str);
      return NULL;
    }
    /* Copy back to the properly typed enum. */
    context = context_int;
  }

  if (WM_operator_poll_context((bContext *)C, ot, context) == false) {
    bool msg_free = false;
    const char *msg = CTX_wm_operator_poll_msg_get(C, &msg_free);
    PyErr_Format(PyExc_RuntimeError,
                 "Operator bpy.ops.%.200s.poll() %.200s",
                 opname,
                 msg ? msg : "failed, context is incorrect");
    CTX_wm_operator_poll_msg_clear(C);
    if (msg_free) {
      MEM_freeN((void *)msg);
    }
    error_val = -1;
  }
  else {
    WM_operator_properties_create_ptr(&ptr, ot);
    WM_operator_properties_sanitize(&ptr, 0);

    if (kw && PyDict_Size(kw)) {
      error_val = pyrna_pydict_to_props(
          &ptr, kw, false, "Converting py args to operator properties: ");
    }

    if (error_val == 0) {
      ReportList *reports;

      reports = MEM_mallocN(sizeof(ReportList), "wmOperatorReportList");

      /* Own so these don't move into global reports. */
      BKE_reports_init(reports, RPT_STORE | RPT_OP_HOLD | RPT_PRINT_HANDLED_BY_OWNER);

#ifdef BPY_RELEASE_GIL
      /* release GIL, since a thread could be started from an operator
       * that updates a driver */
      /* NOTE: I have not seen any examples of code that does this
       * so it may not be officially supported but seems to work ok. */
      {
        PyThreadState *ts = PyEval_SaveThread();
#endif

        operator_ret = WM_operator_call_py(C, ot, context, &ptr, reports, is_undo);

#ifdef BPY_RELEASE_GIL
        /* regain GIL */
        PyEval_RestoreThread(ts);
      }
#endif

      error_val = BPy_reports_to_error(reports, PyExc_RuntimeError, false);

      /* operator output is nice to have in the terminal/console too */
      if (!BLI_listbase_is_empty(&reports->list)) {
        BPy_reports_write_stdout(reports, NULL);
      }

      BKE_reports_clear(reports);
      if ((reports->flag & RPT_FREE) == 0) {
        MEM_freeN(reports);
      }
      else {
        /* The WM is now responsible for running the modal operator,
         * show reports in the info window. */
        reports->flag &= ~RPT_OP_HOLD;
      }
    }

    WM_operator_properties_free(&ptr);

#if 0
    /* if there is some way to know an operator takes args we should use this */
    {
      /* no props */
      if (kw != NULL) {
        PyErr_Format(PyExc_AttributeError, "Operator \"%s\" does not take any args", opname);
        return NULL;
      }

      WM_operator_name_call(C, opname, WM_OP_EXEC_DEFAULT, NULL, NULL);
    }
#endif
  }

  if (error_val == -1) {
    return NULL;
  }

  /* When calling `bpy.ops.wm.read_factory_settings()` `bpy.data's` main pointer
   * is freed by clear_globals(), further access will crash blender.
   * Setting context is not needed in this case, only calling because this
   * function corrects bpy.data (internal Main pointer) */
  BPY_modules_update();

  /* return operator_ret as a bpy enum */
  return pyrna_enum_bitfield_as_set(rna_enum_operator_return_items, operator_ret);
}

static PyObject *pyop_as_string(PyObject *UNUSED(self), PyObject *args)
{
  wmOperatorType *ot;
  PointerRNA ptr;

  const char *opname;
  PyObject *kw = NULL; /* optional args */
  bool all_args = true;
  bool macro_args = true;
  int error_val = 0;

  char *buf = NULL;
  PyObject *pybuf;

  bContext *C = BPY_context_get();

  if (C == NULL) {
    PyErr_SetString(PyExc_RuntimeError,
                    "Context is None, can't get the string representation of this object.");
    return NULL;
  }

  /* All arguments are positional. */
  static const char *_keywords[] = {"", "", "", "", NULL};
  static _PyArg_Parser _parser = {
      "s"  /* `opname` */
      "|"  /* Optional arguments. */
      "O!" /* `kw` */
      "O&" /* `all_args` */
      "O&" /* `macro_args` */
      ":_bpy.ops.as_string",
      _keywords,
      0,
  };
  if (!_PyArg_ParseTupleAndKeywordsFast(args,
                                        NULL,
                                        &_parser,
                                        &opname,
                                        &PyDict_Type,
                                        &kw,
                                        PyC_ParseBool,
                                        &all_args,
                                        PyC_ParseBool,
                                        &macro_args))
  {
    return NULL;
  }

  ot = WM_operatortype_find(opname, true);

  if (ot == NULL) {
    PyErr_Format(PyExc_AttributeError,
                 "_bpy.ops.as_string: operator \"%.200s\" "
                 "could not be found",
                 opname);
    return NULL;
  }

  // WM_operator_properties_create(&ptr, opname);
  /* Save another lookup */
  RNA_pointer_create(NULL, ot->srna, NULL, &ptr);

  if (kw && PyDict_Size(kw)) {
    error_val = pyrna_pydict_to_props(
        &ptr, kw, false, "Converting py args to operator properties: ");
  }

  if (error_val == 0) {
    buf = WM_operator_pystring_ex(C, NULL, all_args, macro_args, ot, &ptr);
  }

  WM_operator_properties_free(&ptr);

  if (error_val == -1) {
    return NULL;
  }

  if (buf) {
    pybuf = PyUnicode_FromString(buf);
    MEM_freeN(buf);
  }
  else {
    pybuf = PyUnicode_FromString("");
  }

  return pybuf;
}

static PyObject *pyop_dir(PyObject *UNUSED(self))
{
  GHashIterator iter;
  PyObject *list;
  int i;

  WM_operatortype_iter(&iter);
  list = PyList_New(BLI_ghash_len(iter.gh));

  for (i = 0; !BLI_ghashIterator_done(&iter); BLI_ghashIterator_step(&iter), i++) {
    wmOperatorType *ot = BLI_ghashIterator_getValue(&iter);
    PyList_SET_ITEM(list, i, PyUnicode_FromString(ot->idname));
  }

  return list;
}

static PyObject *pyop_getrna_type(PyObject *UNUSED(self), PyObject *value)
{
  wmOperatorType *ot;
  if ((ot = ot_lookup_from_py_string(value, "get_rna_type")) == NULL) {
    return NULL;
  }

  PointerRNA ptr;
  RNA_pointer_create(NULL, &RNA_Struct, ot->srna, &ptr);
  BPy_StructRNA *pyrna = (BPy_StructRNA *)pyrna_struct_CreatePyObject(&ptr);
  return (PyObject *)pyrna;
}

static PyObject *pyop_get_bl_options(PyObject *UNUSED(self), PyObject *value)
{
  wmOperatorType *ot;
  if ((ot = ot_lookup_from_py_string(value, "get_bl_options")) == NULL) {
    return NULL;
  }
  return pyrna_enum_bitfield_as_set(rna_enum_operator_type_flag_items, ot->flag);
}

static struct PyMethodDef bpy_ops_methods[] = {
    {"poll", (PyCFunction)pyop_poll, METH_VARARGS, NULL},
    {"call", (PyCFunction)pyop_call, METH_VARARGS, NULL},
    {"as_string", (PyCFunction)pyop_as_string, METH_VARARGS, NULL},
    {"dir", (PyCFunction)pyop_dir, METH_NOARGS, NULL},
    {"get_rna_type", (PyCFunction)pyop_getrna_type, METH_O, NULL},
    {"get_bl_options", (PyCFunction)pyop_get_bl_options, METH_O, NULL},
    {"macro_define", (PyCFunction)PYOP_wrap_macro_define, METH_VARARGS, NULL},
    {NULL, NULL, 0, NULL},
};

static struct PyModuleDef bpy_ops_module = {
    PyModuleDef_HEAD_INIT,
    /*m_name*/ "_bpy.ops",
    /*m_doc*/ NULL,
    /*m_size*/ -1, /* multiple "initialization" just copies the module dict. */
    /*m_methods*/ bpy_ops_methods,
    /*m_slots*/ NULL,
    /*m_traverse*/ NULL,
    /*m_clear*/ NULL,
    /*m_free*/ NULL,
};

PyObject *BPY_operator_module(void)
{
  PyObject *submodule;

  submodule = PyModule_Create(&bpy_ops_module);

  return submodule;
}
