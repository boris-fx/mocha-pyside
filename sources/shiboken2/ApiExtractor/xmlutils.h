/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt for Python.
**
** $QT_BEGIN_LICENSE:GPL-EXCEPT$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#ifndef XMLUTILS_H
#define XMLUTILS_H

#include <QtCore/QSharedPointer>
#include <QtCore/QString>

class XQuery
{
public:
    Q_DISABLE_COPY(XQuery);

    virtual ~XQuery();

    QString evaluate(QString xPathExpression, QString *errorMessage);

    static QSharedPointer<XQuery> create(const QString &focus, QString *errorMessage);

protected:
    XQuery();

    virtual QString doEvaluate(const QString &xPathExpression, QString *errorMessage) = 0;
};

QString xsl_transform(const QString &xml, const QString &xsl, QString *errorMessage);

#endif // XMLUTILS_H
