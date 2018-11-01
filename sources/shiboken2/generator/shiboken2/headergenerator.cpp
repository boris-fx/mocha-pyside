/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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

#include "headergenerator.h"
#include <abstractmetalang.h>
#include <typedatabase.h>
#include <reporthandler.h>
#include <fileout.h>

#include <QtCore/QDir>
#include <QtCore/QTextStream>
#include <QtCore/QVariant>
#include <QtCore/QDebug>

QString HeaderGenerator::fileNameSuffix() const
{
    return QLatin1String("_wrapper.h");
}

QString HeaderGenerator::fileNameForContext(GeneratorContext &context) const
{
    const AbstractMetaClass *metaClass = context.metaClass();
    if (!context.forSmartPointer()) {
        QString fileNameBase = metaClass->qualifiedCppName().toLower();
        fileNameBase.replace(QLatin1String("::"), QLatin1String("_"));
        return fileNameBase + fileNameSuffix();
    }
    const AbstractMetaType *smartPointerType = context.preciseType();
    QString fileNameBase = getFileNameBaseForSmartPointer(smartPointerType, metaClass);
    return fileNameBase + fileNameSuffix();
}

void HeaderGenerator::writeCopyCtor(QTextStream& s, const AbstractMetaClass* metaClass) const
{
    s << INDENT <<  wrapperName(metaClass) << "(const " << metaClass->qualifiedCppName() << "& self)";
    s << " : " << metaClass->qualifiedCppName() << "(self)" << endl;
    s << INDENT << "{" << endl;
    s << INDENT << "}" << endl << endl;
}

void HeaderGenerator::writeProtectedFieldAccessors(QTextStream& s, const AbstractMetaField* field) const
{
    AbstractMetaType *metaType = field->type();
    QString fieldType = metaType->cppSignature();
    QString fieldName = field->enclosingClass()->qualifiedCppName() + QLatin1String("::") + field->name();

    // Force use of pointer to return internal variable memory
    bool useReference = (!metaType->isConstant() &&
                         !metaType->isEnum() &&
                         !metaType->isPrimitive() &&
                         metaType->indirections() == 0);


    // Get function
    s << INDENT << "inline " << fieldType
      << (useReference ? '*' : ' ')
      << ' ' << protectedFieldGetterName(field) << "()"
      << " { return "
      << (useReference ? '&' : ' ') << "this->" << fieldName << "; }" << endl;

    // Set function
    s << INDENT << "inline void " << protectedFieldSetterName(field) << '(' << fieldType << " value)"
      << " { " << fieldName << " = value; }" << endl;
}

