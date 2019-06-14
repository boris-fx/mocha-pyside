/****************************************************************************
**
** Copyright (C) 2018 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt for Python.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "basewrapper.h"
#include "autodecref.h"

extern "C"
{

/*
 * The documentation is located in file signature_doc.rst
 */
#include "signature.h"
#include <structmember.h>

// These constants were needed in former versions of the module:
#define PYTHON_HAS_QUALNAME             (PY_VERSION_HEX >= 0x03030000)
#define PYTHON_HAS_UNICODE              (PY_VERSION_HEX >= 0x03000000)
#define PYTHON_HAS_WEAKREF_PYCFUNCTION  (PY_VERSION_HEX >= 0x030500A0)
#define PYTHON_IS_PYTHON3               (PY_VERSION_HEX >= 0x03000000)
#define PYTHON_HAS_KEYWORDONLY          (PYTHON_IS_PYTHON3)
#define PYTHON_USES_PERCENT_V_FORMAT    (PYTHON_IS_PYTHON3)
#define PYTHON_HAS_DESCR_REDUCE         (PY_VERSION_HEX >= 0x03040000)
#define PYTHON_HAS_METH_REDUCE          (PYTHON_HAS_DESCR_REDUCE)
#define PYTHON_NEEDS_ITERATOR_FLAG      (!PYTHON_IS_PYTHON3)
#define PYTHON_EXPOSES_METHODDESCR      (PYTHON_IS_PYTHON3)
#define PYTHON_NO_TYPE_IN_FUNCTIONS     (!PYTHON_IS_PYTHON3 || Py_LIMITED_API)
#define PYTHON_HAS_INT_AND_LONG         (!PYTHON_IS_PYTHON3)

// These constants are still in use:
#define PYTHON_USES_D_COMMON            (PY_VERSION_HEX >= 0x03020000)

typedef struct safe_globals_struc {
    // init part 1: get arg_dict
    PyObject *helper_module;
    PyObject *arg_dict;
    PyObject *map_dict;
    // init part 2: run module
    PyObject *pyside_type_init_func;
    PyObject *create_signature_func;
    PyObject *seterror_argument_func;
    PyObject *make_helptext_func;
} safe_globals_struc, *safe_globals;

static safe_globals pyside_globals = 0;

static PyObject *GetTypeKey(PyObject *ob);

static PyObject *GetSignature_Function(PyObject *, const char *);
static PyObject *GetSignature_TypeMod(PyObject *, const char *);
static PyObject *GetSignature_Wrapper(PyObject *, const char *);
static PyObject *get_signature(PyObject *self, PyObject *args);

static PyObject *PySide_BuildSignatureProps(PyObject *class_mod);

static void init_module_1(void);
static void init_module_2(void);

static PyObject *
CreateSignature(PyObject *props, PyObject *key)
{
    /*
     * Here is the new function to create all signatures. It simply calls
     * into Python and creates a signature object for a dummy-function.
     * This is so much simpler than using all the attributes explicitly
     * to support '_signature_is_functionlike()'.
     */
    return PyObject_CallFunction(pyside_globals->create_signature_func,
                                 const_cast<char *>("(OO)"), props, key);
}

static PyObject *
pyside_cf_get___signature__(PyObject *func, const char *modifier)
{
    init_module_2();
    return GetSignature_Function(func, modifier);
}

static PyObject *
pyside_sm_get___signature__(PyObject *sm, const char *modifier)
{
    init_module_2();
    Shiboken::AutoDecRef func(PyObject_GetAttrString(sm, "__func__"));
    if (Py_TYPE(func) == PepFunction_TypePtr)
        Py_RETURN_NONE;
    return GetSignature_Function(func, modifier);
}

static PyObject *
_get_class_of_cf(PyObject *ob_cf)
{
    PyObject *selftype = PyCFunction_GET_SELF(ob_cf);
    if (selftype == nullptr) {
        selftype = PyDict_GetItem(pyside_globals->map_dict, ob_cf);
        if (selftype == nullptr) {
            // This must be an overloaded function that we handled special.
            Shiboken::AutoDecRef special(Py_BuildValue("(Os)", ob_cf, "overload"));
            selftype = PyDict_GetItem(pyside_globals->map_dict, special);
            if (selftype == nullptr) {
                // This is probably a module function. We will return type(None).
                selftype = Py_None;
            }
        }
    }

    PyObject *obtype_mod = (PyType_Check(selftype) || PyModule_Check(selftype))
                           ? selftype : reinterpret_cast<PyObject *>(Py_TYPE(selftype));
    Py_INCREF(obtype_mod);
    return obtype_mod;
}

static PyObject *
_get_class_of_sm(PyObject *ob_sm)
{
    Shiboken::AutoDecRef func(PyObject_GetAttrString(ob_sm, "__func__"));
    return _get_class_of_cf(func);
}

static PyObject *
_get_class_of_descr(PyObject *ob)
{
    Shiboken::AutoDecRef func_name(PyObject_GetAttrString(ob, "__name__"));
    return PyObject_GetAttrString(ob, "__objclass__");
}

