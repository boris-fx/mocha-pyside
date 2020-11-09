#!/usr/bin/python
# -*- coding: utf-8 -*-

#############################################################################
##
## Copyright (C) 2016 The Qt Company Ltd.
## Contact: https://www.qt.io/licensing/
##
## This file is part of the test suite of Qt for Python.
##
## $QT_BEGIN_LICENSE:GPL-EXCEPT$
## Commercial License Usage
## Licensees holding valid commercial Qt licenses may use this file in
## accordance with the commercial license agreement provided with the
## Software or, alternatively, in accordance with the terms contained in
## a written agreement between you and The Qt Company. For licensing terms
## and conditions see https://www.qt.io/terms-conditions. For further
## information use the contact form at https://www.qt.io/contact-us.
##
## GNU General Public License Usage
## Alternatively, this file may be used under the terms of the GNU
## General Public License version 3 as published by the Free Software
## Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
## included in the packaging of this file. Please review the following
## information to ensure the GNU General Public License requirements will
## be met: https://www.gnu.org/licenses/gpl-3.0.html.
##
## $QT_END_LICENSE$
##
#############################################################################

'''Test cases for QByteArray concatenation with '+' operator'''

import os
import sys
import unittest

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from init_paths import init_test_paths
init_test_paths(False)

from PySide2.QtCore import QByteArray
import py3kcompat as py3k

class QByteArrayConcatenationOperatorTest(unittest.TestCase):
    '''Test cases for QByteArray concatenation with '+' operator'''

    def testConcatQByteArrayAndPythonString(self):
        #Test concatenation of a QByteArray with a Python bytes, in this order
        qba = QByteArray(py3k.b('foo'))
        result = qba + py3k.b('\x00bar')
        self.assertEqual(type(result), QByteArray)
        self.assertEqual(result, py3k.b('foo\x00bar'))

    def testConcatPythonStringAndQByteArray(self):
        #Test concatenation of a Python bytes with a QByteArray, in this order
        concat_python_string_add_qbytearray_worked = True
        qba = QByteArray(py3k.b('foo'))
        result = py3k.b('bar\x00') + qba
        self.assertEqual(type(result), QByteArray)
        self.assertEqual(result, py3k.b('bar\x00foo'))

if __name__ == '__main__':
    unittest.main()

