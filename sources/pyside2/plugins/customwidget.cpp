/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
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

#include "customwidget.h"
#include <QtCore/qdebug.h>

// Part of the static plugin linked to the QtUiLoader Python module,
// allowing it to create a custom widget written in Python.
PyCustomWidget::PyCustomWidget(PyObject *objectType) :
    m_pyObject(objectType),
    m_name(QString::fromUtf8(reinterpret_cast<PyTypeObject *>(objectType)->tp_name))
{
}

bool PyCustomWidget::isContainer() const
{
    return false;
}

bool PyCustomWidget::isInitialized() const
{
    return m_initialized;
}

QIcon PyCustomWidget::icon() const
{
    return QIcon();
}

QString PyCustomWidget::domXml() const
{
    return QString();
}

QString PyCustomWidget::group() const
{
    return QString();
}

QString PyCustomWidget::includeFile() const
{
    return QString();
}

QString PyCustomWidget::name() const
{
    return m_name;
}

QString PyCustomWidget::toolTip() const
{
    return QString();
}

QString PyCustomWidget::whatsThis() const
{
    return QString();
}

QWidget *PyCustomWidget::createWidget(QWidget *parent)
{
    // Create a python instance and return cpp object
    PyObject *pyParent = nullptr;
    bool unknownParent = false;
    if (parent) {
        pyParent = reinterpret_cast<PyObject *>(Shiboken::BindingManager::instance().retrieveWrapper(parent));
        if (pyParent) {
            Py_INCREF(pyParent);
        } else {
            static Shiboken::Conversions::SpecificConverter converter("QWidget*");
            pyParent = converter.toPython(&parent);
            unknownParent = true;
        }
    } else {
        Py_INCREF(Py_None);
        pyParent = Py_None;
    }

    Shiboken::AutoDecRef pyArgs(PyTuple_New(1));
    PyTuple_SET_ITEM(pyArgs, 0, pyParent); // tuple will keep pyParent reference

    // Call python constructor
    auto result = reinterpret_cast<SbkObject *>(PyObject_CallObject(m_pyObject, pyArgs));
    if (!result) {
        qWarning("Unable to create a Python custom widget of type \"%s\".",
                 qPrintable(m_name));
        PyErr_Print();
        return nullptr;
    }

    if (unknownParent) // if parent does not exist in python, transfer the ownership to cpp
        Shiboken::Object::releaseOwnership(result);
    else
        Shiboken::Object::setParent(pyParent, reinterpret_cast<PyObject *>(result));

    return reinterpret_cast<QWidget *>(Shiboken::Object::cppPointer(result, Py_TYPE(result)));
}

void PyCustomWidget::initialize(QDesignerFormEditorInterface *core)
{
    m_initialized = true;
}