static PyObject *
GetClassOfFunc(PyObject *ob)
{
    if (PyType_Check(ob)) {
        // PySide-928: The type case must do refcounting like the others as well.
        Py_INCREF(ob);
        return ob;
    }
    if (PyType_IsSubtype(Py_TYPE(ob), &PyCFunction_Type))
        return _get_class_of_cf(ob);
    if (Py_TYPE(ob) == PepStaticMethod_TypePtr)
        return _get_class_of_sm(ob);
    if (Py_TYPE(ob) == PepMethodDescr_TypePtr)
        return _get_class_of_descr(ob);
    if (Py_TYPE(ob) == &PyWrapperDescr_Type)
        return _get_class_of_descr(ob);
    Py_FatalError("unexpected type in GetClassOfFunc");
    return nullptr;
}

static PyObject *
get_funcname(PyObject *ob)
{
    PyObject *func = ob;
    if (Py_TYPE(ob) == PepStaticMethod_TypePtr)
        func = PyObject_GetAttrString(ob, "__func__");
    else
        Py_INCREF(func);
    PyObject *func_name = PyObject_GetAttrString(func, "__name__");
    Py_DECREF(func);
    if (func_name == nullptr)
        Py_FatalError("unexpected name problem in compute_name_key");
    return func_name;
}

static PyObject *
compute_name_key(PyObject *ob)
{
    if (PyType_Check(ob))
        return GetTypeKey(ob);
    Shiboken::AutoDecRef func_name(get_funcname(ob));
    Shiboken::AutoDecRef type_key(GetTypeKey(GetClassOfFunc(ob)));
    return Py_BuildValue("(OO)", type_key.object(), func_name.object());
}

static int
build_name_key_to_func(PyObject *obtype)
{
    PyTypeObject *type = reinterpret_cast<PyTypeObject *>(obtype);
    PyMethodDef *meth = type->tp_methods;

    if (meth == 0)
        return 0;

    Shiboken::AutoDecRef type_key(GetTypeKey(obtype));
    for (; meth->ml_name != nullptr; meth++) {
        Shiboken::AutoDecRef func(PyCFunction_NewEx(meth, obtype, nullptr));
        Shiboken::AutoDecRef func_name(get_funcname(func));
        Shiboken::AutoDecRef name_key(Py_BuildValue("(OO)", type_key.object(), func_name.object()));
        if (func.isNull() || name_key.isNull()
            || PyDict_SetItem(pyside_globals->map_dict, name_key, func) < 0)
            return -1;
    }
    return 0;
}

static PyObject *
name_key_to_func(PyObject *ob)
{
    /*
     * We build a mapping from name_key to function.
     * This could also be computed directly, but the Limited API
     * makes this impossible. So we always build our own mapping.
     */
    Shiboken::AutoDecRef name_key(compute_name_key(ob));
    if (name_key.isNull())
        Py_RETURN_NONE;

    PyObject *ret = PyDict_GetItem(pyside_globals->map_dict, name_key);
    if (ret == nullptr) {
        // do a lazy initialization
        Shiboken::AutoDecRef type_key(GetTypeKey(GetClassOfFunc(ob)));
        PyObject *type = PyDict_GetItem(pyside_globals->map_dict,
                                        type_key);
        if (type == nullptr)
            Py_RETURN_NONE;
        assert(PyType_Check(type));
        if (build_name_key_to_func(type) < 0)
            return nullptr;
        ret = PyDict_GetItem(pyside_globals->map_dict, name_key);
    }
    Py_XINCREF(ret);
    return ret;
}

static PyObject *
pyside_md_get___signature__(PyObject *ob_md, const char *modifier)
{
    init_module_2();
    Shiboken::AutoDecRef func(name_key_to_func(ob_md));
    if (func.object() == Py_None)
        return Py_None;
    if (func.isNull())
        Py_FatalError("missing mapping in MethodDescriptor");
    return pyside_cf_get___signature__(func, modifier);
}

static PyObject *
pyside_wd_get___signature__(PyObject *ob, const char *modifier)
{
    init_module_2();
    return GetSignature_Wrapper(ob, modifier);
}

static PyObject *
pyside_tp_get___signature__(PyObject *obtype_mod, const char *modifier)
{
    init_module_2();
    return GetSignature_TypeMod(obtype_mod, modifier);
}

// forward
static PyObject *
GetSignature_Cached(PyObject *props, const char *sig_kind, const char *modifier);

