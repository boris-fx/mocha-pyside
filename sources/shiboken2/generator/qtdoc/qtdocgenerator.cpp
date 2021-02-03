/****************************************************************************
**
** Copyright (C) 2020 The Qt Company Ltd.
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

#include "qtdocgenerator.h"
#include "ctypenames.h"
#include <abstractmetalang.h>
#include <messages.h>
#include <propertyspec.h>
#include <reporthandler.h>
#include <typesystem.h>
#include <qtdocparser.h>
#include <doxygenparser.h>
#include <typedatabase.h>
#include <algorithm>
#include <QtCore/QStack>
#include <QtCore/QRegularExpression>
#include <QtCore/QTextStream>
#include <QtCore/QXmlStreamReader>
#include <QtCore/QFile>
#include <QtCore/QDir>
#include <fileout.h>
#include <limits>

static Indentor INDENT;

static inline QString additionalDocumentationOption() { return QStringLiteral("additional-documentation"); }

static inline QString nameAttribute() { return QStringLiteral("name"); }
static inline QString titleAttribute() { return QStringLiteral("title"); }
static inline QString fullTitleAttribute() { return QStringLiteral("fulltitle"); }
static inline QString briefAttribute() { return QStringLiteral("brief"); }
static inline QString briefStartElement() { return QStringLiteral("<brief>"); }
static inline QString briefEndElement() { return QStringLiteral("</brief>"); }

static inline QString none() { return QStringLiteral("None"); }

static void stripPythonQualifiers(QString *s)
{
    const int lastSep = s->lastIndexOf(QLatin1Char('.'));
    if (lastSep != -1)
        s->remove(0, lastSep + 1);
}

static bool shouldSkip(const AbstractMetaFunction* func)
{
    // Constructors go to separate section
    if (DocParser::skipForQuery(func) || func->isConstructor())
        return true;

    // Search a const clone (QImage::bits() vs QImage::bits() const)
    if (func->isConstant() || !func->ownerClass())
        return false;

    const AbstractMetaArgumentList funcArgs = func->arguments();
    const AbstractMetaFunctionList &ownerFunctions = func->ownerClass()->functions();
    for (AbstractMetaFunction *f : ownerFunctions) {
        if (f != func
            && f->isConstant()
            && f->name() == func->name()
            && f->arguments().count() == funcArgs.count()) {
            // Compare each argument
            bool cloneFound = true;

            const AbstractMetaArgumentList fargs = f->arguments();
            for (int i = 0, max = funcArgs.count(); i < max; ++i) {
                if (funcArgs.at(i)->type()->typeEntry() != fargs.at(i)->type()->typeEntry()) {
                    cloneFound = false;
                    break;
                }
            }
            if (cloneFound)
                return true;
        }
    }
    return false;
}

static bool functionSort(const AbstractMetaFunction* func1, const AbstractMetaFunction* func2)
{
    return func1->name() < func2->name();
}

class Pad
{
public:
    explicit Pad(char c, int count) : m_char(c), m_count(count) {}

    void write(QTextStream &str) const
    {
        for (int i = 0; i < m_count; ++i)
            str << m_char;
    }

private:
    const char m_char;
    const int m_count;
};

inline QTextStream &operator<<(QTextStream &str, const Pad &pad)
{
    pad.write(str);
    return str;
}

template <class String>
static int writeEscapedRstText(QTextStream &str, const String &s)
{
    int escaped = 0;
    for (const QChar &c : s) {
        switch (c.unicode()) {
        case '*':
        case '`':
        case '_':
        case '\\':
            str << '\\';
            ++escaped;
            break;
        }
        str << c;
    }
    return s.size() + escaped;
}

class escape
{
public:
    explicit escape(const QStringRef &s) : m_string(s) {}

    void write(QTextStream &str) const { writeEscapedRstText(str, m_string); }

private:
    const QStringRef m_string;
};

inline QTextStream &operator<<(QTextStream &str, const escape &e)
{
    e.write(str);
    return str;
}

// Return last character of a QString-buffered stream.
static QChar lastChar(const QTextStream &str)
{
    const QString *string = str.string();
    Q_ASSERT(string);
    return string->isEmpty() ? QChar() : *(string->crbegin());
}

static QTextStream &ensureEndl(QTextStream &s)
{
    if (lastChar(s) != QLatin1Char('\n'))
        s << Qt::endl;
    return s;
}

static inline QVersionNumber versionOf(const TypeEntry *te)
{
    if (te) {
        const auto version = te->version();
        if (!version.isNull() && version > QVersionNumber(0, 0))
            return version;
    }
    return QVersionNumber();
}

struct rstVersionAdded
{
    explicit rstVersionAdded(const QVersionNumber &v) : m_version(v) {}

    const QVersionNumber m_version;
};

static QTextStream &operator<<(QTextStream &s, const rstVersionAdded &v)
{
    s << ".. versionadded:: "<< v.m_version.toString() << "\n\n";
    return s;
}

static QByteArray rstDeprecationNote(const char *what)
{
    return QByteArrayLiteral(".. note:: This ")
        + what + QByteArrayLiteral(" is deprecated.\n\n");
}

// RST anchor string: Anything else but letters, numbers, '_' or '.' replaced by '-'
static inline bool isValidRstLabelChar(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_') || c == QLatin1Char('.');
}

static QString toRstLabel(QString s)
{
    for (int i = 0, size = s.size(); i < size; ++i) {
        if (!isValidRstLabelChar(s.at(i)))
            s[i] = QLatin1Char('-');
    }
    return s;
}

class rstLabel
{
public:
    explicit rstLabel(const QString &l) : m_label(l) {}

    friend QTextStream &operator<<(QTextStream &str, const rstLabel &a)
    {
        str << ".. _" << toRstLabel(a.m_label) << ":\n\n";
        return str;
    }

private:
    const QString &m_label;
};

struct QtXmlToSphinx::LinkContext
{
    enum Type
    {
        Method = 0x1, Function = 0x2,
        FunctionMask = Method | Function,
        Class = 0x4, Attribute = 0x8, Module = 0x10,
        Reference = 0x20, External= 0x40
    };

    enum Flags { InsideBold = 0x1, InsideItalic = 0x2 };

    explicit LinkContext(const QString &ref) : linkRef(ref) {}

    QString linkRef;
    QString linkText;
    Type type = Reference;
    int flags = 0;
};

static const char *linkKeyWord(QtXmlToSphinx::LinkContext::Type type)
{
    switch (type) {
    case QtXmlToSphinx::LinkContext::Method:
        return ":meth:";
    case QtXmlToSphinx::LinkContext::Function:
        return ":func:";
    case QtXmlToSphinx::LinkContext::Class:
        return ":class:";
    case QtXmlToSphinx::LinkContext::Attribute:
        return ":attr:";
    case QtXmlToSphinx::LinkContext::Module:
        return ":mod:";
    case QtXmlToSphinx::LinkContext::Reference:
        return ":ref:";
    case QtXmlToSphinx::LinkContext::External:
        break;
    case QtXmlToSphinx::LinkContext::FunctionMask:
        break;
     }
    return "";
}

QTextStream &operator<<(QTextStream &str, const QtXmlToSphinx::LinkContext &linkContext)
{
    // Temporarily turn off bold/italic since links do not work within
    if (linkContext.flags & QtXmlToSphinx::LinkContext::InsideBold)
        str << "**";
    else if (linkContext.flags & QtXmlToSphinx::LinkContext::InsideItalic)
        str << '*';
    str << ' ' << linkKeyWord(linkContext.type) << '`';
    const bool isExternal = linkContext.type == QtXmlToSphinx::LinkContext::External;
    if (!linkContext.linkText.isEmpty()) {
        writeEscapedRstText(str, linkContext.linkText);
        if (isExternal && !linkContext.linkText.endsWith(QLatin1Char(' ')))
            str << ' ';
        str << '<';
    }
    // Convert page titles to RST labels
    str << (linkContext.type == QtXmlToSphinx::LinkContext::Reference
        ? toRstLabel(linkContext.linkRef) : linkContext.linkRef);
    if (!linkContext.linkText.isEmpty())
        str << '>';
    str << '`';
    if (isExternal)
        str << '_';
    str << ' ';
    if (linkContext.flags & QtXmlToSphinx::LinkContext::InsideBold)
        str << "**";
    else if (linkContext.flags & QtXmlToSphinx::LinkContext::InsideItalic)
        str << '*';
    return str;
}

QtXmlToSphinx::QtXmlToSphinx(QtDocGenerator* generator, const QString& doc, const QString& context)
        : m_tableHasHeader(false), m_context(context), m_generator(generator), m_insideBold(false), m_insideItalic(false)
{
    m_handlerMap.insert(QLatin1String("heading"), &QtXmlToSphinx::handleHeadingTag);
    m_handlerMap.insert(QLatin1String("brief"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("para"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("italic"), &QtXmlToSphinx::handleItalicTag);
    m_handlerMap.insert(QLatin1String("bold"), &QtXmlToSphinx::handleBoldTag);
    m_handlerMap.insert(QLatin1String("see-also"), &QtXmlToSphinx::handleSeeAlsoTag);
    m_handlerMap.insert(QLatin1String("snippet"), &QtXmlToSphinx::handleSnippetTag);
    m_handlerMap.insert(QLatin1String("dots"), &QtXmlToSphinx::handleDotsTag);
    m_handlerMap.insert(QLatin1String("codeline"), &QtXmlToSphinx::handleDotsTag);
    m_handlerMap.insert(QLatin1String("table"), &QtXmlToSphinx::handleTableTag);
    m_handlerMap.insert(QLatin1String("header"), &QtXmlToSphinx::handleRowTag);
    m_handlerMap.insert(QLatin1String("row"), &QtXmlToSphinx::handleRowTag);
    m_handlerMap.insert(QLatin1String("item"), &QtXmlToSphinx::handleItemTag);
    m_handlerMap.insert(QLatin1String("argument"), &QtXmlToSphinx::handleArgumentTag);
    m_handlerMap.insert(QLatin1String("teletype"), &QtXmlToSphinx::handleArgumentTag);
    m_handlerMap.insert(QLatin1String("link"), &QtXmlToSphinx::handleLinkTag);
    m_handlerMap.insert(QLatin1String("inlineimage"), &QtXmlToSphinx::handleInlineImageTag);
    m_handlerMap.insert(QLatin1String("image"), &QtXmlToSphinx::handleImageTag);
    m_handlerMap.insert(QLatin1String("list"), &QtXmlToSphinx::handleListTag);
    m_handlerMap.insert(QLatin1String("term"), &QtXmlToSphinx::handleTermTag);
    m_handlerMap.insert(QLatin1String("raw"), &QtXmlToSphinx::handleRawTag);
    m_handlerMap.insert(QLatin1String("underline"), &QtXmlToSphinx::handleItalicTag);
    m_handlerMap.insert(QLatin1String("superscript"), &QtXmlToSphinx::handleSuperScriptTag);
    m_handlerMap.insert(QLatin1String("code"), &QtXmlToSphinx::handleCodeTag);
    m_handlerMap.insert(QLatin1String("badcode"), &QtXmlToSphinx::handleCodeTag);
    m_handlerMap.insert(QLatin1String("legalese"), &QtXmlToSphinx::handleCodeTag);
    m_handlerMap.insert(QLatin1String("rst"), &QtXmlToSphinx::handleRstPassTroughTag);
    m_handlerMap.insert(QLatin1String("section"), &QtXmlToSphinx::handleAnchorTag);
    m_handlerMap.insert(QLatin1String("quotefile"), &QtXmlToSphinx::handleQuoteFileTag);

    // ignored tags
    m_handlerMap.insert(QLatin1String("generatedlist"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("tableofcontents"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("quotefromfile"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("skipto"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("target"), &QtXmlToSphinx::handleTargetTag);
    m_handlerMap.insert(QLatin1String("page"), &QtXmlToSphinx::handlePageTag);
    m_handlerMap.insert(QLatin1String("group"), &QtXmlToSphinx::handlePageTag);

    // useless tags
    m_handlerMap.insert(QLatin1String("description"), &QtXmlToSphinx::handleUselessTag);
    m_handlerMap.insert(QLatin1String("definition"), &QtXmlToSphinx::handleUselessTag);
    m_handlerMap.insert(QLatin1String("printuntil"), &QtXmlToSphinx::handleUselessTag);
    m_handlerMap.insert(QLatin1String("relation"), &QtXmlToSphinx::handleUselessTag);

    // Doxygen tags
    m_handlerMap.insert(QLatin1String("title"), &QtXmlToSphinx::handleHeadingTag);
    m_handlerMap.insert(QLatin1String("ref"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("computeroutput"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("detaileddescription"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("name"), &QtXmlToSphinx::handleParaTag);
    m_handlerMap.insert(QLatin1String("listitem"), &QtXmlToSphinx::handleItemTag);
    m_handlerMap.insert(QLatin1String("parametername"), &QtXmlToSphinx::handleItemTag);
    m_handlerMap.insert(QLatin1String("parameteritem"), &QtXmlToSphinx::handleItemTag);
    m_handlerMap.insert(QLatin1String("ulink"), &QtXmlToSphinx::handleLinkTag);
    m_handlerMap.insert(QLatin1String("itemizedlist"), &QtXmlToSphinx::handleListTag);
    m_handlerMap.insert(QLatin1String("parameternamelist"), &QtXmlToSphinx::handleListTag);
    m_handlerMap.insert(QLatin1String("parameterlist"), &QtXmlToSphinx::handleListTag);

    // Doxygen ignored tags
    m_handlerMap.insert(QLatin1String("highlight"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("linebreak"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("programlisting"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("xreftitle"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("sp"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("entry"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("simplesect"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("verbatim"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("xrefsect"), &QtXmlToSphinx::handleIgnoredTag);
    m_handlerMap.insert(QLatin1String("xrefdescription"), &QtXmlToSphinx::handleIgnoredTag);

    m_result = transform(doc);
}

void QtXmlToSphinx::pushOutputBuffer()
{
    auto *buffer = new QString();
    m_buffers << buffer;
    m_output.setString(buffer);
}

QString QtXmlToSphinx::popOutputBuffer()
{
    Q_ASSERT(!m_buffers.isEmpty());
    QString* str = m_buffers.pop();
    QString strcpy(*str);
    delete str;
    m_output.setString(m_buffers.isEmpty() ? 0 : m_buffers.top());
    return strcpy;
}

QString QtXmlToSphinx::expandFunction(const QString& function) const
{
    const int firstDot = function.indexOf(QLatin1Char('.'));
    const AbstractMetaClass *metaClass = nullptr;
    if (firstDot != -1) {
        const QStringRef className = function.leftRef(firstDot);
        for (const AbstractMetaClass *cls : m_generator->classes()) {
            if (cls->name() == className) {
                metaClass = cls;
                break;
            }
        }
    }

    return metaClass
        ? metaClass->typeEntry()->qualifiedTargetLangName()
          + function.right(function.size() - firstDot)
        : function;
}

QString QtXmlToSphinx::resolveContextForMethod(const QString& methodName) const
{
    const QStringRef currentClass = m_context.splitRef(QLatin1Char('.')).constLast();

    const AbstractMetaClass *metaClass = nullptr;
    for (const AbstractMetaClass *cls : m_generator->classes()) {
        if (cls->name() == currentClass) {
            metaClass = cls;
            break;
        }
    }

    if (metaClass) {
        AbstractMetaFunctionList funcList;
        const AbstractMetaFunctionList &methods = metaClass->queryFunctionsByName(methodName);
        for (AbstractMetaFunction *func : methods) {
            if (methodName == func->name())
                funcList.append(func);
        }

        const AbstractMetaClass *implementingClass = nullptr;
        for (AbstractMetaFunction *func : qAsConst(funcList)) {
            implementingClass = func->implementingClass();
            if (implementingClass->name() == currentClass)
                break;
        }

        if (implementingClass)
            return implementingClass->typeEntry()->qualifiedTargetLangName();
    }

    return QLatin1Char('~') + m_context;
}

QString QtXmlToSphinx::transform(const QString& doc)
{
    Q_ASSERT(m_buffers.isEmpty());
    Indentation indentation(INDENT);
    if (doc.trimmed().isEmpty())
        return doc;

    pushOutputBuffer();

    QXmlStreamReader reader(doc);

    while (!reader.atEnd()) {
        QXmlStreamReader::TokenType token = reader.readNext();
        if (reader.hasError()) {
            QString message;
            QTextStream(&message) << "XML Error "
                << reader.errorString() << " at " << reader.lineNumber()
                << ':' << reader.columnNumber() << '\n' << doc;
            m_output << INDENT << message;
            qCWarning(lcShibokenDoc).noquote().nospace() << message;
            break;
        }

        if (token == QXmlStreamReader::StartElement) {
            QStringRef tagName = reader.name();
            TagHandler handler = m_handlerMap.value(tagName.toString(), &QtXmlToSphinx::handleUnknownTag);
            if (!m_handlers.isEmpty() && ( (m_handlers.top() == &QtXmlToSphinx::handleIgnoredTag) ||
                                           (m_handlers.top() == &QtXmlToSphinx::handleRawTag)) )
                handler = &QtXmlToSphinx::handleIgnoredTag;

            m_handlers.push(handler);
        }
        if (!m_handlers.isEmpty())
            (this->*(m_handlers.top()))(reader);

        if (token == QXmlStreamReader::EndElement) {
            m_handlers.pop();
            m_lastTagName = reader.name().toString();
        }
    }

    if (!m_inlineImages.isEmpty()) {
        // Write out inline image definitions stored in handleInlineImageTag().
        m_output << Qt::endl;
        for (const InlineImage &img : qAsConst(m_inlineImages))
            m_output << ".. |" << img.tag << "| image:: " << img.href << Qt::endl;
        m_output << Qt::endl;
        m_inlineImages.clear();
    }

    m_output.flush();
    QString retval = popOutputBuffer();
    Q_ASSERT(m_buffers.isEmpty());
    return retval;
}

static QString resolveFile(const QStringList &locations, const QString &path)
{
    for (QString location : locations) {
        location.append(QLatin1Char('/'));
        location.append(path);
        if (QFileInfo::exists(location))
            return location;
    }
    return QString();
}

QString QtXmlToSphinx::readFromLocations(const QStringList &locations, const QString &path,
                                         const QString &identifier, QString *errorMessage)
{
    QString resolvedPath;
    if (path.endsWith(QLatin1String(".cpp"))) {
        const QString pySnippet = path.left(path.size() - 3) + QLatin1String("py");
        resolvedPath = resolveFile(locations, pySnippet);
    }
    if (resolvedPath.isEmpty())
        resolvedPath = resolveFile(locations, path);
    if (resolvedPath.isEmpty()) {
        QTextStream(errorMessage) << "Could not resolve \"" << path << "\" in \""
           << locations.join(QLatin1String("\", \""));
        return QString(); // null
    }
    qCDebug(lcShibokenDoc).noquote().nospace() << "snippet file " << path
        << " [" << identifier << ']' << " resolved to " << resolvedPath;
    return readFromLocation(resolvedPath, identifier, errorMessage);
}

QString QtXmlToSphinx::readFromLocation(const QString &location, const QString &identifier,
                                        QString *errorMessage)
{
    QFile inputFile;
    inputFile.setFileName(location);
    if (!inputFile.open(QIODevice::ReadOnly)) {
        QTextStream(errorMessage) << "Could not read code snippet file: "
            << QDir::toNativeSeparators(inputFile.fileName())
            << ": " << inputFile.errorString();
        return QString(); // null
    }

    QString code = QLatin1String(""); // non-null
    if (identifier.isEmpty()) {
        while (!inputFile.atEnd())
            code += QString::fromUtf8(inputFile.readLine());
        return code;
    }

    const QRegularExpression searchString(QLatin1String("//!\\s*\\[")
                                          + identifier + QLatin1String("\\]"));
    Q_ASSERT(searchString.isValid());
    static const QRegularExpression codeSnippetCode(QLatin1String("//!\\s*\\[[\\w\\d\\s]+\\]"));
    Q_ASSERT(codeSnippetCode.isValid());

    bool getCode = false;

    while (!inputFile.atEnd()) {
        QString line = QString::fromUtf8(inputFile.readLine());
        if (getCode && !line.contains(searchString)) {
            line.remove(codeSnippetCode);
            code += line;
        } else if (line.contains(searchString)) {
            if (getCode)
                break;
            getCode = true;
        }
    }

    if (!getCode) {
        QTextStream(errorMessage) << "Code snippet file found ("
            << QDir::toNativeSeparators(location) << "), but snippet ["
            << identifier << "] not found.";
        return QString(); // null
    }

    return code;
}

void QtXmlToSphinx::handleHeadingTag(QXmlStreamReader& reader)
{
    static int headingSize = 0;
    static char type;
    static char types[] = { '-', '^' };
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        uint typeIdx = reader.attributes().value(QLatin1String("level")).toUInt();
        if (typeIdx >= sizeof(types))
            type = types[sizeof(types)-1];
        else
            type = types[typeIdx];
    } else if (token == QXmlStreamReader::EndElement) {
        m_output << Pad(type, headingSize) << Qt::endl << Qt::endl;
    } else if (token == QXmlStreamReader::Characters) {
        m_output << Qt::endl << Qt::endl;
        headingSize = writeEscapedRstText(m_output, reader.text().trimmed());
        m_output << Qt::endl;
    }
}

void QtXmlToSphinx::handleParaTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        pushOutputBuffer();
    } else if (token == QXmlStreamReader::EndElement) {
        QString result = popOutputBuffer().simplified();
        if (result.startsWith(QLatin1String("**Warning:**")))
            result.replace(0, 12, QLatin1String(".. warning:: "));
        else if (result.startsWith(QLatin1String("**Note:**")))
            result.replace(0, 9, QLatin1String(".. note:: "));

        m_output << INDENT << result << Qt::endl << Qt::endl;
    } else if (token == QXmlStreamReader::Characters) {
        const QStringRef text = reader.text();
        const QChar end = lastChar(m_output);
        if (!text.isEmpty() && INDENT.indent == 0 && !end.isNull()) {
            QChar start = text[0];
            if ((end == QLatin1Char('*') || end == QLatin1Char('`')) && start != QLatin1Char(' ') && !start.isPunct())
                m_output << '\\';
        }
        m_output << INDENT << escape(text);
    }
}

void QtXmlToSphinx::handleItalicTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement || token == QXmlStreamReader::EndElement) {
        m_insideItalic = !m_insideItalic;
        m_output << '*';
    } else if (token == QXmlStreamReader::Characters) {
        m_output << escape(reader.text().trimmed());
    }
}

void QtXmlToSphinx::handleBoldTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement || token == QXmlStreamReader::EndElement) {
        m_insideBold = !m_insideBold;
        m_output << "**";
    } else if (token == QXmlStreamReader::Characters) {
        m_output << escape(reader.text().trimmed());
    }
}

void QtXmlToSphinx::handleArgumentTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement || token == QXmlStreamReader::EndElement)
        m_output << "``";
    else if (token == QXmlStreamReader::Characters)
        m_output << reader.text().trimmed();
}

static inline QString functionLinkType() { return QStringLiteral("function"); }
static inline QString classLinkType() { return QStringLiteral("class"); }

static inline QString fixLinkType(const QStringRef &type)
{
    // TODO: create a flag PROPERTY-AS-FUNCTION to ask if the properties
    // are recognized as such or not in the binding
    if (type == QLatin1String("property"))
        return functionLinkType();
    if (type == QLatin1String("typedef"))
        return classLinkType();
    return type.toString();
}

static inline QString linkSourceAttribute(const QString &type)
{
    if (type == functionLinkType() || type == classLinkType())
        return QLatin1String("raw");
    return type == QLatin1String("enum") || type == QLatin1String("page")
        ? type : QLatin1String("href");
}

// "See also" links may appear as nested links:
//     <see-also>QAbstractXmlReceiver<link raw="isValid()" href="qxmlquery.html#isValid" type="function">isValid()</link>
//     which is handled in handleLinkTag
// or direct text:
//     <see-also>rootIsDecorated()</see-also>
//     which is handled here.

void QtXmlToSphinx::handleSeeAlsoTag(QXmlStreamReader& reader)
{
    switch (reader.tokenType()) {
    case QXmlStreamReader::StartElement:
        m_output << INDENT << ".. seealso:: ";
        break;
    case QXmlStreamReader::Characters: {
        // Direct embedded link: <see-also>rootIsDecorated()</see-also>
        const QStringRef textR = reader.text().trimmed();
        if (!textR.isEmpty()) {
            const QString text = textR.toString();
            if (m_seeAlsoContext.isNull()) {
                const QString type = text.endsWith(QLatin1String("()"))
                    ? functionLinkType() : classLinkType();
                m_seeAlsoContext.reset(handleLinkStart(type, text));
            }
            handleLinkText(m_seeAlsoContext.data(), text);
        }
    }
        break;
    case QXmlStreamReader::EndElement:
        if (!m_seeAlsoContext.isNull()) { // direct, no nested </link> seen
            handleLinkEnd(m_seeAlsoContext.data());
            m_seeAlsoContext.reset();
        }
        m_output << Qt::endl << Qt::endl;
        break;
    default:
        break;
    }
}

static inline QString fallbackPathAttribute() { return QStringLiteral("path"); }

static inline bool snippetComparison()
{
    return ReportHandler::debugLevel() >= ReportHandler::FullDebug;
}

template <class Indent> // const char*/class Indentor
void formatSnippet(QTextStream &str, Indent indent, const QString &snippet)
{
    const QVector<QStringRef> lines = snippet.splitRef(QLatin1Char('\n'));
    for (const QStringRef &line : lines) {
        if (!line.trimmed().isEmpty())
            str << indent << line;
        str << Qt::endl;
    }
}