void HeaderGenerator::generateClass(QTextStream &s, GeneratorContext &classContext)
{
    AbstractMetaClass *metaClass = classContext.metaClass();
    if (ReportHandler::isDebug(ReportHandler::SparseDebug))
        qCDebug(lcShiboken) << "Generating header for " << metaClass->fullName();
    m_inheritedOverloads.clear();
    Indentation indent(INDENT);

    // write license comment
    s << licenseComment();

    QString wrapperName;
    if (!classContext.forSmartPointer()) {
        wrapperName = HeaderGenerator::wrapperName(metaClass);
    } else {
        wrapperName = HeaderGenerator::wrapperName(classContext.preciseType());
    }
    QString outerHeaderGuard = getFilteredCppSignatureString(wrapperName).toUpper();
    QString innerHeaderGuard;

    // Header
    s << "#ifndef SBK_" << outerHeaderGuard << "_H" << endl;
    s << "#define SBK_" << outerHeaderGuard << "_H" << endl << endl;

    if (!avoidProtectedHack())
        s << "#define protected public" << endl << endl;

    //Includes
    s << metaClass->typeEntry()->include() << endl;

    if (shouldGenerateCppWrapper(metaClass) &&
        usePySideExtensions() && metaClass->isQObject())
        s << "namespace PySide { class DynamicQMetaObject; }\n\n";

    while (shouldGenerateCppWrapper(metaClass)) {
        if (!innerHeaderGuard.isEmpty()) {
            s << "#  ifndef SBK_" << innerHeaderGuard << "_H" << endl;
            s << "#  define SBK_" << innerHeaderGuard << "_H" << endl << endl;
            s << "// Inherited base class:" << endl;
        }

        // Class
        s << "class " << wrapperName;
        s << " : public " << metaClass->qualifiedCppName();

        s << endl << '{' << endl << "public:" << endl;

        const AbstractMetaFunctionList &funcs = filterFunctions(metaClass);
        for (AbstractMetaFunction *func : funcs) {
            if ((func->attributes() & AbstractMetaAttributes::FinalCppMethod) == 0)
                writeFunction(s, func);
        }

        if (avoidProtectedHack() && metaClass->hasProtectedFields()) {
            const AbstractMetaFieldList &fields = metaClass->fields();
            for (AbstractMetaField *field : fields) {
                if (!field->isProtected())
                    continue;
                writeProtectedFieldAccessors(s, field);
            }
        }

        //destructor
        // PYSIDE-504: When C++ 11 is used, then the destructor must always be written.
        // See generator.h for further reference.
        if (!avoidProtectedHack() || !metaClass->hasPrivateDestructor() || alwaysGenerateDestructor) {
            s << INDENT;
            if (avoidProtectedHack() && metaClass->hasPrivateDestructor())
                s << "// C++11: need to declare (unimplemented) destructor because "
                     "the base class destructor is private." << endl;
            s << '~' << wrapperName << "();" << endl;
        }

        writeCodeSnips(s, metaClass->typeEntry()->codeSnips(), TypeSystem::CodeSnipPositionDeclaration, TypeSystem::NativeCode);

        if ((!avoidProtectedHack() || !metaClass->hasPrivateDestructor())
            && usePySideExtensions() && metaClass->isQObject()) {
            s << "public:\n";
            s << INDENT << "int qt_metacall(QMetaObject::Call call, int id, void** args) override;" << endl;
            s << INDENT << "void* qt_metacast(const char* _clname) override;" << endl;
        }

        if (!m_inheritedOverloads.isEmpty()) {
            s << INDENT << "// Inherited overloads, because the using keyword sux" << endl;
            writeInheritedOverloads(s);
            m_inheritedOverloads.clear();
        }

        if (usePySideExtensions())
            s << INDENT << "static void pysideInitQtMetaTypes();" << endl;

        s << "};" << endl << endl;
        if (!innerHeaderGuard.isEmpty())
            s << "#  endif // SBK_" << innerHeaderGuard << "_H" << endl << endl;

        // PYSIDE-500: Use also includes for inherited wrapper classes, because
        // without the protected hack, we sometimes need to cast inherited wrappers.
        // But we don't use multiple include files. Instead, they are inserted as recursive
        // headers. This keeps the file structure as simple as before the enhanced inheritance.
        metaClass = metaClass->baseClass();
        if (!metaClass || !avoidProtectedHack())
            break;
        classContext = GeneratorContext(metaClass);
        if (!classContext.forSmartPointer()) {
            wrapperName = HeaderGenerator::wrapperName(metaClass);
        } else {
            wrapperName = HeaderGenerator::wrapperName(classContext.preciseType());
        }
        innerHeaderGuard = getFilteredCppSignatureString(wrapperName).toUpper();
    }

    s << "#endif // SBK_" << outerHeaderGuard << "_H" << endl << endl;
}