static PyObject *
GetTypeKey(PyObject *ob)
{
    assert(PyType_Check(ob) || PyModule_Check(ob));
    /*
     * We obtain a unique key using the module name and the type name.
     *
     * The type name is a bit funny when modules are nested.
     * Example:
     *
     * "sample.Photon.ValueIdentity" is a class.
     *   name:   "ValueIdentity"
     *   module: "sample.Photon"
     *
     * This is the PyCFunction behavior, as opposed to Python functions.
     */
    Shiboken::AutoDecRef class_name(PyObject_GetAttrString(ob, "__name__"));
    Shiboken::AutoDecRef module_name(PyObject_GetAttrString(ob, "__module__"));

    if (module_name.isNull())
        PyErr_Clear();

    // Note: if we have a module, then __module__ is null, and we get
    // the module name through __name__ .
    if (class_name.isNull())
        return nullptr;
    if (module_name.object())
        return Py_BuildValue("(OO)", module_name.object(), class_name.object());
    return Py_BuildValue("O", class_name.object());
}

static PyObject *empty_dict = nullptr;

static PyObject *
TypeKey_to_PropsDict(PyObject *type_key, PyObject *obtype)
{
    PyObject *dict = PyDict_GetItem(pyside_globals->arg_dict, type_key);
    if (dict == nullptr) {
        if (empty_dict == nullptr)
            empty_dict = PyDict_New();
        dict = empty_dict;
    }
    if (!PyDict_Check(dict))
        dict = PySide_BuildSignatureProps(type_key);
    return dict;
}

static PyObject *
GetSignature_Function(PyObject *obfunc, const char *modifier)
{
    // make sure that we look into PyCFunction, only...
    if (Py_TYPE(obfunc) == PepFunction_TypePtr)
        Py_RETURN_NONE;
    Shiboken::AutoDecRef obtype_mod(GetClassOfFunc(obfunc));
    Shiboken::AutoDecRef type_key(GetTypeKey(obtype_mod));
    if (type_key.isNull())
        Py_RETURN_NONE;
    PyObject *dict = TypeKey_to_PropsDict(type_key, obtype_mod);
    if (dict == nullptr)
        return nullptr;
    Shiboken::AutoDecRef func_name(PyObject_GetAttrString(obfunc, "__name__"));
    PyObject *props = !func_name.isNull() ? PyDict_GetItem(dict, func_name) : nullptr;
    if (props == nullptr)
        Py_RETURN_NONE;

    int flags = PyCFunction_GET_FLAGS(obfunc);
    const char *sig_kind;
    if (PyModule_Check(obtype_mod))
        sig_kind = "function";
    else if (flags & METH_CLASS)
        sig_kind = "classmethod";
    else if (flags & METH_STATIC)
        sig_kind = "staticmethod";
    else
        sig_kind = "method";
    return GetSignature_Cached(props, sig_kind, modifier);
}

static PyObject *
GetSignature_Wrapper(PyObject *ob, const char *modifier)
{
    Shiboken::AutoDecRef func_name(PyObject_GetAttrString(ob, "__name__"));
    Shiboken::AutoDecRef objclass(PyObject_GetAttrString(ob, "__objclass__"));
    Shiboken::AutoDecRef class_key(GetTypeKey(objclass));

    if (func_name.isNull() || objclass.isNull() || class_key.isNull())
        return nullptr;
    PyObject *dict = TypeKey_to_PropsDict(class_key, objclass);
    if (dict == nullptr)
        return nullptr;
    PyObject *props = PyDict_GetItem(dict, func_name);
    if (props == nullptr)
        Py_RETURN_NONE;
    return GetSignature_Cached(props, "method", modifier);
}

static PyObject *
GetSignature_TypeMod(PyObject *ob, const char *modifier)
{
    Shiboken::AutoDecRef ob_name(PyObject_GetAttrString(ob, "__name__"));
    Shiboken::AutoDecRef ob_key(GetTypeKey(ob));

    PyObject *dict = TypeKey_to_PropsDict(ob_key, ob);
    if (dict == nullptr)
        return nullptr;
    PyObject *props = PyDict_GetItem(dict, ob_name);
    if (props == nullptr)
        Py_RETURN_NONE;
    return GetSignature_Cached(props, "method", modifier);
}

static PyObject *
GetSignature_Cached(PyObject *props, const char *sig_kind, const char *modifier)
{
    Shiboken::AutoDecRef key(modifier == nullptr
                             ? Py_BuildValue("s", sig_kind)
                             : Py_BuildValue("(ss)", sig_kind, modifier));
    PyObject *value = PyDict_GetItem(props, key);
    if (value == nullptr) {
        // we need to compute a signature object
        value = CreateSignature(props, key);
        if (value != nullptr) {
            if (PyDict_SetItem(props, key, value) < 0)
                // this is an error
                return nullptr;
        }
        else {
            // key not found
            Py_RETURN_NONE;
        }
    }
    return Py_INCREF(value), value;
}

static const char *PySide_CompressedSignaturePackage[] = {
#include "embed/signature.inc"
    };

static const unsigned char PySide_SignatureLoader[] = {
#include "embed/signature_bootstrap.inc"
    };