static QString msgSnippetComparison(const QString &location, const QString &identifier,
                                    const QString &pythonCode, const QString &fallbackCode)
{
    QString result;
    QTextStream str(&result);
    str << "Python snippet " << location;
    if (!identifier.isEmpty())
        str << " [" << identifier << ']';
    str << ":\n";
    formatSnippet(str, "  ", pythonCode);
    str << "Corresponding fallback snippet:\n";
    formatSnippet(str, "  ", fallbackCode);
    str << "-- end --\n";
    return result;
}

void QtXmlToSphinx::handleSnippetTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        const bool consecutiveSnippet = m_lastTagName == QLatin1String("snippet")
            || m_lastTagName == QLatin1String("dots") || m_lastTagName == QLatin1String("codeline");
        if (consecutiveSnippet) {
            m_output.flush();
            m_output.string()->chop(2);
        }
        QString location = reader.attributes().value(QLatin1String("location")).toString();
        QString identifier = reader.attributes().value(QLatin1String("identifier")).toString();
        QString errorMessage;
        const QString pythonCode =
            readFromLocations(m_generator->codeSnippetDirs(), location, identifier, &errorMessage);
        if (!errorMessage.isEmpty())
            qCWarning(lcShibokenDoc, "%s", qPrintable(msgTagWarning(reader, m_context, m_lastTagName, errorMessage)));
        // Fall back to C++ snippet when "path" attribute is present.
        // Also read fallback snippet when comparison is desired.
        QString fallbackCode;
        if ((pythonCode.isEmpty() || snippetComparison())
            && reader.attributes().hasAttribute(fallbackPathAttribute())) {
            const QString fallback = reader.attributes().value(fallbackPathAttribute()).toString();
            if (QFileInfo::exists(fallback)) {
                if (pythonCode.isEmpty())
                    qCWarning(lcShibokenDoc, "%s", qPrintable(msgFallbackWarning(reader, m_context, m_lastTagName, location, identifier, fallback)));
                fallbackCode = readFromLocation(fallback, identifier, &errorMessage);
                if (!errorMessage.isEmpty())
                    qCWarning(lcShibokenDoc, "%s", qPrintable(msgTagWarning(reader, m_context, m_lastTagName, errorMessage)));
            }
        }

        if (!pythonCode.isEmpty() && !fallbackCode.isEmpty() && snippetComparison())
            qCDebug(lcShibokenDoc, "%s", qPrintable(msgSnippetComparison(location, identifier, pythonCode, fallbackCode)));

        if (!consecutiveSnippet)
            m_output << INDENT << "::\n\n";

        Indentation indentation(INDENT);
        const QString code = pythonCode.isEmpty() ? fallbackCode : pythonCode;
        if (code.isEmpty())
            m_output << INDENT << "<Code snippet \"" << location << ':' << identifier << "\" not found>\n";
        else
            formatSnippet(m_output, INDENT, code);
        m_output << Qt::endl;
    }
}
void QtXmlToSphinx::handleDotsTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        const bool consecutiveSnippet = m_lastTagName == QLatin1String("snippet")
            || m_lastTagName == QLatin1String("dots") || m_lastTagName == QLatin1String("codeline");
        if (consecutiveSnippet) {
            m_output.flush();
            m_output.string()->chop(2);
        } else {
            m_output << INDENT << "::\n\n";
        }
        Indentation indentation(INDENT);
        pushOutputBuffer();
        m_output << INDENT;
        int indent = reader.attributes().value(QLatin1String("indent")).toInt();
        for (int i = 0; i < indent; ++i)
            m_output << ' ';
    } else if (token == QXmlStreamReader::Characters) {
        m_output << reader.text().toString();
    } else if (token == QXmlStreamReader::EndElement) {
        m_output << popOutputBuffer() << "\n\n\n";
    }
}