void HeaderGenerator::writeFunction(QTextStream& s, const AbstractMetaFunction* func)
{

    // do not write copy ctors here.
    if (!func->isPrivate() && func->functionType() == AbstractMetaFunction::CopyConstructorFunction) {
        writeCopyCtor(s, func->ownerClass());
        return;
    }
    if (func->isUserAdded())
        return;

    if (avoidProtectedHack() && func->isProtected() && !func->isConstructor() && !func->isOperatorOverload()) {
        s << INDENT << "inline " << (func->isStatic() ? "static " : "");
        s << functionSignature(func, QString(), QLatin1String("_protected"), Generator::EnumAsInts|Generator::OriginalTypeDescription)
            << " { ";
        s << (func->type() ? "return " : "");
        if (!func->isAbstract())
            s << func->ownerClass()->qualifiedCppName() << "::";
        s << func->originalName() << '(';
        QStringList args;
        const AbstractMetaArgumentList &arguments = func->arguments();
        for (const AbstractMetaArgument *arg : arguments) {
            QString argName = arg->name();
            const TypeEntry* enumTypeEntry = 0;
            if (arg->type()->isFlags())
                enumTypeEntry = static_cast<const FlagsTypeEntry*>(arg->type()->typeEntry())->originator();
            else if (arg->type()->isEnum())
                enumTypeEntry = arg->type()->typeEntry();
            if (enumTypeEntry)
                argName = QString::fromLatin1("%1(%2)").arg(arg->type()->cppSignature(), argName);
            args << argName;
        }
        s << args.join(QLatin1String(", ")) << ')';
        s << "; }" << endl;
    }

    // pure virtual functions need a default implementation
    const bool notAbstract = !func->isAbstract();
    if ((func->isPrivate() && notAbstract && !visibilityModifiedToPrivate(func))
        || (func->isModifiedRemoved() && notAbstract))
        return;

    if (avoidProtectedHack() && func->ownerClass()->hasPrivateDestructor()
        && (func->isAbstract() || func->isVirtual()))
        return;

    if (func->isConstructor() || func->isAbstract() || func->isVirtual()) {
        s << INDENT;
        Options virtualOption = Generator::OriginalTypeDescription;

        const bool virtualFunc = func->isVirtual() || func->isAbstract();
        if (!virtualFunc && !func->hasSignatureModifications())
            virtualOption = Generator::NoOption;

        s << functionSignature(func, QString(), QString(), virtualOption);

        if (virtualFunc)
            s << " override";
        s << ';' << endl;
        // Check if this method hide other methods in base classes
        const AbstractMetaFunctionList &ownerFuncs = func->ownerClass()->functions();
        for (const AbstractMetaFunction *f : ownerFuncs) {
            if (f != func
                && !f->isConstructor()
                && !f->isPrivate()
                && !f->isVirtual()
                && !f->isAbstract()
                && !f->isStatic()
                && f->name() == func->name()) {
                m_inheritedOverloads << f;
            }
        }

        // TODO: when modified an abstract method ceases to be virtual but stays abstract
        //if (func->isModifiedRemoved() && func->isAbstract()) {
        //}
    }
}

static void _writeTypeIndexValue(QTextStream& s, const QString& variableName,
                                 int typeIndex)
{
    s << "    ";
    s.setFieldWidth(56);
    s << variableName;
    s.setFieldWidth(0);
    s << " = " << typeIndex;
}

static inline void _writeTypeIndexValueLine(QTextStream& s,
                                            const QString& variableName,
                                            int typeIndex)
{
    _writeTypeIndexValue(s, variableName, typeIndex);
    s << ",\n";
}

void HeaderGenerator::writeTypeIndexValueLine(QTextStream& s, const TypeEntry* typeEntry)
{
    if (!typeEntry || !typeEntry->generateCode())
        return;
    s.setFieldAlignment(QTextStream::AlignLeft);
    const int typeIndex = typeEntry->sbkIndex();
    _writeTypeIndexValueLine(s, getTypeIndexVariableName(typeEntry), typeIndex);
    if (typeEntry->isComplex()) {
        const ComplexTypeEntry* cType = static_cast<const ComplexTypeEntry*>(typeEntry);
        if (cType->baseContainerType()) {
            const AbstractMetaClass *metaClass = AbstractMetaClass::findClass(classes(), cType);
            if (metaClass->templateBaseClass())
                _writeTypeIndexValueLine(s, getTypeIndexVariableName(metaClass, true), typeIndex);
        }
    }
    if (typeEntry->isEnum()) {
        const EnumTypeEntry* ete = static_cast<const EnumTypeEntry*>(typeEntry);
        if (ete->flags())
            writeTypeIndexValueLine(s, ete->flags());
    }
}

void HeaderGenerator::writeTypeIndexValueLines(QTextStream& s, const AbstractMetaClass* metaClass)
{
    if (!metaClass->typeEntry()->generateCode())
        return;
    writeTypeIndexValueLine(s, metaClass->typeEntry());
    const AbstractMetaEnumList &enums = metaClass->enums();
    for (const AbstractMetaEnum *metaEnum : enums) {
        if (metaEnum->isPrivate())
            continue;
        writeTypeIndexValueLine(s, metaEnum->typeEntry());
    }
}