static safe_globals_struc *
init_phase_1(void)
{
    {
        safe_globals_struc *p = reinterpret_cast<safe_globals_struc *>
                                    (malloc(sizeof(safe_globals_struc)));
        if (p == nullptr)
            goto error;
        /*
         * Initializing module signature_bootstrap.
         * Since we now have an embedding script, we can do this without any
         * Python strings in the C code.
         */
#ifdef Py_LIMITED_API
        // We must work for multiple versions, so use source code.
#else
        Shiboken::AutoDecRef marshal_str(Py_BuildValue("s", "marshal"));
        if (marshal_str.isNull())
            goto error;
        Shiboken::AutoDecRef marshal_module(PyImport_Import(marshal_str));
        if (marshal_module.isNull())
            goto error;
        Shiboken::AutoDecRef loads(PyObject_GetAttrString(marshal_module, "loads"));
        if (loads.isNull())
            goto error;
#endif
        char *bytes_cast = reinterpret_cast<char *>(
                                       const_cast<unsigned char *>(PySide_SignatureLoader));
        Shiboken::AutoDecRef bytes(PyBytes_FromStringAndSize(bytes_cast,
                                       sizeof(PySide_SignatureLoader)));
        if (bytes.isNull())
            goto error;
#ifdef Py_LIMITED_API
        PyObject *builtins = PyEval_GetBuiltins();
        PyObject *compile = PyDict_GetItemString(builtins, "compile");
        if (compile == nullptr)
            goto error;
        Shiboken::AutoDecRef code_obj(PyObject_CallFunction(compile, "Oss",
                                            bytes.object(), "(builtin)", "exec"));
#else
        Shiboken::AutoDecRef code_obj(PyObject_CallFunctionObjArgs(
                                            loads, bytes.object(), nullptr));
#endif
        if (code_obj.isNull())
            goto error;
        p->helper_module = PyImport_ExecCodeModule(const_cast<char *>
                                                       ("signature_bootstrap"), code_obj);
        if (p->helper_module == nullptr)
            goto error;
        // Initialize the module
        PyObject *mdict = PyModule_GetDict(p->helper_module);
        if (PyDict_SetItemString(mdict, "__builtins__", PyEval_GetBuiltins()) < 0)
            goto error;
        /*
         * Unpack an embedded ZIP file with more signature modules.
         * They will be loaded later with the zipimporter.
         * Due to MSVC's limitation to 64k strings, we need to assemble pieces.
         */
        const char **block_ptr = (const char **)PySide_CompressedSignaturePackage;
        int npieces = 0;
        PyObject *piece, *zipped_string_sequence = PyList_New(0);
        if (zipped_string_sequence == nullptr)
            return nullptr;
        for (; **block_ptr != 0; ++block_ptr) {
            npieces++;
            // we avoid the string/unicode dilemma by not using PyString_XXX:
            piece = Py_BuildValue("s", *block_ptr);
            if (piece == nullptr || PyList_Append(zipped_string_sequence, piece) < 0)
                goto error;
        }
        if (PyDict_SetItemString(mdict, "zipstring_sequence", zipped_string_sequence) < 0)
            goto error;
        Py_DECREF(zipped_string_sequence);

        // build a dict for diverse mappings
        p->map_dict = PyDict_New();
        if (p->map_dict == nullptr)
            goto error;

        // build a dict for the prepared arguments
        p->arg_dict = PyDict_New();
        if (p->arg_dict == nullptr
            || PyObject_SetAttrString(p->helper_module, "pyside_arg_dict", p->arg_dict) < 0)
            goto error;
        return p;
    }
error:
    PyErr_Print();
    PyErr_SetString(PyExc_SystemError, "could not initialize part 1");
    return nullptr;
}

static int
init_phase_2(safe_globals_struc *p, PyMethodDef *methods)
{
    {
        PyMethodDef *ml;

        // The single function to be called, but maybe more to come.
        for (ml = methods; ml->ml_name != nullptr; ml++) {
            PyObject *v = PyCFunction_NewEx(ml, nullptr, nullptr);
            if (v == nullptr
                || PyObject_SetAttrString(p->helper_module, ml->ml_name, v) != 0)
                goto error;
            Py_DECREF(v);
        }
        PyObject *bootstrap_func = PyObject_GetAttrString(p->helper_module, "bootstrap");
        if (bootstrap_func == nullptr)
            goto error;
        // The return value of the bootstrap function is the loader module.
        PyObject *loader = PyObject_CallFunction(bootstrap_func, const_cast<char *>("()"));
        if (loader == nullptr)
            goto error;
        // now the loader should be initialized
        p->pyside_type_init_func = PyObject_GetAttrString(loader, "pyside_type_init");
        if (p->pyside_type_init_func == nullptr)
            goto error;
        p->create_signature_func = PyObject_GetAttrString(loader, "create_signature");
        if (p->create_signature_func == nullptr)
            goto error;
        p->seterror_argument_func = PyObject_GetAttrString(loader, "seterror_argument");
        if (p->seterror_argument_func == nullptr)
            goto error;
        p->make_helptext_func = PyObject_GetAttrString(loader, "make_helptext");
        if (p->make_helptext_func == nullptr)
            goto error;
        return 0;
    }
error:
    PyErr_Print();
    PyErr_SetString(PyExc_SystemError, "could not initialize part 2");
    return -1;
}