void QtXmlToSphinx::handleTableTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        m_currentTable.clear();
        m_tableHasHeader = false;
    } else if (token == QXmlStreamReader::EndElement) {
        // write the table on m_output
        m_currentTable.setHeaderEnabled(m_tableHasHeader);
        m_currentTable.normalize();
        m_output << ensureEndl << m_currentTable;
        m_currentTable.clear();
    }
}

void QtXmlToSphinx::handleTermTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        pushOutputBuffer();
    } else if (token == QXmlStreamReader::Characters) {
        m_output << reader.text().toString().replace(QLatin1String("::"), QLatin1String("."));
    } else if (token == QXmlStreamReader::EndElement) {
        TableCell cell;
        cell.data = popOutputBuffer().trimmed();
        m_currentTable.appendRow(TableRow(1, cell));
    }
}


void QtXmlToSphinx::handleItemTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        if (m_currentTable.isEmpty())
            m_currentTable.appendRow({});
        TableRow& row = m_currentTable.last();
        TableCell cell;
        cell.colSpan = reader.attributes().value(QLatin1String("colspan")).toShort();
        cell.rowSpan = reader.attributes().value(QLatin1String("rowspan")).toShort();
        row << cell;
        pushOutputBuffer();
    } else if (token == QXmlStreamReader::EndElement) {
        QString data = popOutputBuffer().trimmed();
        if (!m_currentTable.isEmpty()) {
            TableRow& row = m_currentTable.last();
            if (!row.isEmpty())
                row.last().data = data;
        }
    }
}

void QtXmlToSphinx::handleRowTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        m_tableHasHeader = reader.name() == QLatin1String("header");
        m_currentTable.appendRow({});
    }
}

enum ListType { BulletList, OrderedList, EnumeratedList };

static inline ListType webXmlListType(const QStringRef &t)
{
    if (t == QLatin1String("enum"))
        return EnumeratedList;
    if (t == QLatin1String("ordered"))
        return OrderedList;
    return BulletList;
}

void QtXmlToSphinx::handleListTag(QXmlStreamReader& reader)
{
    // BUG We do not support a list inside a table cell
    static ListType listType = BulletList;
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        listType = webXmlListType(reader.attributes().value(QLatin1String("type")));
        if (listType == EnumeratedList) {
            m_currentTable.appendRow(TableRow{TableCell(QLatin1String("Constant")),
                                              TableCell(QLatin1String("Description"))});
            m_tableHasHeader = true;
        }
        INDENT.indent--;
    } else if (token == QXmlStreamReader::EndElement) {
        INDENT.indent++;
        if (!m_currentTable.isEmpty()) {
            switch (listType) {
            case BulletList:
            case OrderedList: {
                m_output << Qt::endl;
                const char *separator = listType == BulletList ? "* " : "#. ";
                const char *indent    = listType == BulletList ? "  " : "   ";
                for (const TableCell &cell : m_currentTable.constFirst()) {
                    const QVector<QStringRef> itemLines = cell.data.splitRef(QLatin1Char('\n'));
                    m_output << INDENT << separator << itemLines.constFirst() << Qt::endl;
                    for (int i = 1, max = itemLines.count(); i < max; ++i)
                        m_output << INDENT << indent << itemLines[i] << Qt::endl;
                }
                m_output << Qt::endl;
            }
                break;
            case EnumeratedList:
                m_currentTable.setHeaderEnabled(m_tableHasHeader);
                m_currentTable.normalize();
                m_output << ensureEndl << m_currentTable;
                break;
            }
        }
        m_currentTable.clear();
    }
}

void QtXmlToSphinx::handleLinkTag(QXmlStreamReader& reader)
{
    switch (reader.tokenType()) {
    case QXmlStreamReader::StartElement: {
        // <link> embedded in <see-also> means the characters of <see-also> are no link.
        m_seeAlsoContext.reset();
        const QString type = fixLinkType(reader.attributes().value(QLatin1String("type")));
        const QString ref = reader.attributes().value(linkSourceAttribute(type)).toString();
        m_linkContext.reset(handleLinkStart(type, ref));
    }
        break;
    case QXmlStreamReader::Characters:
        Q_ASSERT(!m_linkContext.isNull());
        handleLinkText(m_linkContext.data(), reader.text().toString());
        break;
    case QXmlStreamReader::EndElement:
        Q_ASSERT(!m_linkContext.isNull());
        handleLinkEnd(m_linkContext.data());
        m_linkContext.reset();
        break;
    default:
        break;
    }
}

QtXmlToSphinx::LinkContext *QtXmlToSphinx::handleLinkStart(const QString &type, QString ref) const
{
    ref.replace(QLatin1String("::"), QLatin1String("."));
    ref.remove(QLatin1String("()"));
    auto *result = new LinkContext(ref);

    if (m_insideBold)
        result->flags |= LinkContext::InsideBold;
    else if (m_insideItalic)
        result->flags |= LinkContext::InsideItalic;

    if (type == functionLinkType() && !m_context.isEmpty()) {
        result->type = LinkContext::Method;
        const QVector<QStringRef> rawlinklist = result->linkRef.splitRef(QLatin1Char('.'));
        if (rawlinklist.size() == 1 || rawlinklist.constFirst() == m_context) {
            QString context = resolveContextForMethod(rawlinklist.constLast().toString());
            if (!result->linkRef.startsWith(context))
                result->linkRef.prepend(context + QLatin1Char('.'));
        } else {
            result->linkRef = expandFunction(result->linkRef);
        }
    } else if (type == functionLinkType() && m_context.isEmpty()) {
        result->type = LinkContext::Function;
    } else if (type == classLinkType()) {
        result->type = LinkContext::Class;
        if (const TypeEntry *type = TypeDatabase::instance()->findType(result->linkRef)) {
            result->linkRef = type->qualifiedTargetLangName();
        } else { // fall back to the old heuristic if the type wasn't found.
            const QVector<QStringRef> rawlinklist = result->linkRef.splitRef(QLatin1Char('.'));
            QStringList splittedContext = m_context.split(QLatin1Char('.'));
            if (rawlinklist.size() == 1 || rawlinklist.constFirst() == splittedContext.constLast()) {
                splittedContext.removeLast();
                result->linkRef.prepend(QLatin1Char('~') + splittedContext.join(QLatin1Char('.'))
                                                 + QLatin1Char('.'));
            }
        }
    } else if (type == QLatin1String("enum")) {
        result->type = LinkContext::Attribute;
    } else if (type == QLatin1String("page")) {
        // Module, external web page or reference
        if (result->linkRef == m_generator->moduleName())
            result->type = LinkContext::Module;
        else if (result->linkRef.startsWith(QLatin1String("http")))
            result->type = LinkContext::External;
        else
            result->type = LinkContext::Reference;
    } else if (type == QLatin1String("external")) {
        result->type = LinkContext::External;
    } else {
        result->type = LinkContext::Reference;
    }
    return result;
}

// <link raw="Model/View Classes" href="model-view-programming.html#model-view-classes"
//  type="page" page="Model/View Programming">Model/View Classes</link>
// <link type="page" page="http://doc.qt.io/qt-5/class.html">QML types</link>
// <link raw="Qt Quick" href="qtquick-index.html" type="page" page="Qt Quick">Qt Quick</link>
// <link raw="QObject" href="qobject.html" type="class">QObject</link>
// <link raw="Qt::Window" href="qt.html#WindowType-enum" type="enum" enum="Qt::WindowType">Qt::Window</link>
// <link raw="QNetworkSession::reject()" href="qnetworksession.html#reject" type="function">QNetworkSession::reject()</link>