// Format the typedefs for the typedef entries to be generated
static void formatTypeDefEntries(QTextStream &s)
{
    QVector<const TypedefEntry *> entries;
    const auto typeDbEntries = TypeDatabase::instance()->typedefEntries();
    for (auto it = typeDbEntries.cbegin(), end = typeDbEntries.cend(); it != end; ++it) {
        if (it.value()->generateCode() != 0)
            entries.append(it.value());
    }
    if (entries.isEmpty())
        return;
    s << "\n// typedef entries\n";
    for (const auto e : entries) {
        const QString name = e->qualifiedCppName();
        // Fixme: simplify by using nested namespaces in C++ 17.
        const auto components = name.splitRef(QLatin1String("::"));
        const int nameSpaceCount = components.size() -  1;
        for (int n = 0; n < nameSpaceCount; ++n)
            s << "namespace " << components.at(n) << " {\n";
        s << "using " << components.constLast() << " = " << e->sourceType() << ";\n";
        for (int n = 0; n < nameSpaceCount; ++n)
            s << "}\n";
    }
    s << '\n';
}


bool HeaderGenerator::finishGeneration()
{
    // Generate the main header for this module.
    // This header should be included by binding modules
    // extendind on top of this one.
    QSet<Include> includes;
    QString macros;
    QTextStream macrosStream(&macros);
    QString sbkTypeFunctions;
    QTextStream typeFunctions(&sbkTypeFunctions);
    QString protectedEnumSurrogates;
    QTextStream protEnumsSurrogates(&protectedEnumSurrogates);

    Indentation indent(INDENT);

    macrosStream << "// Type indices\nenum : int {\n";
    AbstractMetaEnumList globalEnums = this->globalEnums();
    const AbstractMetaClassList &classList = classes();
    for (const AbstractMetaClass *metaClass : classList) {
        writeTypeIndexValueLines(macrosStream, metaClass);
        lookForEnumsInClassesNotToBeGenerated(globalEnums, metaClass);
    }

    for (const AbstractMetaEnum *metaEnum : qAsConst(globalEnums))
        writeTypeIndexValueLine(macrosStream, metaEnum->typeEntry());

    // Write the smart pointer define indexes.
    int smartPointerCountIndex = getMaxTypeIndex();
    int smartPointerCount = 0;
    const QVector<const AbstractMetaType *> &instantiatedSmartPtrs = instantiatedSmartPointers();
    for (const AbstractMetaType *metaType : instantiatedSmartPtrs) {
        _writeTypeIndexValue(macrosStream, getTypeIndexVariableName(metaType),
                             smartPointerCountIndex);
        macrosStream << ", // " << metaType->cppSignature() << endl;
        ++smartPointerCountIndex;
        ++smartPointerCount;
    }

    _writeTypeIndexValue(macrosStream,
                         QLatin1String("SBK_") + moduleName() + QLatin1String("_IDX_COUNT"),
                         getMaxTypeIndex() + smartPointerCount);
    macrosStream << "\n};\n";

    macrosStream << "namespace MODULE_NAMESPACE" << endl;
    macrosStream << "{" << endl;
    {
        Indentation indentation(INDENT);

        macrosStream << INDENT << "// This variable stores all Python types exported by this module." << endl;
        macrosStream << INDENT << "extern PyTypeObject** " << cppApiVariableName() << ';' << endl << endl;
        macrosStream << INDENT << "// This variable stores the Python module object exported by this module." << endl;
        macrosStream << INDENT << "extern PyObject* " << pythonModuleObjectName() << ';' << endl << endl;
        macrosStream << INDENT << "// This variable stores all type converters exported by this module." << endl;
        macrosStream << INDENT << "extern SbkConverter** " << convertersVariableName() << ';' << endl << endl;
    }

    macrosStream << "}" << endl;

    macrosStream << "using MODULE_NAMESPACE::" << cppApiVariableName() << ';' << endl;
    macrosStream << "using MODULE_NAMESPACE::" << convertersVariableName() << ';' << endl;

    // TODO-CONVERTER ------------------------------------------------------------------------------
    // Using a counter would not do, a fix must be made to APIExtractor's getTypeIndex().
    macrosStream << "// Converter indices\nenum : int {\n";
    const PrimitiveTypeEntryList &primitives = primitiveTypes();
    int pCount = 0;
    for (const PrimitiveTypeEntry *ptype : primitives) {
        /* Note: do not generate indices for typedef'd primitive types
         * as they'll use the primitive type converters instead, so we
         * don't need to create any other.
         */
        if (!ptype->generateCode() || !ptype->customConversion())
            continue;

        _writeTypeIndexValueLine(macrosStream, getTypeIndexVariableName(ptype), pCount++);
    }

    const QVector<const AbstractMetaType *> &containers = instantiatedContainers();
    for (const AbstractMetaType *container : containers) {
        _writeTypeIndexValue(macrosStream, getTypeIndexVariableName(container), pCount);
        macrosStream << ", // " << container->cppSignature() << endl;
        pCount++;
    }

    // Because on win32 the compiler will not accept a zero length array.
    if (pCount == 0)
        pCount++;
    _writeTypeIndexValue(macrosStream, QStringLiteral("SBK_%1_CONVERTERS_IDX_COUNT")
                                       .arg(moduleName()), pCount);
    macrosStream << "\n};\n";

    formatTypeDefEntries(macrosStream);

    // TODO-CONVERTER ------------------------------------------------------------------------------

    macrosStream << "// Macros for type check" << endl;
    for (const AbstractMetaEnum *cppEnum : qAsConst(globalEnums)) {
        if (cppEnum->isAnonymous() || cppEnum->isPrivate())
            continue;
        includes << cppEnum->typeEntry()->include();
        writeProtectedEnumSurrogate(protEnumsSurrogates, cppEnum);
        writeSbkTypeFunction(typeFunctions, cppEnum);
    }

    for (AbstractMetaClass *metaClass : classList) {
        if (!shouldGenerate(metaClass))
            continue;

        //Includes
        const TypeEntry* classType = metaClass->typeEntry();
        includes << classType->include();

        const AbstractMetaEnumList &enums = metaClass->enums();
        for (const AbstractMetaEnum *cppEnum : enums) {
            if (cppEnum->isAnonymous() || cppEnum->isPrivate())
                continue;
            EnumTypeEntry* enumType = cppEnum->typeEntry();
            includes << enumType->include();
            writeProtectedEnumSurrogate(protEnumsSurrogates, cppEnum);
            writeSbkTypeFunction(typeFunctions, cppEnum);
        }

        if (!metaClass->isNamespace())
            writeSbkTypeFunction(typeFunctions, metaClass);
    }

    for (const AbstractMetaType *metaType : instantiatedSmartPtrs) {
        const TypeEntry *classType = metaType->typeEntry();
        includes << classType->include();
        writeSbkTypeFunction(typeFunctions, metaType);
    }

    QString moduleHeaderFileName(outputDirectory()
                                 + QDir::separator() + subDirectoryForPackage(packageName())
                                 + QDir::separator() + getModuleHeaderFileName());

    QString includeShield(QLatin1String("SBK_") + moduleName().toUpper() + QLatin1String("_PYTHON_H"));

    FileOut file(moduleHeaderFileName);
    QTextStream& s = file.stream;
    // write license comment
    s << licenseComment() << endl << endl;

    s << "#ifndef " << includeShield << endl;
    s << "#define " << includeShield << endl<< endl;
    if (!avoidProtectedHack()) {
        s << "//workaround to access protected functions" << endl;
        s << "#define protected public" << endl << endl;
    }

    s << "#include <exception>" << endl;
    s << "#ifndef STD_EXCEPTION_TRANSLATOR" << endl;
    s << "#define STD_EXCEPTION_TRANSLATOR" << endl;
    s << "using stdExceptionTranslator = void ( * )( const std::exception& );" << endl;
    s << "namespace " << internalNamespaceName() << endl;
    s << "{" << endl;
    {
        Indentation indentation(INDENT);
        s << INDENT << "extern stdExceptionTranslator setPythonError;" << endl;
    }
    s << "}" << endl;
    s << "using " << internalNamespaceName() << "::setPythonError;" << endl;
    s << "#endif // STD_EXCEPTION_TRANSLATOR" << endl;

    s << "#include <sbkpython.h>" << endl;
    s << "#include <sbkconverter.h>" << endl;

    QStringList requiredTargetImports = TypeDatabase::instance()->requiredTargetImports();
    if (!requiredTargetImports.isEmpty()) {
        s << "#if !defined(MODULE_NAMESPACE)" << endl;
        {
            Indentation indentation(INDENT);
            s << "#" << INDENT << "define MODULE_NAMESPACE " << internalNamespaceName() << endl;
        }
        s << "#endif  // !defined(MODULE_NAMESPACE)" << endl;
        s << endl;
        s << "// Module Includes" << endl;
        for (const QString &requiredModule : qAsConst(requiredTargetImports))
            s << "#include <" << getModuleHeaderFileName(requiredModule) << ">" << endl;
        s << endl;
    }

    s << "// Binded library includes" << endl;
    for (const Include &include : qAsConst(includes))
        s << include;

    if (!primitiveTypes().isEmpty()) {
        s << "// Conversion Includes - Primitive Types" << endl;
        const PrimitiveTypeEntryList &primitiveTypeList = primitiveTypes();
        for (const PrimitiveTypeEntry *ptype : primitiveTypeList)
            s << ptype->include();
        s << endl;
    }

    if (!containerTypes().isEmpty()) {
        s << "// Conversion Includes - Container Types" << endl;
        const ContainerTypeEntryList &containerTypeList = containerTypes();
        for (const ContainerTypeEntry *ctype : containerTypeList)
            s << ctype->include();
        s << endl;
    }

    s << macros << endl;

    if (!protectedEnumSurrogates.isEmpty()) {
        s << "// Protected enum surrogates" << endl;
        s << protectedEnumSurrogates << endl;
    }

    s << "namespace Shiboken" << endl << '{' << endl << endl;

    s << "// PyType functions, to get the PyObjectType for a type T\n";
    s << sbkTypeFunctions << endl;

    s << "} // namespace Shiboken" << endl << endl;

    s << "#endif // " << includeShield << endl << endl;

    return file.done() != FileOut::Failure;
}