static int
_fixup_getset(PyTypeObject *type, const char *name, PyGetSetDef *new_gsp)
{
    /*
     * This function pre-fills all fields of the new gsp. We then
     * insert the changed values.
     */
    PyGetSetDef *gsp = type->tp_getset;
    if (gsp != nullptr) {
        for (; gsp->name != nullptr; gsp++) {
            if (strcmp(gsp->name, name) == 0) {
                new_gsp->set = gsp->set;
                new_gsp->doc = gsp->doc;
                new_gsp->closure = gsp->closure;
                return 1;  // success
            }
        }
    }
    // staticmethod has just a __doc__ in the class
    assert(strcmp(type->tp_name, "staticmethod") == 0);
    return 0;
}

static int
add_more_getsets(PyTypeObject *type, PyGetSetDef *gsp, PyObject **old_descr)
{
    /*
     * This function is used to assign a new __signature__ attribute,
     * and also to override a __doc__ attribute.
     */
    assert(PyType_Check(type));
    PyType_Ready(type);
    PyObject *dict = type->tp_dict;
    for (; gsp->name != nullptr; gsp++) {
        PyObject *have_descr = PyDict_GetItemString(dict, gsp->name);
        if (have_descr != nullptr) {
            assert(strcmp(gsp->name, "__doc__") == 0);
            Py_INCREF(have_descr);
            *old_descr = have_descr;
            if (!_fixup_getset(type, gsp->name, gsp))
                continue;
        }
        Shiboken::AutoDecRef descr(PyDescr_NewGetSet(type, gsp));
        if (descr.isNull())
            return -1;
        if (PyDict_SetItemString(dict, gsp->name, descr) < 0)
            return -1;
    }
    PyType_Modified(type);
    return 0;
}

////////////////////////////////////////////////////////////////////////////
//
// Augmenting builtin types with a __signature__ attribute.
//
// This is a harmless change to Python, similar like __text_signature__.
// We could avoid it, but then we would need to copy quite some module
// initialization functions which are pretty version- and word size
// dependent. I think this little patch is the lesser of the two evils.
//
// Please note that in fact we are modifying 'type', the metaclass of all
// objects, because we add new functionality.
//
// Addendum 2019-01-12: We now also compute a docstring from the signature.
//

// keep the original __doc__ functions
static PyObject *old_cf_doc_descr = 0;
static PyObject *old_sm_doc_descr = 0;
static PyObject *old_md_doc_descr = 0;
static PyObject *old_tp_doc_descr = 0;
static PyObject *old_wd_doc_descr = 0;

static int handle_doc_in_progress = 0;

static PyObject *
handle_doc(PyObject *ob, PyObject *old_descr)
{
    init_module_1();
    init_module_2();
    Shiboken::AutoDecRef ob_type(GetClassOfFunc(ob));
    PyTypeObject *type = reinterpret_cast<PyTypeObject *>(ob_type.object());
    if (handle_doc_in_progress || strncmp(type->tp_name, "PySide2.", 8) != 0)
        return PyObject_CallMethod(old_descr, const_cast<char *>("__get__"), const_cast<char *>("(O)"), ob);
    handle_doc_in_progress++;
    PyObject *res = PyObject_CallFunction(
                        pyside_globals->make_helptext_func,
                        const_cast<char *>("(O)"), ob);
    handle_doc_in_progress--;
    if (res == nullptr) {
        PyErr_Print();
        Py_FatalError("handle_doc did not receive a result");
    }
    return res;
}

static PyObject *
pyside_cf_get___doc__(PyObject *cf) {
    return handle_doc(cf, old_cf_doc_descr);
}

static PyObject *
pyside_sm_get___doc__(PyObject *sm) {
    return handle_doc(sm, old_sm_doc_descr);
}

static PyObject *
pyside_md_get___doc__(PyObject *md) {
    return handle_doc(md, old_md_doc_descr);
}

static PyObject *
pyside_tp_get___doc__(PyObject *tp) {
    return handle_doc(tp, old_tp_doc_descr);
}

static PyObject *
pyside_wd_get___doc__(PyObject *wd) {
    return handle_doc(wd, old_wd_doc_descr);
}

static PyGetSetDef new_PyCFunction_getsets[] = {
    {const_cast<char *>("__signature__"), (getter)pyside_cf_get___signature__},
    {const_cast<char *>("__doc__"), (getter)pyside_cf_get___doc__},
    {0}
};

static PyGetSetDef new_PyStaticMethod_getsets[] = {
    {const_cast<char *>("__signature__"), (getter)pyside_sm_get___signature__},
    {const_cast<char *>("__doc__"), (getter)pyside_sm_get___doc__},
    {0}
};

static PyGetSetDef new_PyMethodDescr_getsets[] = {
    {const_cast<char *>("__signature__"), (getter)pyside_md_get___signature__},
    {const_cast<char *>("__doc__"), (getter)pyside_md_get___doc__},
    {0}
};