static QString fixLinkText(const QtXmlToSphinx::LinkContext *linkContext,
                           QString linktext)
{
    if (linkContext->type == QtXmlToSphinx::LinkContext::External
        || linkContext->type == QtXmlToSphinx::LinkContext::Reference) {
        return linktext;
    }
    // For the language reference documentation, strip the module name.
    // Clear the link text if that matches the function/class/enumeration name.
    const int lastSep = linktext.lastIndexOf(QLatin1String("::"));
    if (lastSep != -1)
        linktext.remove(0, lastSep + 2);
    else
         stripPythonQualifiers(&linktext);
    if (linkContext->linkRef == linktext)
        return QString();
    if ((linkContext->type & QtXmlToSphinx::LinkContext::FunctionMask) != 0
        && (linkContext->linkRef + QLatin1String("()")) == linktext) {
        return QString();
    }
    const QStringRef item = linkContext->linkRef.splitRef(QLatin1Char('.')).constLast();
    if (item == linktext)
        return QString();
    if ((linkContext->type & QtXmlToSphinx::LinkContext::FunctionMask) != 0
        && (item + QLatin1String("()")) == linktext) {
        return QString();
    }
    return  linktext;
}

void QtXmlToSphinx::handleLinkText(LinkContext *linkContext, const QString &linktext) const
{
    linkContext->linkText = fixLinkText(linkContext, linktext);
}

void QtXmlToSphinx::handleLinkEnd(LinkContext *linkContext)
{
    m_output << *linkContext;
}

// Copy images that are placed in a subdirectory "images" under the webxml files
// by qdoc to a matching subdirectory under the "rst/PySide2/<module>" directory
static bool copyImage(const QString &href, const QString &docDataDir,
                      const QString &context, const QString &outputDir,
                      QString *errorMessage)
{
    const QChar slash = QLatin1Char('/');
    const int lastSlash = href.lastIndexOf(slash);
    const QString imagePath = lastSlash != -1 ? href.left(lastSlash) : QString();
    const QString imageFileName = lastSlash != -1 ? href.right(href.size() - lastSlash - 1) : href;
    QFileInfo imageSource(docDataDir + slash + href);
    if (!imageSource.exists()) {
        QTextStream(errorMessage) << "Image " << href << " does not exist in "
            << QDir::toNativeSeparators(docDataDir);
        return false;
    }
    // Determine directory from context, "Pyside2.QtGui.QPainter" ->"Pyside2/QtGui".
    // FIXME: Not perfect yet, should have knowledge about namespaces (DataVis3D) or
    // nested classes "Pyside2.QtGui.QTouchEvent.QTouchPoint".
    QString relativeTargetDir = context;
    const int lastDot = relativeTargetDir.lastIndexOf(QLatin1Char('.'));
    if (lastDot != -1)
        relativeTargetDir.truncate(lastDot);
    relativeTargetDir.replace(QLatin1Char('.'), slash);
    if (!imagePath.isEmpty())
        relativeTargetDir += slash + imagePath;

    const QString targetDir = outputDir + slash + relativeTargetDir;
    const QString targetFileName = targetDir + slash + imageFileName;
    if (QFileInfo::exists(targetFileName))
        return true;
    if (!QFileInfo::exists(targetDir)) {
        const QDir outDir(outputDir);
        if (!outDir.mkpath(relativeTargetDir)) {
            QTextStream(errorMessage) << "Cannot create " << QDir::toNativeSeparators(relativeTargetDir)
                << " under " << QDir::toNativeSeparators(outputDir);
            return false;
        }
    }

    QFile source(imageSource.absoluteFilePath());
    if (!source.copy(targetFileName)) {
        QTextStream(errorMessage) << "Cannot copy " << QDir::toNativeSeparators(source.fileName())
            << " to " << QDir::toNativeSeparators(targetFileName) << ": "
            << source.errorString();
        return false;
    }
    qCDebug(lcShibokenDoc()).noquote().nospace() << __FUNCTION__ << " href=\""
        << href << "\", context=\"" << context << "\", docDataDir=\""
        << docDataDir << "\", outputDir=\"" << outputDir << "\", copied \""
        << source.fileName() << "\"->\"" << targetFileName << '"';
    return true;
}

bool QtXmlToSphinx::copyImage(const QString &href) const
{
    QString errorMessage;
    const bool result =
        ::copyImage(href, m_generator->docDataDir(), m_context,
                    m_generator->outputDirectory(), &errorMessage);
    if (!result)
        qCWarning(lcShibokenDoc, "%s", qPrintable(errorMessage));
    return result;
}

void QtXmlToSphinx::handleImageTag(QXmlStreamReader& reader)
{
    if (reader.tokenType() != QXmlStreamReader::StartElement)
        return;
    const QString href = reader.attributes().value(QLatin1String("href")).toString();
    if (copyImage(href))
        m_output << INDENT << ".. image:: " <<  href << Qt::endl << Qt::endl;
}

void QtXmlToSphinx::handleInlineImageTag(QXmlStreamReader& reader)
{
    if (reader.tokenType() != QXmlStreamReader::StartElement)
        return;
    const QString href = reader.attributes().value(QLatin1String("href")).toString();
    if (!copyImage(href))
        return;
    // Handle inline images by substitution references. Insert a unique tag
    // enclosed by '|' and define it further down. Determine tag from the base
    //file name with number.
    QString tag = href;
    int pos = tag.lastIndexOf(QLatin1Char('/'));
    if (pos != -1)
        tag.remove(0, pos + 1);
    pos = tag.indexOf(QLatin1Char('.'));
    if (pos != -1)
        tag.truncate(pos);
    tag += QString::number(m_inlineImages.size() + 1);
    m_inlineImages.append(InlineImage{tag, href});
    m_output << '|' << tag << '|' << ' ';
}

void QtXmlToSphinx::handleRawTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        QString format = reader.attributes().value(QLatin1String("format")).toString();
        m_output << INDENT << ".. raw:: " << format.toLower() << Qt::endl << Qt::endl;
    } else if (token == QXmlStreamReader::Characters) {
        const QVector<QStringRef> lst(reader.text().split(QLatin1Char('\n')));
        for (const QStringRef &row : lst)
            m_output << INDENT << INDENT << row << Qt::endl;
    } else if (token == QXmlStreamReader::EndElement) {
        m_output << Qt::endl << Qt::endl;
    }
}

void QtXmlToSphinx::handleCodeTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        m_output << INDENT << "::\n\n";
        INDENT.indent++;
    } else if (token == QXmlStreamReader::Characters) {
        const QVector<QStringRef> lst(reader.text().split(QLatin1Char('\n')));
        for (const QStringRef &row : lst)
            m_output << INDENT << INDENT << row << Qt::endl;
    } else if (token == QXmlStreamReader::EndElement) {
        m_output << Qt::endl << Qt::endl;
        INDENT.indent--;
    }
}

void QtXmlToSphinx::handleUnknownTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement)
        qCDebug(lcShibokenDoc).noquote().nospace() << "Unknown QtDoc tag: \"" << reader.name().toString() << "\".";
}

void QtXmlToSphinx::handleSuperScriptTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        m_output << " :sup:`";
        pushOutputBuffer();
    } else if (token == QXmlStreamReader::Characters) {
        m_output << reader.text().toString();
    } else if (token == QXmlStreamReader::EndElement) {
        m_output << popOutputBuffer();
        m_output << '`';
    }
}

void QtXmlToSphinx::handlePageTag(QXmlStreamReader &reader)
{
    if (reader.tokenType() != QXmlStreamReader::StartElement)
        return;

    const QStringRef title = reader.attributes().value(titleAttribute());
    if (!title.isEmpty())
        m_output << rstLabel(title.toString());

    const QStringRef fullTitle = reader.attributes().value(fullTitleAttribute());
    const int size = fullTitle.isEmpty()
       ? writeEscapedRstText(m_output, title)
       : writeEscapedRstText(m_output, fullTitle);

    m_output << Qt::endl << Pad('*', size) << Qt::endl << Qt::endl;
}

void QtXmlToSphinx::handleTargetTag(QXmlStreamReader &reader)
{
    if (reader.tokenType() != QXmlStreamReader::StartElement)
        return;
    const QStringRef name = reader.attributes().value(nameAttribute());
    if (!name.isEmpty())
        m_output << INDENT << rstLabel(name.toString());
}

void QtXmlToSphinx::handleIgnoredTag(QXmlStreamReader&)
{
}

void QtXmlToSphinx::handleUselessTag(QXmlStreamReader&)
{
    // Tag "description" just marks the init of "Detailed description" title.
    // Tag "definition" just marks enums. We have a different way to process them.
}

void QtXmlToSphinx::handleAnchorTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::StartElement) {
        QString anchor;
        if (reader.attributes().hasAttribute(QLatin1String("id")))
            anchor = reader.attributes().value(QLatin1String("id")).toString();
        else if (reader.attributes().hasAttribute(QLatin1String("name")))
            anchor = reader.attributes().value(QLatin1String("name")).toString();
        if (!anchor.isEmpty() && m_opened_anchor != anchor) {
            m_opened_anchor = anchor;
            if (!m_context.isEmpty())
                anchor.prepend(m_context + QLatin1Char('_'));
            m_output << INDENT << rstLabel(anchor);
        }
   } else if (token == QXmlStreamReader::EndElement) {
       m_opened_anchor.clear();
   }
}

void QtXmlToSphinx::handleRstPassTroughTag(QXmlStreamReader& reader)
{
    if (reader.tokenType() == QXmlStreamReader::Characters)
        m_output << reader.text();
}

void QtXmlToSphinx::handleQuoteFileTag(QXmlStreamReader& reader)
{
    QXmlStreamReader::TokenType token = reader.tokenType();
    if (token == QXmlStreamReader::Characters) {
        QString location = reader.text().toString();
        location.prepend(m_generator->libSourceDir() + QLatin1Char('/'));
        QString errorMessage;
        QString code = readFromLocation(location, QString(), &errorMessage);
        if (!errorMessage.isEmpty())
            qCWarning(lcShibokenDoc, "%s", qPrintable(msgTagWarning(reader, m_context, m_lastTagName, errorMessage)));
        m_output << INDENT << "::\n\n";
        Indentation indentation(INDENT);
        if (code.isEmpty())
            m_output << INDENT << "<Code snippet \"" << location << "\" not found>\n";
        else
            formatCode(m_output, code, INDENT);
        m_output << Qt::endl;
    }
}

bool QtXmlToSphinx::convertToRst(QtDocGenerator *generator,
                                 const QString &sourceFileName,
                                 const QString &targetFileName,
                                 const QString &context, QString *errorMessage)
{
    QFile sourceFile(sourceFileName);
    if (!sourceFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (errorMessage)
            *errorMessage = msgCannotOpenForReading(sourceFile);
        return false;
    }
    const QString doc = QString::fromUtf8(sourceFile.readAll());
    sourceFile.close();

    FileOut targetFile(targetFileName);
    QtXmlToSphinx x(generator, doc, context);
    targetFile.stream << x;
    return targetFile.done(errorMessage) != FileOut::Failure;
}

void QtXmlToSphinx::Table::normalize()
{
    if (m_normalized || isEmpty())
        return;

    //QDoc3 generates tables with wrong number of columns. We have to
    //check and if necessary, merge the last columns.
    int maxCols = -1;
    for (const auto &row : qAsConst(m_rows)) {
        if (row.count() > maxCols)
            maxCols = row.count();
    }
    if (maxCols <= 0)
        return;
    // add col spans
    for (int row = 0; row < m_rows.count(); ++row) {
        for (int col = 0; col < m_rows.at(row).count(); ++col) {
            QtXmlToSphinx::TableCell& cell = m_rows[row][col];
            bool mergeCols = (col >= maxCols);
            if (cell.colSpan > 0) {
                QtXmlToSphinx::TableCell newCell;
                newCell.colSpan = -1;
                for (int i = 0, max = cell.colSpan-1; i < max; ++i) {
                    m_rows[row].insert(col + 1, newCell);
                }
                cell.colSpan = 0;
                col++;
            } else if (mergeCols) {
                m_rows[row][maxCols - 1].data += QLatin1Char(' ') + cell.data;
            }
        }
    }

    // row spans
    const int numCols = m_rows.constFirst().count();
    for (int col = 0; col < numCols; ++col) {
        for (int row = 0; row < m_rows.count(); ++row) {
            if (col < m_rows[row].count()) {
                QtXmlToSphinx::TableCell& cell = m_rows[row][col];
                if (cell.rowSpan > 0) {
                    QtXmlToSphinx::TableCell newCell;
                    newCell.rowSpan = -1;
                    int targetRow = row + 1;
                    const int targetEndRow =
                        std::min(targetRow + cell.rowSpan - 1, m_rows.count());
                    cell.rowSpan = 0;
                    for ( ; targetRow < targetEndRow; ++targetRow)
                        m_rows[targetRow].insert(col, newCell);
                    row++;
                }
            }
        }
    }
    m_normalized = true;
}