void HeaderGenerator::writeProtectedEnumSurrogate(QTextStream& s, const AbstractMetaEnum* cppEnum)
{
    if (avoidProtectedHack() && cppEnum->isProtected())
        s << "enum " << protectedEnumSurrogateName(cppEnum) << " {};" << endl;
}

void HeaderGenerator::writeSbkTypeFunction(QTextStream& s, const AbstractMetaEnum* cppEnum)
{
    QString enumName;
    if (avoidProtectedHack() && cppEnum->isProtected()) {
        enumName = protectedEnumSurrogateName(cppEnum);
    } else {
        enumName = cppEnum->name();
        if (cppEnum->enclosingClass())
            enumName = cppEnum->enclosingClass()->qualifiedCppName() + QLatin1String("::") + enumName;
    }

    s << "template<> inline PyTypeObject* SbkType< ::" << enumName << " >() ";
    s << "{ return " << cpythonTypeNameExt(cppEnum->typeEntry()) << "; }\n";

    FlagsTypeEntry* flag = cppEnum->typeEntry()->flags();
    if (flag) {
        s <<  "template<> inline PyTypeObject* SbkType< ::" << flag->name() << " >() "
          << "{ return " << cpythonTypeNameExt(flag) << "; }\n";
    }
}

void HeaderGenerator::writeSbkTypeFunction(QTextStream& s, const AbstractMetaClass* cppClass)
{
    s <<  "template<> inline PyTypeObject* SbkType< ::" << cppClass->qualifiedCppName() << " >() "
      <<  "{ return reinterpret_cast<PyTypeObject*>(" << cpythonTypeNameExt(cppClass->typeEntry()) << "); }\n";
}