static PyGetSetDef new_PyType_getsets[] = {
    {const_cast<char *>("__signature__"), (getter)pyside_tp_get___signature__},
    {const_cast<char *>("__doc__"), (getter)pyside_tp_get___doc__},
    {0}
};

static PyGetSetDef new_PyWrapperDescr_getsets[] = {
    {const_cast<char *>("__signature__"), (getter)pyside_wd_get___signature__},
    {const_cast<char *>("__doc__"), (getter)pyside_wd_get___doc__},
    {0}
};

////////////////////////////////////////////////////////////////////////////
//
// get_signature  --  providing a superior interface
//
// Additionally to the interface via __signature__, we also provide
// a general function, which allows for different signature layouts.
// The "modifier" argument is a string that is passed in from loader.py .
// Configuration what the modifiers mean is completely in Python.
//

static PyObject *
get_signature(PyObject *self, PyObject *args)
{
    PyObject *ob;
    const char *modifier = nullptr;

    init_module_1();

    if (!PyArg_ParseTuple(args, "O|s", &ob, &modifier))
        return nullptr;
    if (Py_TYPE(ob) == PepFunction_TypePtr)
        Py_RETURN_NONE;

    if (PyType_IsSubtype(Py_TYPE(ob), &PyCFunction_Type))
        return pyside_cf_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == PepStaticMethod_TypePtr)
        return pyside_sm_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == PepMethodDescr_TypePtr)
        return pyside_md_get___signature__(ob, modifier);
    if (PyType_Check(ob))
        return pyside_tp_get___signature__(ob, modifier);
    if (Py_TYPE(ob) == &PyWrapperDescr_Type)
        return pyside_wd_get___signature__(ob, modifier);
    Py_RETURN_NONE;
}

////////////////////////////////////////////////////////////////////////////
//
// This special Type_Ready does certain initializations earlier with
// our new version.
//

#ifndef _WIN32
////////////////////////////////////////////////////////////////////////////
// a stack trace for linux-like platforms
#include <stdio.h>
#include <execinfo.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

void handler(int sig) {
    void *array[30];
    size_t size;

    // get void*'s for all entries on the stack
    size = backtrace(array, 30);

    // print out all the frames to stderr
    fprintf(stderr, "Error: signal %d:\n", sig);
    backtrace_symbols_fd(array, size, STDERR_FILENO);
    exit(1);
}

////////////////////////////////////////////////////////////////////////////
#endif // _WIN32

static int
PySide_PatchTypes(void)
{
    static int init_done = 0;

    if (!init_done) {
        Shiboken::AutoDecRef meth_descr(PyObject_GetAttrString(
                                        reinterpret_cast<PyObject *>(&PyString_Type), "split"));
        Shiboken::AutoDecRef wrap_descr(PyObject_GetAttrString(
                                        reinterpret_cast<PyObject *>(Py_TYPE(Py_True)), "__add__"));
        if (meth_descr.isNull() || wrap_descr.isNull()
            || PyType_Ready(Py_TYPE(meth_descr)) < 0
            || add_more_getsets(PepMethodDescr_TypePtr,  new_PyMethodDescr_getsets,  &old_md_doc_descr) < 0
            || add_more_getsets(&PyCFunction_Type,       new_PyCFunction_getsets,    &old_cf_doc_descr) < 0
            || add_more_getsets(PepStaticMethod_TypePtr, new_PyStaticMethod_getsets, &old_sm_doc_descr) < 0
            || add_more_getsets(&PyType_Type,            new_PyType_getsets,         &old_tp_doc_descr) < 0
            || add_more_getsets(Py_TYPE(wrap_descr),     new_PyWrapperDescr_getsets, &old_wd_doc_descr) < 0
            )
            return -1;
#ifndef _WIN32
        // We enable the stack trace in CI, only.
        const char *testEnv = getenv("QTEST_ENVIRONMENT");
        if (testEnv && strstr(testEnv, "ci"))
            signal(SIGSEGV, handler);   // install our handler
#endif // _WIN32
        init_done = 1;
    }
    return 0;
}

static void
init_module_1(void)
{
    static int init_done = 0;

    if (!init_done) {
        pyside_globals = init_phase_1();
        if (pyside_globals != nullptr)
            init_done = 1;
    }
}

static int
PySide_BuildSignatureArgs(PyObject *obtype_mod, const char *signatures[])
{
    init_module_1();
    Shiboken::AutoDecRef type_key(GetTypeKey(obtype_mod));
    /*
     * PYSIDE-996: Avoid string overflow in MSVC, which has a limit of
     * 2**15 unicode characters (64 K memory).
     * Instead of one huge string, we take a ssize_t that is the
     * address of a string array. It will not be turned into a real
     * string list until really used by Python. This is quite optimal.
     */
    Shiboken::AutoDecRef numkey(Py_BuildValue("n", signatures));
    if (type_key.isNull() || numkey.isNull()
        || PyDict_SetItem(pyside_globals->arg_dict, type_key, numkey) < 0)
        return -1;
    /*
     * We record also a mapping from type key to type/module. This helps to
     * lazily initialize the Py_LIMITED_API in name_key_to_func().
     */
    return PyDict_SetItem(pyside_globals->map_dict, type_key, obtype_mod) == 0 ? 0 : -1;
}