QTextStream& operator<<(QTextStream& s, const QtXmlToSphinx::Table &table)
{
    table.format(s);
    return s;
}

void QtXmlToSphinx::Table::format (QTextStream& s) const
{
    if (isEmpty())
        return;

    if (!isNormalized()) {
        qCDebug(lcShibokenDoc) << "Attempt to print an unnormalized table!";
        return;
    }

    // calc width and height of each column and row
    const int headerColumnCount = m_rows.constFirst().count();
    QVector<int> colWidths(headerColumnCount);
    QVector<int> rowHeights(m_rows.count());
    for (int i = 0, maxI = m_rows.count(); i < maxI; ++i) {
        const QtXmlToSphinx::TableRow& row = m_rows.at(i);
        for (int j = 0, maxJ = std::min(row.count(), colWidths.size()); j < maxJ; ++j) {
            const QVector<QStringRef> rowLines = row[j].data.splitRef(QLatin1Char('\n')); // cache this would be a good idea
            for (const QStringRef &str : rowLines)
                colWidths[j] = std::max(colWidths[j], str.count());
            rowHeights[i] = std::max(rowHeights[i], row[j].data.count(QLatin1Char('\n')) + 1);
        }
    }

    if (!*std::max_element(colWidths.begin(), colWidths.end()))
        return; // empty table (table with empty cells)

    // create a horizontal line to be used later.
    QString horizontalLine = QLatin1String("+");
    for (int i = 0, max = colWidths.count(); i < max; ++i) {
        horizontalLine += QString(colWidths.at(i), QLatin1Char('-'));
        horizontalLine += QLatin1Char('+');
    }

    // write table rows
    for (int i = 0, maxI = m_rows.count(); i < maxI; ++i) { // for each row
        const QtXmlToSphinx::TableRow& row = m_rows.at(i);

        // print line
        s << INDENT << '+';
        for (int col = 0; col < headerColumnCount; ++col) {
            char c;
            if (col >= row.length() || row[col].rowSpan == -1)
                c = ' ';
            else if (i == 1 && hasHeader())
                c = '=';
            else
                c = '-';
            s << Pad(c, colWidths.at(col)) << '+';
        }
        s << Qt::endl;


        // Print the table cells
        for (int rowLine = 0; rowLine < rowHeights[i]; ++rowLine) { // for each line in a row
            int j = 0;
            for (int maxJ = std::min(row.count(), headerColumnCount); j < maxJ; ++j) { // for each column
                const QtXmlToSphinx::TableCell& cell = row[j];
                const QVector<QStringRef> rowLines = cell.data.splitRef(QLatin1Char('\n')); // FIXME: Cache this!!!
                if (!j) // First column, so we need print the identation
                    s << INDENT;

                if (!j || !cell.colSpan)
                    s << '|';
                else
                    s << ' ';
                if (rowLine < rowLines.count())
                    s << qSetFieldWidth(colWidths[j]) << Qt::left << rowLines.at(rowLine) << qSetFieldWidth(0);
                else
                    s << Pad(' ', colWidths.at(j));
            }
            for ( ; j < headerColumnCount; ++j) // pad
                s << '|' << Pad(' ', colWidths.at(j));
            s << "|\n";
        }
    }
    s << INDENT << horizontalLine << Qt::endl << Qt::endl;
}

static QString getFuncName(const AbstractMetaFunction* cppFunc) {
    static bool hashInitialized = false;
    static QHash<QString, QString> operatorsHash;
    if (!hashInitialized) {
        operatorsHash.insert(QLatin1String("operator+"), QLatin1String("__add__"));
        operatorsHash.insert(QLatin1String("operator+="), QLatin1String("__iadd__"));
        operatorsHash.insert(QLatin1String("operator-"), QLatin1String("__sub__"));
        operatorsHash.insert(QLatin1String("operator-="), QLatin1String("__isub__"));
        operatorsHash.insert(QLatin1String("operator*"), QLatin1String("__mul__"));
        operatorsHash.insert(QLatin1String("operator*="), QLatin1String("__imul__"));
        operatorsHash.insert(QLatin1String("operator/"), QLatin1String("__div__"));
        operatorsHash.insert(QLatin1String("operator/="), QLatin1String("__idiv__"));
        operatorsHash.insert(QLatin1String("operator%"), QLatin1String("__mod__"));
        operatorsHash.insert(QLatin1String("operator%="), QLatin1String("__imod__"));
        operatorsHash.insert(QLatin1String("operator<<"), QLatin1String("__lshift__"));
        operatorsHash.insert(QLatin1String("operator<<="), QLatin1String("__ilshift__"));
        operatorsHash.insert(QLatin1String("operator>>"), QLatin1String("__rshift__"));
        operatorsHash.insert(QLatin1String("operator>>="), QLatin1String("__irshift__"));
        operatorsHash.insert(QLatin1String("operator&"), QLatin1String("__and__"));
        operatorsHash.insert(QLatin1String("operator&="), QLatin1String("__iand__"));
        operatorsHash.insert(QLatin1String("operator|"), QLatin1String("__or__"));
        operatorsHash.insert(QLatin1String("operator|="), QLatin1String("__ior__"));
        operatorsHash.insert(QLatin1String("operator^"), QLatin1String("__xor__"));
        operatorsHash.insert(QLatin1String("operator^="), QLatin1String("__ixor__"));
        operatorsHash.insert(QLatin1String("operator=="), QLatin1String("__eq__"));
        operatorsHash.insert(QLatin1String("operator!="), QLatin1String("__ne__"));
        operatorsHash.insert(QLatin1String("operator<"), QLatin1String("__lt__"));
        operatorsHash.insert(QLatin1String("operator<="), QLatin1String("__le__"));
        operatorsHash.insert(QLatin1String("operator>"), QLatin1String("__gt__"));
        operatorsHash.insert(QLatin1String("operator>="), QLatin1String("__ge__"));
        hashInitialized = true;
    }

    QHash<QString, QString>::const_iterator it = operatorsHash.constFind(cppFunc->name());
    QString result = it != operatorsHash.cend() ? it.value() : cppFunc->name();
    result.replace(QLatin1String("::"), QLatin1String("."));
    return result;
}

QtDocGenerator::QtDocGenerator() : m_docParser(nullptr)
{
}

QtDocGenerator::~QtDocGenerator()
{
    delete m_docParser;
}

QString QtDocGenerator::fileNameSuffix() const
{
    return QLatin1String(".rst");
}

bool QtDocGenerator::shouldGenerate(const AbstractMetaClass *cls) const
{
    return Generator::shouldGenerate(cls)
        && cls->typeEntry()->type() != TypeEntry::SmartPointerType;
}

QString QtDocGenerator::fileNameForContext(const GeneratorContext &context) const
{
    const AbstractMetaClass *metaClass = context.metaClass();
    if (!context.forSmartPointer()) {
        return metaClass->name() + fileNameSuffix();
    }
    const AbstractMetaType *smartPointerType = context.preciseType();
    QString fileNameBase = getFileNameBaseForSmartPointer(smartPointerType, metaClass);
    return fileNameBase + fileNameSuffix();
}

void QtDocGenerator::writeFormattedText(QTextStream &s, const Documentation &doc,
                                        const AbstractMetaClass *metaClass,
                                        Documentation::Type docType)
{
    QString metaClassName;

    if (metaClass)
        metaClassName = metaClass->fullName();

    if (doc.format() == Documentation::Native) {
        QtXmlToSphinx x(this,doc.value(docType), metaClassName);
        s << x;
    } else {
        const QString &value = doc.value(docType);
        const QVector<QStringRef> lines = value.splitRef(QLatin1Char('\n'));
        int typesystemIndentation = std::numeric_limits<int>::max();
        bool firstLine = true;
        // check how many spaces must be removed from the beginning of each line
        // (ignore first line as that always has zero spaces at the start)
        for (const QStringRef &line : lines) {
            if (!firstLine) {
            
                const auto it = std::find_if(line.cbegin(), line.cend(),
                   [](QChar c) { return !c.isSpace(); });
                if (it != line.cend())
                    typesystemIndentation = qMin(typesystemIndentation, int(it - line.cbegin()));
           }
           firstLine = false;
        }
        if (typesystemIndentation == std::numeric_limits<int>::max())
            typesystemIndentation = 0;
        firstLine = true;
        for (const QStringRef &line : lines) {
            s << INDENT
                << (!firstLine && typesystemIndentation > 0 && typesystemIndentation < line.size()
                    ? line.right(line.size() - typesystemIndentation) : line)
                << Qt::endl;
            firstLine = false;
        }
    }

    s << Qt::endl;
}

static void writeInheritedByList(QTextStream& s, const AbstractMetaClass* metaClass, const AbstractMetaClassList& allClasses)
{
    AbstractMetaClassList res;
    for (AbstractMetaClass *c : allClasses) {
        if (c != metaClass && c->inheritsFrom(metaClass))
            res << c;
    }

    if (res.isEmpty())
        return;

    s << "**Inherited by:** ";
    QStringList classes;
    for (AbstractMetaClass *c : qAsConst(res))
        classes << QLatin1String(":ref:`") + c->name() + QLatin1Char('`');
    s << classes.join(QLatin1String(", ")) << Qt::endl << Qt::endl;
}

// Extract the <brief> section from a WebXML (class) documentation and remove it
// from the source.
static bool extractBrief(Documentation *sourceDoc, Documentation *brief)
{
    if (sourceDoc->format() != Documentation::Native)
        return false;
    QString value = sourceDoc->value();
    const int briefStart = value.indexOf(briefStartElement());
    if (briefStart < 0)
        return false;
    const int briefEnd = value.indexOf(briefEndElement(), briefStart + briefStartElement().size());
    if (briefEnd < briefStart)
        return false;
    const int briefLength = briefEnd + briefEndElement().size() - briefStart;
    brief->setFormat(Documentation::Native);
    QString briefValue = value.mid(briefStart, briefLength);
    briefValue.insert(briefValue.size() - briefEndElement().size(),
                      QLatin1String("<rst> More_...</rst>"));
    brief->setValue(briefValue);
    value.remove(briefStart, briefLength);
    sourceDoc->setValue(value);
    return true;
}

void QtDocGenerator::generateClass(QTextStream &s, const GeneratorContext &classContext)
{
    const AbstractMetaClass *metaClass = classContext.metaClass();
    qCDebug(lcShibokenDoc).noquote().nospace() << "Generating Documentation for " << metaClass->fullName();

    m_packages[metaClass->package()] << fileNameForContext(classContext);

    m_docParser->setPackageName(metaClass->package());
    m_docParser->fillDocumentation(const_cast<AbstractMetaClass*>(metaClass));

    QString className = metaClass->name();
    s << ".. _" << className << ":" << "\n\n";
    s << ".. currentmodule:: " << metaClass->package() << "\n\n\n";

    s << className << Qt::endl;
    s << Pad('*', className.count()) << Qt::endl << Qt::endl;

    auto documentation = metaClass->documentation();
    Documentation brief;
    if (extractBrief(&documentation, &brief))
        writeFormattedText(s, brief.value(), metaClass);

    s << ".. inheritance-diagram:: " << metaClass->fullName() << Qt::endl
      << "    :parts: 2" << Qt::endl << Qt::endl;
    // TODO: This would be a parameter in the future...


    writeInheritedByList(s, metaClass, classes());

    const auto version = versionOf(metaClass->typeEntry());
    if (!version.isNull())
        s << rstVersionAdded(version);
    if (metaClass->attributes().testFlag(AbstractMetaAttributes::Deprecated))
        s << rstDeprecationNote("class");

    writeFunctionList(s, metaClass);
    writePropertyList(s, metaClass);

    //Function list
    AbstractMetaFunctionList functionList = metaClass->functions();
    std::sort(functionList.begin(), functionList.end(), functionSort);

    s << "\nDetailed Description\n"
           "--------------------\n\n"
        << ".. _More:\n";

    writeInjectDocumentation(s, TypeSystem::DocModificationPrepend, metaClass, nullptr);
    if (!writeInjectDocumentation(s, TypeSystem::DocModificationReplace, metaClass, nullptr))
        writeFormattedText(s, documentation.value(), metaClass);

    if (!metaClass->isNamespace())
        writeConstructors(s, metaClass);
    writeEnums(s, metaClass);
    if (!metaClass->isNamespace())
        writeFields(s, metaClass);


    QStringList uniqueFunctions;
    for (AbstractMetaFunction *func : qAsConst(functionList)) {
        if (shouldSkip(func))
            continue;

        if (func->isStatic())
            s <<  ".. staticmethod:: ";
        else
            s <<  ".. method:: ";

        writeFunction(s, metaClass, func, !uniqueFunctions.contains(func->name()));
        uniqueFunctions.append(func->name());
    }

    for (const AddedProperty& prop: metaClass->typeEntry()->addedProperties()) {
       s << ".. attribute:: ";
       writeProperty(s, metaClass, prop);
    }

    writeInjectDocumentation(s, TypeSystem::DocModificationAppend, metaClass, nullptr);
}