void HeaderGenerator::writeSbkTypeFunction(QTextStream &s, const AbstractMetaType *metaType)
{
    s <<  "template<> inline PyTypeObject* SbkType< ::" << metaType->cppSignature() << " >() "
      <<  "{ return reinterpret_cast<PyTypeObject*>(" << cpythonTypeNameExt(metaType) << "); }\n";
}

void HeaderGenerator::writeInheritedOverloads(QTextStream& s)
{
    for (const AbstractMetaFunction *func : qAsConst(m_inheritedOverloads)) {
        s << INDENT << "inline ";
        s << functionSignature(func, QString(), QString(), Generator::EnumAsInts|Generator::OriginalTypeDescription) << " { ";
        s << (func->type() ? "return " : "");
        s << func->ownerClass()->qualifiedCppName() << "::" << func->originalName() << '(';
        QStringList args;
        const AbstractMetaArgumentList &arguments = func->arguments();
        for (const AbstractMetaArgument *arg : arguments) {
            QString argName = arg->name();
            const TypeEntry* enumTypeEntry = 0;
            if (arg->type()->isFlags())
                enumTypeEntry = static_cast<const FlagsTypeEntry*>(arg->type()->typeEntry())->originator();
            else if (arg->type()->isEnum())
                enumTypeEntry = arg->type()->typeEntry();
            if (enumTypeEntry)
                argName = arg->type()->cppSignature() + QLatin1Char('(') + argName + QLatin1Char(')');
            args << argName;
        }
        s << args.join(QLatin1String(", ")) << ')';
        s << "; }" << endl;
    }
}
