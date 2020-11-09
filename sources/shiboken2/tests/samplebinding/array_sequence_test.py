#!/usr/bin/env python

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

'''Test case for Array types (PySequence).'''

import os
import sys
import unittest

sys.path.append(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from shiboken_paths import init_paths
init_paths()
import sample

class ArrayTester(unittest.TestCase):
    '''Test case for arrays.'''

    def testIntArray(self):
        intList = [1, 2, 3, 4]
        self.assertEqual(sample.sumIntArray(intList), 10)

    def testIntArrayModified(self):
        intList = [1, 2, 3, 4]
        tester = sample.ArrayModifyTest()
        self.assertEqual(tester.sumIntArray(4, intList), 10)

    def testDoubleArray(self):
        doubleList = [1.2, 2.3, 3.4, 4.5]
        self.assertEqual(sample.sumDoubleArray(doubleList), 11.4)

if __name__ == '__main__':
    unittest.main()