void QtDocGenerator::writeFunctionList(QTextStream& s, const AbstractMetaClass* cppClass)
{
    QStringList functionList;
    QStringList virtualList;
    QStringList signalList;
    QStringList slotList;
    QStringList staticFunctionList;

    const AbstractMetaFunctionList &classFunctions = cppClass->functions();
    for (AbstractMetaFunction *func : classFunctions) {
        if (shouldSkip(func))
            continue;

        QString className;
        if (!func->isConstructor())
            className = cppClass->fullName() + QLatin1Char('.');
        else if (func->implementingClass() && func->implementingClass()->enclosingClass())
            className = func->implementingClass()->enclosingClass()->fullName() + QLatin1Char('.');
        QString funcName = getFuncName(func);

        QString str = QLatin1String("def :meth:`");

        str += funcName;
        str += QLatin1Char('<');
        if (!funcName.startsWith(className))
            str += className;
        str += funcName;
        str += QLatin1String(">` (");
        str += parseArgDocStyle(func);
        str += QLatin1Char(')');

        if (func->isStatic())
            staticFunctionList << str;
        else if (func->isVirtual())
            virtualList << str;
        else if (func->isSignal())
            signalList << str;
        else if (func->isSlot())
            slotList << str;
        else
            functionList << str;
    }

    if (!functionList.isEmpty() || !staticFunctionList.isEmpty()) {
        QtXmlToSphinx::Table functionTable;

        s << "\nSynopsis\n--------\n\n";

        writeFunctionBlock(s, QLatin1String("Functions"), functionList);
        writeFunctionBlock(s, QLatin1String("Virtual functions"), virtualList);
        writeFunctionBlock(s, QLatin1String("Slots"), slotList);
        writeFunctionBlock(s, QLatin1String("Signals"), signalList);
        writeFunctionBlock(s, QLatin1String("Static functions"), staticFunctionList);
    }
}

void QtDocGenerator::writePropertyList(QTextStream& s, const AbstractMetaClass* cppClass)
{
    AddedPropertyList props = cppClass->typeEntry()->addedProperties();
    if (props.isEmpty()) {
        return;
    }
    QtXmlToSphinx::Table propertyTable;
    QtXmlToSphinx::TableRow row;
    QStringList propList;
    for(const AddedProperty & prop: props)
    {
        QString propStr = QStringLiteral("property :attr:`%1<%2>` [%3] of ")
                .arg(prop.name())
                .arg(QStringLiteral("%1.%2").arg(cppClass->qualifiedCppName())
                                     .arg(prop.name()))
                .arg(prop.access() == AddedProperty::ReadWrite ?
                     QStringLiteral("read-write") : QStringLiteral("read-only"));
        QString scalarType = prop.scalarType(),
                classType  = prop.classType();
        if (!scalarType.isEmpty()) {
            propStr += scalarType;
        }
        else if (!classType.isEmpty()) {
            propStr += QStringLiteral(":class:`%1`").arg(classType);
        }
        else {
            propStr += QStringLiteral("unknown type");
        }
        propList << propStr;
    }
    qSort(propList);
    s << "Properties" << Qt::endl << "^^^^^^^^^^" << Qt::endl;
    s << ".. container:: property_list" << Qt::endl << Qt::endl;
    {
        Indentation indentation(INDENT);
        for(const QString & prop: propList)
        {
            s << "*" << INDENT << prop << Qt::endl;
        }
        s << Qt::endl << Qt::endl;
    }
}

void QtDocGenerator::writeFunctionBlock(QTextStream& s, const QString& title, QStringList& functions)
{
    if (!functions.isEmpty()) {
        s << title << Qt::endl
          << QString(title.size(), QLatin1Char('^')) << Qt::endl;

        std::sort(functions.begin(), functions.end());

        s << ".. container:: function_list\n\n";
        Indentation indentation(INDENT);
        for (const QString &func : qAsConst(functions))
            s << INDENT << '*' << ' ' << func << Qt::endl;

        s << Qt::endl << Qt::endl;
    }
}

void QtDocGenerator::writeEnums(QTextStream& s, const AbstractMetaClass* cppClass)
{
    static const QString section_title = QLatin1String(".. attribute:: ");

    const AbstractMetaEnumList &enums = cppClass->enums();
    for (AbstractMetaEnum *en : enums) {
        s << section_title << cppClass->fullName() << '.' << en->name() << Qt::endl << Qt::endl;
        writeFormattedText(s, en->documentation().value(), cppClass);
        const auto version = versionOf(en->typeEntry());
        if (!version.isNull())
            s << rstVersionAdded(version);
    }

}

void QtDocGenerator::writeFields(QTextStream& s, const AbstractMetaClass* cppClass)
{
    static const QString section_title = QLatin1String(".. attribute:: ");

    const AbstractMetaFieldList &fields = cppClass->fields();
    for (AbstractMetaField *field : fields) {
        s << section_title << cppClass->fullName() << "." << field->name() << Qt::endl << Qt::endl;
        //TODO: request for member ‘documentation’ is ambiguous
        writeFormattedText(s, field->AbstractMetaAttributes::documentation().value(), cppClass);
    }
}

void QtDocGenerator::writeConstructors(QTextStream& s, const AbstractMetaClass* cppClass)
{
    static const QString sectionTitle = QLatin1String(".. class:: ");

    AbstractMetaFunctionList lst = cppClass->queryFunctions(AbstractMetaClass::Constructors | AbstractMetaClass::Visible);
    for (int i = lst.size() - 1; i >= 0; --i) {
        if (lst.at(i)->isModifiedRemoved() || lst.at(i)->functionType() == AbstractMetaFunction::MoveConstructorFunction)
            lst.removeAt(i);
    }

    bool first = true;
    QHash<QString, AbstractMetaArgument*> arg_map;

    IndentorBase<1> indent1;
    indent1.indent = INDENT.total();
    if (lst.isEmpty()) {
        s << sectionTitle << cppClass->fullName();
    } else {
        for (AbstractMetaFunction *func : qAsConst(lst)) {
            s << indent1;
            if (first) {
                first = false;
                s << sectionTitle;
                indent1.indent += sectionTitle.size();
            }
            s << functionSignature(cppClass, func) << "\n\n";
            const auto version = versionOf(func->typeEntry());
            if (!version.isNull())
                s << indent1 << rstVersionAdded(version);
            if (func->attributes().testFlag(AbstractMetaAttributes::Deprecated))
                s << indent1 << rstDeprecationNote("constructor");

            const AbstractMetaArgumentList &arguments = func->arguments();
            for (AbstractMetaArgument *arg : arguments) {
                if (!arg_map.contains(arg->name())) {
                    arg_map.insert(arg->name(), arg);
                }
            }
        }
    }

    s << Qt::endl;

    for (QHash<QString, AbstractMetaArgument*>::const_iterator it = arg_map.cbegin(), end = arg_map.cend(); it != end; ++it) {
        Indentation indentation(INDENT, 2);
        writeParameterType(s, cppClass, it.value());
    }

    s << Qt::endl;

    for (AbstractMetaFunction *func : qAsConst(lst))
        writeFormattedText(s, func->documentation().value(), cppClass);
}

QString QtDocGenerator::parseArgDocStyle(const AbstractMetaFunction* func)
{
    QString ret;
    int optArgs = 0;

    const AbstractMetaArgumentList &arguments = func->arguments();
    for (AbstractMetaArgument *arg : arguments) {

        if (func->argumentRemoved(arg->argumentIndex() + 1))
            continue;

        bool thisIsoptional = !arg->defaultValueExpression().isEmpty();
        if (optArgs || thisIsoptional) {
            ret += QLatin1Char('[');
            optArgs++;
        }

        if (arg->argumentIndex() > 0)
            ret += QLatin1String(", ");

        ret += arg->name();

        if (thisIsoptional) {
            QString defValue = arg->defaultValueExpression();
            if (defValue == QLatin1String("QString()")) {
                defValue = QLatin1String("\"\"");
            } else if (defValue == QLatin1String("QStringList()")
                       || defValue.startsWith(QLatin1String("QVector"))
                       || defValue.startsWith(QLatin1String("QList"))) {
                defValue = QLatin1String("list()");
            } else if (defValue == QLatin1String("QVariant()")) {
                defValue = none();
            } else {
                defValue.replace(QLatin1String("::"), QLatin1String("."));
                if (defValue == QLatin1String("nullptr"))
                    defValue = none();
                else if (defValue == QLatin1String("0") && arg->type()->isObject())
                    defValue = none();
            }
            ret += QLatin1Char('=') + defValue;
        }
    }

    ret += QString(optArgs, QLatin1Char(']'));
    return ret;
}

void QtDocGenerator::writeDocSnips(QTextStream &s,
                                 const CodeSnipList &codeSnips,
                                 TypeSystem::CodeSnipPosition position,
                                 TypeSystem::Language language)
{
    Indentation indentation(INDENT);
    QStringList invalidStrings;
    const static QString startMarkup = QLatin1String("[sphinx-begin]");
    const static QString endMarkup = QLatin1String("[sphinx-end]");

    invalidStrings << QLatin1String("*") << QLatin1String("//") << QLatin1String("/*") << QLatin1String("*/");

    for (const CodeSnip &snip : codeSnips) {
        if ((snip.position != position) ||
            !(snip.language & language))
            continue;

        QString code = snip.code();
        while (code.contains(startMarkup) && code.contains(endMarkup)) {
            int startBlock = code.indexOf(startMarkup) + startMarkup.size();
            int endBlock = code.indexOf(endMarkup);

            if ((startBlock == -1) || (endBlock == -1))
                break;

            QString codeBlock = code.mid(startBlock, endBlock - startBlock);
            const QStringList rows = codeBlock.split(QLatin1Char('\n'));
            int currentRow = 0;
            int offset = 0;

            for (QString row : rows) {
                for (const QString &invalidString : qAsConst(invalidStrings))
                    row.remove(invalidString);

                if (row.trimmed().size() == 0) {
                    if (currentRow == 0)
                        continue;
                    s << Qt::endl;
                }

                if (currentRow == 0) {
                    //find offset
                    for (auto c : row) {
                        if (c == QLatin1Char(' '))
                            offset++;
                        else if (c == QLatin1Char('\n'))
                            offset = 0;
                        else
                            break;
                    }
                }
                s << row.midRef(offset) << Qt::endl;
                currentRow++;
            }

            code = code.mid(endBlock+endMarkup.size());
        }
    }
}

bool QtDocGenerator::writeInjectDocumentation(QTextStream& s,
                                            TypeSystem::DocModificationMode mode,
                                            const AbstractMetaClass* cppClass,
                                            const AbstractMetaFunction* func)
{
    Indentation indentation(INDENT);
    bool didSomething = false;

    const DocModificationList &mods = cppClass->typeEntry()->docModifications();
    for (const DocModification &mod : mods) {
        if (mod.mode() == mode) {
            bool modOk = func ? mod.signature() == func->minimalSignature() : mod.signature().isEmpty();

            if (modOk) {
                Documentation doc;
                Documentation::Format fmt;

                if (mod.format() == TypeSystem::NativeCode)
                    fmt = Documentation::Native;
                else if (mod.format() == TypeSystem::TargetLangCode)
                    fmt = Documentation::Target;
                else
                    continue;

                doc.setValue(mod.code(), Documentation::Detailed, fmt);
                writeFormattedText(s, doc.value(), cppClass);
                didSomething = true;
            }
        }
    }

    s << Qt::endl;

    // TODO: Deprecate the use of doc string on glue code.
    //       This is pre "add-function" and "inject-documentation" tags.
    const TypeSystem::CodeSnipPosition pos = mode == TypeSystem::DocModificationPrepend
        ? TypeSystem::CodeSnipPositionBeginning : TypeSystem::CodeSnipPositionEnd;
    if (func)
        writeDocSnips(s, func->injectedCodeSnips(), pos, TypeSystem::TargetLangCode);
    else
        writeDocSnips(s, cppClass->typeEntry()->codeSnips(), pos, TypeSystem::TargetLangCode);
    return didSomething;
}