static PyMethodDef signature_methods[] = {
    {"get_signature", (PyCFunction)get_signature, METH_VARARGS,
        "get the __signature__, but pass an optional string parameter"},
    {nullptr, nullptr}
};

static void
init_module_2(void)
{
    static int init_done = 0;

    if (!init_done) {
        // Phase 2 will call __init__.py which touches a signature, itself.
        // Therefore we set init_done prior to init_phase_2().
        init_done = 1;
        init_phase_2(pyside_globals, signature_methods);
    }
}

static PyObject *
_address_to_stringlist(PyObject *numkey)
{
    ssize_t address = PyNumber_AsSsize_t(numkey, PyExc_ValueError);
    if (address == -1 && PyErr_Occurred())
        return nullptr;
    char **sig_strings = reinterpret_cast<char **>(address);
    PyObject *res_list = PyList_New(0);
    if (res_list == nullptr)
        return nullptr;
    for (; *sig_strings != nullptr; ++sig_strings) {
        char *sig_str = *sig_strings;
        Shiboken::AutoDecRef pystr(Py_BuildValue("s", sig_str));
        if (pystr.isNull() || PyList_Append(res_list, pystr) < 0)
            return nullptr;
    }
    return res_list;
}

static PyObject *
PySide_BuildSignatureProps(PyObject *type_key)
{
    /*
     * Here is the second part of the function.
     * This part will be called on-demand when needed by some attribute.
     * We simply pick up the arguments that we stored here and replace
     * them by the function result.
     */
    init_module_2();
    if (type_key == nullptr)
        return nullptr;
    PyObject *numkey = PyDict_GetItem(pyside_globals->arg_dict, type_key);
    Shiboken::AutoDecRef strings(_address_to_stringlist(numkey));
    if (strings.isNull())
        return nullptr;
    Shiboken::AutoDecRef arg_tup(Py_BuildValue("(OO)", type_key, strings.object()));
    if (arg_tup.isNull())
        return nullptr;
    PyObject *dict = PyObject_CallObject(pyside_globals->pyside_type_init_func, arg_tup);
    if (dict == nullptr) {
        if (PyErr_Occurred())
            return nullptr;
        // No error: return an empty dict.
        if (empty_dict == nullptr)
            empty_dict = PyDict_New();
        return empty_dict;
    }
    // We replace the arguments by the result dict.
    if (PyDict_SetItem(pyside_globals->arg_dict, type_key, dict) < 0)
        return nullptr;
    return dict;
}

static int _finish_nested_classes(PyObject *dict);
static int _build_func_to_type(PyObject *obtype);

static int
PySide_FinishSignatures(PyObject *module, const char *signatures[])
{
    /*
     * Initialization of module functions and resolving of static methods.
     */
    const char *name = PyModule_GetName(module);
    if (name == nullptr)
        return -1;

    // we abuse the call for types, since they both have a __name__ attribute.
    if (PySide_BuildSignatureArgs(module, signatures) < 0)
        return -1;

    /*
     * Note: This function crashed when called from PySide_BuildSignatureArgs.
     * Probably this was too early.
     *
     * Pep384: We need to switch this always on since we have no access
     * to the PyCFunction attributes. Therefore I simplified things
     * and always use our own mapping.
     */
    PyObject *key, *func, *obdict = PyModule_GetDict(module);
    Py_ssize_t pos = 0;

    while (PyDict_Next(obdict, &pos, &key, &func))
        if (PyCFunction_Check(func))
            if (PyDict_SetItem(pyside_globals->map_dict, func, module) < 0)
                return -1;
    if (_finish_nested_classes(obdict) < 0)
        return -1;
    return 0;
}

static int
_finish_nested_classes(PyObject *obdict)
{
    PyObject *key, *value, *obtype;
    PyTypeObject *subtype;
    Py_ssize_t pos = 0;

    if (obdict == nullptr)
        return -1;
    while (PyDict_Next(obdict, &pos, &key, &value)) {
        if (PyType_Check(value)) {
            obtype = value;
            if (_build_func_to_type(obtype) < 0)
                return -1;
            // now continue with nested cases
            subtype = reinterpret_cast<PyTypeObject *>(obtype);
            if (_finish_nested_classes(subtype->tp_dict) < 0)
                return -1;
        }
    }
    return 0;
}

