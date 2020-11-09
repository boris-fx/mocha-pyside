#############################################################################
##
## Copyright (C) 2017 The Qt Company Ltd.
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

import os
import sys
import unittest

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from init_paths import init_test_paths
init_test_paths(False)

import PySide2

# This test tests the new "macro" feature of qApp.
# It also uses the qApp variable to finish the instance and start over.

# Note: this test makes qapplication_singleton_test.py obsolete.

class qAppMacroTest(unittest.TestCase):
    _test_1093_is_first = True

    def test_qApp_is_like_a_macro_and_can_restart(self):
        self._test_1093_is_first = False
        from PySide2 import QtCore
        try:
            from PySide2 import QtGui, QtWidgets
        except ImportError:
            QtWidgets = QtGui = QtCore
        # qApp is in the builtins
        self.assertEqual(bool(qApp), False)
        # and the type is None
        self.assertTrue(qApp is None)
        # now we create an application for all cases
        classes = (QtCore.QCoreApplication,
                   QtGui.QGuiApplication,
                   QtWidgets.QApplication)
        for klass in classes:
            print("created", klass([]))
            qApp.shutdown()
            print("deleted qApp", qApp)
        # creating without deletion raises:
        QtCore.QCoreApplication([])
        with self.assertRaises(RuntimeError):
            QtCore.QCoreApplication([])
        self.assertEqual(QtCore.QCoreApplication.instance(), qApp)

    def test_1093(self):
        # Test that without creating a QApplication staticMetaObject still exists.
        # Please see https://bugreports.qt.io/browse/PYSIDE-1093 for explanation.
        # Note: This test must run first, otherwise we would be mislead!
        assert self._test_1093_is_first
        from PySide2 import QtCore
        self.assertTrue(QtCore.QObject.staticMetaObject is not None)
        app = QtCore.QCoreApplication.instance()
        self.assertTrue(QtCore.QObject.staticMetaObject is not None)
        if app is None:
            app = QtCore.QCoreApplication([])
        self.assertTrue(QtCore.QObject.staticMetaObject is not None)
        qApp.shutdown()


if __name__ == '__main__':
    unittest.main()