QString QtDocGenerator::functionSignature(const AbstractMetaClass* cppClass, const AbstractMetaFunction* func)
{
    QString funcName;

    funcName = cppClass->fullName();
    if (!func->isConstructor())
        funcName += QLatin1Char('.') + getFuncName(func);

    return funcName + QLatin1Char('(') + parseArgDocStyle(func)
        + QLatin1Char(')');
}

QString QtDocGenerator::translateToPythonType(const AbstractMetaType* type, const AbstractMetaClass* cppClass)
{
    static const QStringList nativeTypes = {boolT(), floatT(), intT(),
        QLatin1String("object"),
        QLatin1String("str")
    };
    const QString name = type->name();
    if (nativeTypes.contains(name))
        return name;

    static const QMap<QString, QString> typeMap = {
        { QLatin1String("PyObject"), QLatin1String("object") },
        { QLatin1String("QString"), QLatin1String("str") },
        { QLatin1String("uchar"), QLatin1String("str") },
        { QLatin1String("QStringList"), QLatin1String("list of strings") },
        { qVariantT(), QLatin1String("object") },
        { QLatin1String("quint32"), intT() },
        { QLatin1String("uint32_t"), intT() },
        { QLatin1String("quint64"), intT() },
        { QLatin1String("qint64"), intT() },
        { QLatin1String("size_t"), intT() },
        { QLatin1String("int64_t"), intT() },
        { QLatin1String("qreal"), floatT() }
    };
    const auto found = typeMap.find(name);
    if (found != typeMap.end())
        return found.value();

    QString strType;
    if (type->isConstant() && name == QLatin1String("char") && type->indirections() == 1) {
        strType = QLatin1String("str");
    } else if (name.startsWith(QLatin1String("unsigned short"))) {
        strType = QLatin1String("int");
    } else if (name.startsWith(QLatin1String("unsigned "))) { // uint and ulong
        strType = QLatin1String("long");
    } else if (name == QLatin1String("int") || name == QLatin1String("uint") || 
               name == QLatin1String("float") || name == QLatin1String("double") ||
               name == QLatin1String("bool")) {
        strType = name;
    } else if (type->isContainer()) {
        strType = translateType(type, cppClass, Options(ExcludeConst) | ExcludeReference);
        strType.replace(QLatin1String(" "), QLatin1String(""));
        strType.remove(QLatin1Char('*'));
        strType.remove(QLatin1Char('>'));
        strType.remove(QLatin1Char('<'));
        strType.replace(QLatin1String("::"), QLatin1String("."));
        if (strType.startsWith(QLatin1String(".")))
            strType.remove(0, 1);
        if (strType.contains(QLatin1String("QList")) || strType.contains(QLatin1String("QVector"))) {
            strType.replace(QLatin1String("QList"), QLatin1String("list of "));
            strType.replace(QLatin1String("QVector"), QLatin1String("list of "));
        } else if (strType.contains(QLatin1String("QHash")) || strType.contains(QLatin1String("QMap"))) {
            strType.remove(QLatin1String("QHash"));
            strType.remove(QLatin1String("QMap"));
            QStringList types = strType.split(QLatin1Char(','));
            strType = QString::fromLatin1("Dictionary with keys of type %1 and values of type %2.")
                                         .arg(types[0], types[1]);
        } else if (strType.contains(QLatin1String("QPair"))) {
            strType.remove(QLatin1String("QPair"));
            QStringList types = strType.split(QLatin1Char(','));
            strType = QString::fromLatin1("2-items container of {%1, %2}").arg(types[0]).arg(types[1]);
        }
    } else {
        const AbstractMetaClass *k = AbstractMetaClass::findClass(classes(), type->typeEntry());
        strType = k ? k->fullName() : type->name();
        strType = QStringLiteral(":any:`") + strType + QLatin1Char('`');
    }
    return strType;
}

void QtDocGenerator::writeParameterType(QTextStream& s, const AbstractMetaClass* cppClass, const AbstractMetaArgument* arg)
{
    s << INDENT << ":type " << arg->name() << ": "
      << translateToPythonType(arg->type(), cppClass) << Qt::endl;
}

void QtDocGenerator::writeFunctionParametersType(QTextStream &s, const AbstractMetaClass *cppClass,
                                                 const AbstractMetaFunction *func)
{
    s << Qt::endl;
    const AbstractMetaArgumentList &funcArgs = func->arguments();
    for (AbstractMetaArgument *arg : funcArgs) {

        if (func->argumentRemoved(arg->argumentIndex() + 1))
            continue;

        writeParameterType(s, cppClass, arg);
    }

    if (!func->isConstructor() && !func->isVoid()) {

        QString retType;
        // check if the return type was modified
        const FunctionModificationList &mods = func->modifications();
        for (const FunctionModification &mod : mods) {
            for (const ArgumentModification &argMod : mod.argument_mods) {
                if (argMod.index == 0) {
                    retType = argMod.modified_type;
                    break;
                }
            }
        }

        if (retType.isEmpty())
            retType = translateToPythonType(func->type(), cppClass);
        s << INDENT << ":rtype: " << retType << Qt::endl;
    }
    s << Qt::endl;
}

void QtDocGenerator::writeFunction(QTextStream& s, const AbstractMetaClass* cppClass,
                                   const AbstractMetaFunction* func, bool indexed)
{
    s << functionSignature(cppClass, func);

    {
        Indentation indentation(INDENT);
        if (!indexed)
            s << QLatin1Char('\n') << INDENT << QLatin1String(":noindex:");
        s << "\n\n";
        writeFunctionParametersType(s, cppClass, func);
        const auto version = versionOf(func->typeEntry());
        if (!version.isNull())
            s << INDENT << rstVersionAdded(version);
        if (func->attributes().testFlag(AbstractMetaAttributes::Deprecated))
            s << INDENT << rstDeprecationNote("function");
    }
    writeInjectDocumentation(s, TypeSystem::DocModificationPrepend, cppClass, func);
    if (!writeInjectDocumentation(s, TypeSystem::DocModificationReplace, cppClass, func)) {
        writeFormattedText(s, func->documentation(), cppClass, Documentation::Brief);
        writeFormattedText(s, func->documentation(), cppClass, Documentation::Detailed);
    }
    writeInjectDocumentation(s, TypeSystem::DocModificationAppend, cppClass, func);
}

namespace
{
class FormatedTextWriter: public std::binary_function<Documentation,
        QtDocGenerator*, void>
{
public:
    FormatedTextWriter(QTextStream& s, const AbstractMetaClass* cppClass) :
            m_s(s), m_cppClass(cppClass)
    {
    }
    void operator()(Documentation doc, QtDocGenerator* generator) const
    {
        generator->writeFormattedText(m_s, doc, m_cppClass);
    }
private:
    QTextStream& m_s;
    const AbstractMetaClass* m_cppClass;
};
}
void QtDocGenerator::writeProperty(QTextStream& s,
        const AbstractMetaClass* cppClass, const AddedProperty& prop)
{
    s << QStringLiteral("%1.%2").arg(cppClass->qualifiedCppName()).arg(prop.name()) << Qt::endl << Qt::endl << Qt::endl;
    {
        Indentation indentation(INDENT);
        s << INDENT << ":type: ";
        QString scalarType = prop.scalarType(), classType = prop.classType();
        if (!scalarType.isEmpty()) {
            s << scalarType;
        }
        else if (!classType.isEmpty()) {
            s << QStringLiteral(":class:`%1.%2`").arg(cppClass->package()).arg(classType);
        }
        else {
            s << "unknown type";
        }
        s << Qt::endl;
        s << INDENT << ":access: " << (prop.access() == AddedProperty::ReadWrite ? "read-write" : "read-only");
        std::vector<Documentation> prependDocs, appendDocs, replaceDocs;
        for (DocModification mod : cppClass->typeEntry()->docModifications()) {
            // TODO: add property mark to property signature
            if (mod.signature() != prop.name())
                continue;
            Documentation doc;
            Documentation::Format fmt;
            if (mod.format() == TypeSystem::NativeCode)
                fmt = Documentation::Native;
            else if (mod.format() == TypeSystem::TargetLangCode)
                fmt = Documentation::Target;
            else
                continue;
            doc.setValue(mod.code(), Documentation::Detailed, fmt);
            switch (mod.mode())
            {
            case TypeSystem::DocModificationAppend:
                appendDocs.push_back(doc);
                break;
            case TypeSystem::DocModificationPrepend:
                prependDocs.push_back(doc);
                break;
            case TypeSystem::DocModificationReplace:
                replaceDocs.push_back(doc);
                break;
            default:
                break;
            }
        }
        if (replaceDocs.size()) {
            std::for_each(replaceDocs.begin(), replaceDocs.end(), std::bind2nd(FormatedTextWriter(s, cppClass), this));
        }
        else {
            std::for_each(appendDocs.begin(), appendDocs.end(), std::bind2nd(FormatedTextWriter(s, cppClass), this));
            std::for_each(prependDocs.begin(), prependDocs.end(), std::bind2nd(FormatedTextWriter(s, cppClass), this));
        }
    }
    s << Qt::endl << Qt::endl;
}

static void writeFancyToc(QTextStream& s, const QStringList& items, int cols = 4)
{
    using TocMap = QMap<QChar, QStringList>;
    TocMap tocMap;
    QChar Q = QLatin1Char('Q');
    QChar idx;
    for (QString item : items) {
        if (item.isEmpty())
            continue;
        item.chop(4); // Remove the .rst extension
        // skip namespace if necessary
        const QString className = item.split(QLatin1Char('.')).last();
        if (className.startsWith(Q) && className.length() > 1)
            idx = className[1];
        else
            idx = className[0];
        tocMap[idx] << item;
    }
    QtXmlToSphinx::Table table;
    QtXmlToSphinx::TableRow row;

    int itemsPerCol = (items.size() + tocMap.size()*2) / cols;
    QString currentColData;
    int i = 0;
    QTextStream ss(&currentColData);
    QMutableMapIterator<QChar, QStringList> it(tocMap);
    while (it.hasNext()) {
        it.next();
        std::sort(it.value().begin(), it.value().end());

        if (i)
            ss << Qt::endl;

        ss << "**" << it.key() << "**\n\n";
        i += 2; // a letter title is equivalent to two entries in space
        for (const QString &item : qAsConst(it.value())) {
            ss << "* :doc:`" << item << "`\n";
            ++i;

            // end of column detected!
            if (i > itemsPerCol) {
                ss.flush();
                QtXmlToSphinx::TableCell cell(currentColData);
                row << cell;
                currentColData.clear();
                i = 0;
            }
        }
    }
    if (i) {
        ss.flush();
        QtXmlToSphinx::TableCell cell(currentColData);
        row << cell;
        currentColData.clear();
        i = 0;
    }
    table.appendRow(row);
    table.normalize();
    s << ".. container:: pysidetoc\n\n";
    s << table;
}

bool QtDocGenerator::finishGeneration()
{
    if (!classes().isEmpty())
        writeModuleDocumentation();
    if (!m_additionalDocumentationList.isEmpty())
        writeAdditionalDocumentation();
    return true;
}