static int
_build_func_to_type(PyObject *obtype)
{
    /*
     * There is no general way to directly get the type of a static method.
     * On Python 3, the type is hidden in an unused pointer in the
     * PyCFunction structure, but the Limited API does not allow to access
     * this, either.
     *
     * In the end, it was easier to avoid such tricks and build an explicit
     * mapping from function to type.
     *
     * We walk through the method list of the type
     * and record the mapping from static method to this type in a dict.
     * We also check for hidden methods, see below.
     */
    PyTypeObject *type = reinterpret_cast<PyTypeObject *>(obtype);
    PyObject *dict = type->tp_dict;
    PyMethodDef *meth = type->tp_methods;

    if (meth == 0)
        return 0;

    for (; meth->ml_name != nullptr; meth++) {
        /*
         * It is possible that a method is overwritten by another
         * attribute with the same name. This case was obviously provoked
         * explicitly in "testbinding.TestObject.staticMethodDouble",
         * where instead of the method a "PySide2.QtCore.Signal" object
         * was in the dict.
         * This overlap is also found in regular PySide under
         * "PySide2.QtCore.QProcess.error" where again a signal object is
         * returned. These hidden methods will be opened for the
         * signature module by adding them under the name
         * "{name}.overload".
         */
        PyObject *descr = PyDict_GetItemString(dict, meth->ml_name);
        const char *look_attr = meth->ml_flags & METH_STATIC ? "__func__" : "__name__";
        int check_name = meth->ml_flags & METH_STATIC ? 0 : 1;
        if (descr == nullptr)
            /*
             * If here, then the method has been DelAttr'ed in
             * Shiboken::ObjectType::introduceProperty
             */
            continue;

        // We first check all methods if one is hidden by something else.
        Shiboken::AutoDecRef look(PyObject_GetAttrString(descr, look_attr));
        Shiboken::AutoDecRef given(Py_BuildValue("s", meth->ml_name));
        if (look.isNull()
            || (check_name && PyObject_RichCompareBool(look, given, Py_EQ) != 1)) {
            PyErr_Clear();
            Shiboken::AutoDecRef cfunc(PyCFunction_NewEx(meth,
                                     reinterpret_cast<PyObject*>(type), nullptr));
            if (cfunc.isNull())
                return -1;
            if (meth->ml_flags & METH_STATIC)
                descr = PyStaticMethod_New(cfunc);
            else
                descr = PyDescr_NewMethod(type, meth);
            if (descr == nullptr)
                return -1;
            char mangled_name[200];
            strcpy(mangled_name, meth->ml_name);
            strcat(mangled_name, ".overload");
            if (PyDict_SetItemString(dict, mangled_name, descr) < 0)
                return -1;
            if (meth->ml_flags & METH_STATIC) {
                // This is the special case where a static method is hidden.
                Shiboken::AutoDecRef special(Py_BuildValue("(Os)", cfunc.object(), "overload"));
                if (PyDict_SetItem(pyside_globals->map_dict, special, obtype) < 0)
                    return -1;
            }
            if (PyDict_SetItemString(pyside_globals->map_dict, mangled_name, obtype) < 0)
                return -1;
            continue;
        }
        // Then we insert the mapping for static methods.
        if (meth->ml_flags & METH_STATIC) {
            if (PyDict_SetItem(pyside_globals->map_dict, look, obtype) < 0)
                return -1;
        }
    }
    return 0;
}

int
SbkSpecial_Type_Ready(PyObject *module, PyTypeObject *type,
                      const char *signatures[])
{
    if (PyType_Ready(type) < 0)
        return -1;
    PyObject *ob_type = reinterpret_cast<PyObject *>(type);
    int ret = PySide_BuildSignatureArgs(ob_type, signatures);
    if (ret < 0) {
        PyErr_Print();
        PyErr_SetNone(PyExc_ImportError);
    }
    return ret;
}

void
FinishSignatureInitialization(PyObject *module, const char *signatures[])
{
    /*
     * This function is called at the very end of a module initialization.
     * We now patch certain types to support the __signature__ attribute,
     * initialize module functions and resolve static methods.
     *
     * Still, it is not possible to call init phase 2 from here,
     * because the import is still running. Do it from Python!
     */
    PySide_PatchTypes();
    if (PySide_FinishSignatures(module, signatures) < 0) {
        PyErr_Print();
        PyErr_SetNone(PyExc_ImportError);
    }
}

void
SetError_Argument(PyObject *args, const char *func_name)
{
    /*
     * This function replaces the type error construction with extra
     * overloads parameter in favor of using the signature module.
     * Error messages are rare, so we do it completely in Python.
     */
    init_module_1();
    init_module_2();
    Shiboken::AutoDecRef res(PyObject_CallFunction(
                                 pyside_globals->seterror_argument_func,
                                 const_cast<char *>("(Os)"), args, func_name));
    if (res.isNull()) {
        PyErr_Print();
        Py_FatalError("seterror_argument did not receive a result");
    }
    PyObject *err, *msg;
    if (!PyArg_UnpackTuple(res, func_name, 2, 2, &err, &msg)) {
        PyErr_Print();
        Py_FatalError("unexpected failure in seterror_argument");
    }
    PyErr_SetObject(err, msg);
}

} //extern "C"