void QtDocGenerator::writeModuleDocumentation()
{
    QMap<QString, QStringList>::iterator it = m_packages.begin();
    for (; it != m_packages.end(); ++it) {
        QString typesystem = it.key();
        QString typesystemDir = typesystem;
        typesystemDir.replace(QLatin1Char('.'), QLatin1Char('/'));
        QString outputDir = outputDirectory() + QLatin1Char('/') + typesystemDir;
        FileOut output(outputDir + QLatin1String("/index.rst"));
        QTextStream& s = output.stream;

        s << ".. module:: " << it.key() << Qt::endl << Qt::endl;

        const QString &title = it.key();
        s << title << Qt::endl;
        s << Pad('*', title.length()) << Qt::endl << Qt::endl;

        /* Avoid showing "Detailed Description for *every* class in toc tree */
        Indentation indentation(INDENT);
        // Store the it.key() in a QString so that it can be stripped off unwanted
        // information when neeeded. For example, the RST files in the extras directory
        // doesn't include the PySide# prefix in their names.
        const QString moduleName = it.key();
        const int lastIndex = moduleName.lastIndexOf(QLatin1Char('.'));

        const TypeSystemTypeEntry* typesystemEntry = TypeDatabase::instance()->findTypeSystemType(typesystem);
        if (typesystemEntry)
        {
           std::vector<Documentation> prependDocs, appendDocs, replaceDocs;
           for (DocModification mod : typesystemEntry->docModifications())
           {
              if (!mod.signature().isEmpty())
                 continue;
              Documentation doc;
              Documentation::Format fmt;
              if (mod.format() == TypeSystem::NativeCode)
                 fmt = Documentation::Native;
              else if (mod.format() == TypeSystem::TargetLangCode)
                 fmt = Documentation::Target;
              else
                 continue;
              doc.setValue(mod.code(), Documentation::Detailed, fmt);
              switch (mod.mode())
              {
              case TypeSystem::DocModificationAppend:
                 appendDocs.push_back(doc);
                 break;
              case TypeSystem::DocModificationPrepend:
                 prependDocs.push_back(doc);
                 break;
              case TypeSystem::DocModificationReplace:
                 replaceDocs.push_back(doc);
                 break;
              default:
                 break;
              }
           }
           if (replaceDocs.size()) {
              std::for_each(replaceDocs.begin(), replaceDocs.end(),
                 std::bind2nd(FormatedTextWriter(s, NULL), this));
           }
           else {
              std::for_each(appendDocs.begin(), appendDocs.end(),
                 std::bind2nd(FormatedTextWriter(s, NULL), this));
              std::for_each(prependDocs.begin(), prependDocs.end(),
                 std::bind2nd(FormatedTextWriter(s, NULL), this));
           }
        }

        // Search for extra-sections
        if (!m_extraSectionDir.isEmpty()) {
            QDir extraSectionDir(m_extraSectionDir);
            if (!extraSectionDir.exists())
                qCWarning(lcShibokenDoc) << m_extraSectionDir << "doesn't exist";

            QStringList fileList = extraSectionDir.entryList(QStringList() << (moduleName.mid(lastIndex + 1) + QLatin1String("?*.rst")), QDir::Files);
            QStringList::iterator it2 = fileList.begin();
            for (; it2 != fileList.end(); ++it2) {
                QString origFileName(*it2);
                it2->remove(0, moduleName.indexOf(QLatin1Char('.')));
                QString newFilePath = outputDir + QLatin1Char('/') + *it2;
                if (QFile::exists(newFilePath))
                    QFile::remove(newFilePath);
                if (!QFile::copy(m_extraSectionDir + QLatin1Char('/') + origFileName, newFilePath)) {
                    qCDebug(lcShibokenDoc).noquote().nospace() << "Error copying extra doc "
                        << QDir::toNativeSeparators(m_extraSectionDir + QLatin1Char('/') + origFileName)
                        << " to " << QDir::toNativeSeparators(newFilePath);
                }
            }
            it.value().append(fileList);
        }

//        writeFancyToc(s, it.value());

        // Writing global functions
        // Only list functions that have function details specified. This stops us repeating
        // all global functions in every package's Global functions section
        
        AbstractMetaFunctionList allGlobalFuncs = globalFunctions();
        AbstractMetaFunctionList globalFuncs;
        QMap<AbstractMetaFunction *, DocModification>  functionModification;
        for (auto function : allGlobalFuncs)
        {
            if (shouldSkip(function))
                continue;
            for (auto mod : typesystemEntry->docModifications())
            {
                if (mod.signature() == function->minimalSignature())
                {
                    globalFuncs << function;
                    functionModification[function] = mod;
                    break;
                }
            }
        }

        if (!globalFuncs.isEmpty())
        {
            FileOut foutput(outputDir + QStringLiteral("/GlobalFunctions.rst"));
            QTextStream& fs = foutput.stream;
            // Header
            fs << ".. module:: " << it.key() << endl << endl;
            fs << "Global functions" << endl << "****************" << endl
                    << endl;
            fs << ".. container:: function_list" << endl << endl;
            {
                // Function list
                foreach (AbstractMetaFunction* function, globalFuncs)
                {
                    QString funcName = QStringLiteral("%1.%2").arg(it.key()).arg(
                            getFuncName(function));
                    fs << "*" << INDENT << ":func:`" << funcName << "`" << endl;
                }
            }
            fs << endl << endl;
            // Detailed description
            fs << "Detailed Description" << endl << "--------------------"
               << endl << endl;

            // Function details
            for (auto function : globalFuncs)
            {
                QString funcName = QStringLiteral("%1.%2").arg(it.key()).arg(
                        getFuncName(function));
                fs << ".. function:: " << funcName << "("
                        << parseArgDocStyle(function) << ")" << endl;
                writeFunctionParametersType(fs, NULL, function);
                fs << endl;
                const DocModification & mod = functionModification[function];

                Documentation doc;
                Documentation::Format fmt;
                if (mod.format() == TypeSystem::NativeCode)
                    fmt = Documentation::Native;
                else if (mod.format() == TypeSystem::TargetLangCode)
                    fmt = Documentation::Target;
                else
                    continue;
                Indentation indentation(INDENT);
                doc.setValue(mod.code(), Documentation::Detailed, fmt);
                writeFormattedText(fs, doc, NULL);
            }
        }

        s << INDENT << ".. container:: classes" << endl << endl;
        {
            Indentation indentation(INDENT);
            s << INDENT << ".. toctree::" << Qt::endl;
            Indentation deeperIndentation(INDENT);
            s << INDENT << ":maxdepth: 1" << endl << Qt::endl;
            if (!globalFuncs.isEmpty()) {
               s << INDENT << "GlobalFunctions.rst" << Qt::endl;
            }
            QStringList classes = it.value();
            qSort(classes);
            for (const QString &className : qAsConst(classes))
                s << INDENT << className << Qt::endl;
            s << Qt::endl << Qt::endl;
        }

        s << "Detailed Description\n--------------------\n\n";

        // module doc is always wrong and C++istic, so go straight to the extra directory!
        QFile moduleDoc(m_extraSectionDir + QLatin1Char('/') + moduleName.mid(lastIndex + 1) + QLatin1String(".rst"));
        if (moduleDoc.open(QIODevice::ReadOnly | QIODevice::Text)) {
            s << moduleDoc.readAll();
            moduleDoc.close();
        } else {
            // try the normal way
            Documentation moduleDoc = m_docParser->retrieveModuleDocumentation(it.key());
            if (moduleDoc.format() == Documentation::Native) {
                QString context = it.key();
                stripPythonQualifiers(&context);
                QtXmlToSphinx x(this, moduleDoc.value(), context);
                s << x;
            } else {
                s << moduleDoc.value();
            }
        }
    }
}

static inline QString msgNonExistentAdditionalDocFile(const QString &dir,
                                                      const QString &fileName)
{
    const QString result = QLatin1Char('"') + fileName
        + QLatin1String("\" does not exist in ")
        + QDir::toNativeSeparators(dir) + QLatin1Char('.');
    return result;
}

void QtDocGenerator::writeAdditionalDocumentation()
{
    QFile additionalDocumentationFile(m_additionalDocumentationList);
    if (!additionalDocumentationFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(lcShibokenDoc, "%s",
                  qPrintable(msgCannotOpenForReading(additionalDocumentationFile)));
        return;
    }

    QDir outDir(outputDirectory());
    const QString rstSuffix = fileNameSuffix();

    QString errorMessage;
    int successCount = 0;
    int count = 0;

    QString targetDir = outDir.absolutePath();

    while (!additionalDocumentationFile.atEnd()) {
        const QByteArray lineBA = additionalDocumentationFile.readLine().trimmed();
        if (lineBA.isEmpty() || lineBA.startsWith('#'))
            continue;
        const QString line = QFile::decodeName(lineBA);
        // Parse "[directory]" specification
        if (line.size() > 2 && line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
            const QString dir = line.mid(1, line.size() - 2);
            if (dir.isEmpty() || dir == QLatin1String(".")) {
                targetDir = outDir.absolutePath();
            } else {
                if (!outDir.exists(dir) && !outDir.mkdir(dir)) {
                    qCWarning(lcShibokenDoc, "Cannot create directory %s under %s",
                              qPrintable(dir),
                              qPrintable(QDir::toNativeSeparators(outputDirectory())));
                    break;
                }
                targetDir = outDir.absoluteFilePath(dir);
            }
        } else {
            // Normal file entry
            QFileInfo fi(m_docDataDir + QLatin1Char('/') + line);
            if (fi.isFile()) {
                const QString rstFileName = fi.baseName() + rstSuffix;
                const QString rstFile = targetDir + QLatin1Char('/') + rstFileName;
                const QString context = targetDir.mid(targetDir.lastIndexOf(QLatin1Char('/')) + 1);
                if (QtXmlToSphinx::convertToRst(this, fi.absoluteFilePath(),
                                                rstFile, context, &errorMessage)) {
                    ++successCount;
                    qCDebug(lcShibokenDoc).nospace().noquote() << __FUNCTION__
                        << " converted " << fi.fileName()
                        << ' ' << rstFileName;
                } else {
                    qCWarning(lcShibokenDoc, "%s", qPrintable(errorMessage));
                }
            } else {
                qCWarning(lcShibokenDoc, "%s",
                          qPrintable(msgNonExistentAdditionalDocFile(m_docDataDir, line)));
            }
            ++count;
        }
    }
    additionalDocumentationFile.close();

    qCInfo(lcShibokenDoc, "Created %d/%d additional documentation files.",
           successCount, count);
}

#ifdef __WIN32__
#   define PATH_SEP ';'
#else
#   define PATH_SEP ':'
#endif

bool QtDocGenerator::doSetup()
{
    if (m_codeSnippetDirs.isEmpty())
        m_codeSnippetDirs = m_libSourceDir.split(QLatin1Char(PATH_SEP));

    if (!m_docParser)
        m_docParser = new QtDocParser;

    if (m_libSourceDir.isEmpty() || m_docDataDir.isEmpty()) {
        qCWarning(lcShibokenDoc) << "Documentation data dir and/or Qt source dir not informed, "
                                 "documentation will not be extracted from Qt sources.";
        return false;
    }

    m_docParser->setDocumentationDataDirectory(m_docDataDir);
    m_docParser->setLibrarySourceDirectory(m_libSourceDir);
    return true;
}


Generator::OptionDescriptions QtDocGenerator::options() const
{
    return OptionDescriptions()
        << qMakePair(QLatin1String("doc-parser=<parser>"),
                     QLatin1String("The documentation parser used to interpret the documentation\n"
                                   "input files (qdoc|doxygen)"))
        << qMakePair(QLatin1String("documentation-code-snippets-dir=<dir>"),
                     QLatin1String("Directory used to search code snippets used by the documentation"))
        << qMakePair(QLatin1String("documentation-data-dir=<dir>"),
                     QLatin1String("Directory with XML files generated by documentation tool"))
        << qMakePair(QLatin1String("documentation-extra-sections-dir=<dir>"),
                     QLatin1String("Directory used to search for extra documentation sections"))
        << qMakePair(QLatin1String("library-source-dir=<dir>"),
                     QLatin1String("Directory where library source code is located"))
        << qMakePair(additionalDocumentationOption() + QLatin1String("=<file>"),
                     QLatin1String("List of additional XML files to be converted to .rst files\n"
                                   "(for example, tutorials)."));
}

bool QtDocGenerator::handleOption(const QString &key, const QString &value)
{
    if (key == QLatin1String("library-source-dir")) {
        m_libSourceDir = value;
        return true;
    }
    if (key == QLatin1String("documentation-data-dir")) {
        m_docDataDir = value;
        return true;
    }
    if (key == QLatin1String("documentation-code-snippets-dir")) {
        m_codeSnippetDirs = value.split(QLatin1Char(PATH_SEP));
        return true;
    }
    if (key == QLatin1String("documentation-extra-sections-dir")) {
        m_extraSectionDir = value;
        return true;
    }
    if (key == QLatin1String("doc-parser")) {
        qCDebug(lcShibokenDoc).noquote().nospace() << "doc-parser: " << value;
        if (value == QLatin1String("doxygen"))
            m_docParser = new DoxygenParser;
        return true;
    }
    if (key == additionalDocumentationOption()) {
        m_additionalDocumentationList = value;
        return true;
    }
    return false;
}
