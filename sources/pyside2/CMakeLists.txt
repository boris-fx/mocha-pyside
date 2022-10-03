include(cmake/Macros/icecc.cmake) # this must be the first line!

cmake_minimum_required(VERSION 3.1)
cmake_policy(VERSION 3.1)

# Don't ignore targets that do not exist, inside add_dependencies calls.
cmake_policy(SET CMP0046 NEW)

set (QT_MAJOR_VERSION 5)

project(pysidebindings)

set(CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/../cmake_helpers/
                      ${CMAKE_CURRENT_SOURCE_DIR}/../shiboken2/data/
                      ${CMAKE_SOURCE_DIR}/cmake/Macros/
                      ${CMAKE_MODULE_PATH})

if(CMAKE_BUILD_TYPE)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" ISL_BUILD_TYPE)
else()
    set(ISL_BUILD_TYPE "debug")
endif()

if(NOT ISL_QT_ROOT_DIR)
    message(FATAL_ERROR "ISL_QT_ROOT_DIR is not set")
endif()

if(NOT ISL_DEPENDENCIES_ROOT)
    set(ISL_DEPENDENCIES_ROOT "../")
    message(WARNING "ISL_DEPENDENCIES_ROOT isn't set. Assuming ${ISL_DEPENDENCIES_ROOT}")
endif()

if(NOT ISL_LINKAGE_MODE_TOKEN)
    set(ISL_LINKAGE_MODE_TOKEN "shared")
    message(WARNING "ISL_LINKAGE_MODE_TOKEN isn't set. Assuming ${ISL_LINKAGE_MODE_TOKEN}")
endif()

set(PYSIDE_GENERATE_PYI_FILES "ON" CACHE BOOL "Enable/Disable generation of _pyi files")

include(shiboken_helpers)
include(helpers)

# Python definitions
set(PYTHON_DIRECTORY "${ISL_DEPENDENCIES_ROOT}/python/${ISL_PYTHON_VERSION}/${ISL_TARGET_OS}/${ISL_BUILD_TYPE}/${ISL_LINKAGE_MODE_TOKEN}")
if(NOT EXISTS ${PYTHON_DIRECTORY})
   message(FATAL_ERROR "${PYTHON_DIRECTORY} does not exist")
endif()

# Unfortunately, FIND_FILE/FIND_PATH are non-recursive functions
find_path(PYTHON_INCLUDE_PATH
     Python.h
     PATHS "${PYTHON_DIRECTORY}/include"
     NO_SYSTEM_ENVIRONMENT_PATH
)

file(GLOB_RECURSE _python_h_file "${PYTHON_DIRECTORY}/*Python.h")
get_filename_component(PYTHON_INCLUDE_PATH ${_python_h_file} PATH)

if(NOT EXISTS ${PYTHON_INCLUDE_PATH})
    message(FATAL_ERROR "Can't find Python.h file in directory ${PYTHON_DIRECTORY}/include")
endif()

set(PYTHON_INCLUDE_DIRS ${PYTHON_INCLUDE_PATH})

string(REGEX REPLACE "^([0-9]+)\\.[0-9]+\\.[0-9]+$" "\\1" PYTHON_VERSION_MAJOR "${ISL_PYTHON_VERSION}")
string(REGEX REPLACE "^[0-9]+\\.([0-9]+)\\.[0-9]+$" "\\1" PYTHON_VERSION_MINOR "${ISL_PYTHON_VERSION}")
string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+)$" "\\1" PYTHON_VERSION_PATCH "${ISL_PYTHON_VERSION}")
set(PYTHON_SUFFIX "-python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}")

find_library(PYTHON_LIBRARIES
    NAMES "python${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}" 
    "python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}" "python"
    "python${PYTHON_VERSION_MAJOR}"
    PATHS "${PYTHON_DIRECTORY}/lib"
    NO_DEFAULT_PATH)
find_program(PYTHON_EXECUTABLE
    NAMES "python${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}" 
    "python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}" "python" 
    PATHS "${PYTHON_DIRECTORY}" "${PYTHON_DIRECTORY}/bin"
    NO_SYSTEM_ENVIRONMENT_PATH NO_DEFAULT_PATH)
set(PYTHON_DEBUG_LIBRARIES ${PYTHON_LIBRARIES})

set(PYSIDE_VERSION_FILE_PATH "${CMAKE_SOURCE_DIR}/pyside_version.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${PYSIDE_VERSION_FILE_PATH}
)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} "${PYSIDE_VERSION_FILE_PATH}"
  OUTPUT_VARIABLE PYSIDE_VERSION_OUTPUT
  ERROR_VARIABLE PYSIDE_VERSION_OUTPUT_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT PYSIDE_VERSION_OUTPUT)
    message(FATAL_ERROR "Could not identify PySide2 version. Error: ${PYSIDE_VERSION_OUTPUT_ERROR}")
endif()

list(GET PYSIDE_VERSION_OUTPUT 0 BINDING_API_MAJOR_VERSION)
list(GET PYSIDE_VERSION_OUTPUT 1 BINDING_API_MINOR_VERSION)
list(GET PYSIDE_VERSION_OUTPUT 2 BINDING_API_MICRO_VERSION)
# a - alpha, b - beta, rc - rc
list(GET PYSIDE_VERSION_OUTPUT 3 BINDING_API_PRE_RELEASE_VERSION_TYPE)
# the number of the pre release (alpha1, beta3, rc7, etc.)
list(GET PYSIDE_VERSION_OUTPUT 4 BINDING_API_PRE_RELEASE_VERSION)

macro(get_python_extension_suffix)
  # Result of imp.get_suffixes() depends on the platform, but generally looks something like:
  # [('.cpython-34m-x86_64-linux-gnu.so', 'rb', 3), ('.cpython-34m.so', 'rb', 3),
  # ('.abi3.so', 'rb', 3), ('.so', 'rb', 3), ('.py', 'r', 1), ('.pyc', 'rb', 2)]
  # We pick the first most detailed one, strip of the file extension part.

  execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
       import imp, re
       first_suffix = imp.get_suffixes()[0][0]
       res = re.search(r'^(.+)\\.', first_suffix)
       if res:
           first_suffix = res.group(1)
       else:
           first_suffix = ''
       print(first_suffix)
       "
    OUTPUT_VARIABLE PYTHON_EXTENSION_SUFFIX
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  message("PYTHON_EXTENSION_SUFFIX: " ${PYTHON_EXTENSION_SUFFIX})
endmacro()


if (NOT PYTHON_EXTENSION_SUFFIX)
  get_python_extension_suffix()
endif()

# On Windows, PYTHON_LIBRARIES can be a list. Example:
#    optimized;C:/Python36/libs/python36.lib;debug;C:/Python36/libs/python36_d.lib
# On other platforms, this result is not used at all.
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        for lib in '${PYTHON_LIBRARIES}'.split(';'):
            if '/' in lib:
                prefix, py = lib.rsplit( '/', 1)
                if py.startswith('python3'):
                    print(prefix + '/python3.lib')
                    break
        "
    OUTPUT_VARIABLE PYTHON_LIMITED_LIBRARIES
    OUTPUT_STRIP_TRAILING_WHITESPACE)

if (WIN32)
    set(PATH_SEP "\;")
else()
    set(PATH_SEP ":")
endif()

option(FORCE_LIMITED_API "Enable the limited API." "yes")

set(PYTHON_LIMITED_API 0)
if(FORCE_LIMITED_API STREQUAL "yes")
    # GREATER_EQUAL is available only from cmake 3.7 on. We mean python 3.5 .
    if (${PYTHON_VERSION_MAJOR} EQUAL 3 AND ${PYTHON_VERSION_MINOR} GREATER 4)
        add_definitions("-DPy_LIMITED_API=0x03050000")
        set(PYTHON_LIMITED_API 1)
    endif()
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "${SHIBOKEN_BUILD_TYPE}" CACHE STRING "Build Type")
endif()
if (CMAKE_BUILD_TYPE STREQUAL "Release")
    add_definitions("-DNDEBUG")
endif()

if (PYTHON_LIMITED_API)
    if (WIN32 AND NOT EXISTS "${PYTHON_LIMITED_LIBRARIES}")
        message(FATAL_ERROR "The Limited API was enabled, but ${PYTHON_LIMITED_LIBRARIES} was not found!")
    endif()
    message(STATUS "******************************************************")
    message(STATUS "** Limited API enabled ${PYTHON_LIMITED_LIBRARIES}")
    message(STATUS "******************************************************")
endif()

if (NOT PYTHON_CONFIG_SUFFIX)
  if (PYTHON_VERSION_MAJOR EQUAL 2)
      set(PYTHON_CONFIG_SUFFIX "-python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}")
      if (PYTHON_EXTENSION_SUFFIX)
          set(PYTHON_CONFIG_SUFFIX "${PYTHON_CONFIG_SUFFIX}${PYTHON_EXTENSION_SUFFIX}")
      endif()
  elseif (PYTHON_VERSION_MAJOR EQUAL 3)
      if (PYTHON_LIMITED_API)
          if(WIN32)
              set(PYTHON_EXTENSION_SUFFIX "")
          else()
              set(PYTHON_EXTENSION_SUFFIX ".abi3")
          endif()
      endif()
      set(PYTHON_CONFIG_SUFFIX "${PYTHON_EXTENSION_SUFFIX}")
  endif()
endif()

if (NOT PYTHON_SHARED_LIBRARY_SUFFIX)
  set(PYTHON_SHARED_LIBRARY_SUFFIX "${PYTHON_CONFIG_SUFFIX}")

  # Append a "v" to disambiguate the python version and the pyside version in the
  # shared library file name.
  if (APPLE AND PYTHON_VERSION_MAJOR EQUAL 2)
      set(PYTHON_SHARED_LIBRARY_SUFFIX "${PYTHON_SHARED_LIBRARY_SUFFIX}v")
  endif()
endif()

# Currently we don't need this ugly suffix.
set(PYTHON_EXTENSION_SUFFIX "")

message(STATUS "PYTHON_EXTENSION_SUFFIX:      ${PYTHON_EXTENSION_SUFFIX}")
message(STATUS "PYTHON_CONFIG_SUFFIX:         ${PYTHON_CONFIG_SUFFIX}")
message(STATUS "PYTHON_SHARED_LIBRARY_SUFFIX: ${PYTHON_SHARED_LIBRARY_SUFFIX}")

find_package(Shiboken2 2.0.0 REQUIRED)
find_package(Qt${QT_MAJOR_VERSION} 5.12 REQUIRED COMPONENTS Core)
add_definitions(${Qt${QT_MAJOR_VERSION}Core_DEFINITIONS})

find_file(GL_H "gl.h" PATH_SUFFIXES "GL")
message(STATUS "GL Headers path:" "${GL_H}")
include(FindQt5Extra)

set(XVFB_EXEC "")
option(USE_XVFB "Uses xvfb-run with the unit tests to avoid QtGui tests popping windows on the screen." FALSE)
if(USE_XVFB)
    find_program(XVFB_RUN NAMES xvfb-run)
    if (NOT ${XVFB_RUN} MATCHES "XVFB_RUN-NOTFOUND")
        set(XVFB_EXEC ${XVFB_RUN} -a)
        message(STATUS "Using xvfb-run to perform QtGui tests.")
    endif()
endif()

if(ISL_QT_NAMESPACE)
    add_definitions(-DQT_NAMESPACE=${ISL_QT_NAMESPACE})
endif()
if(ISL_QT_NAMESPACE_MAC_CRC)
    add_definitions(-DQT_NAMESPACE_MAC_CRC=${ISL_QT_NAMESPACE_MAC_CRC})
endif()

option(BUILD_TESTS "Build tests." FALSE)
option(ENABLE_VERSION_SUFFIX "Used to use current version in suffix to generated files. This is used to allow multiples versions installed simultaneous." FALSE)
set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
set(LIB_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/lib${LIB_SUFFIX}" CACHE PATH "The subdirectory relative to the install prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)
if(CMAKE_HOST_APPLE)
    set(ALTERNATIVE_QT_INCLUDE_DIR "" CACHE PATH "Deprecated. CMake now finds the proper include dir itself.")
    set(OSX_USE_LIBCPP "OFF" CACHE BOOL "Explicitly link the libc++ standard library (useful for osx deployment targets lower than 10.9.")
    if(OSX_USE_LIBCPP)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

# Force usage of the C++11 standard, without a silent fallback
# to C++98 if the compiler does not support C++11.
if(${QT_MAJOR_VERSION} GREATER_EQUAL 6)
    set(CMAKE_CXX_STANDARD 17)
else()
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# >= Qt5: QT_INCLUDE_DIR does no longer exist. Derive from QtCore
if(${QT_MAJOR_VERSION} GREATER_EQUAL 6)
    get_target_property(QT_INCLUDE_DIR Qt6::Core INTERFACE_INCLUDE_DIRECTORIES)
    get_filename_component(QT_INCLUDE_DIR "${QT_INCLUDE_DIR}" DIRECTORY)
else()
    # On Windows, macOS, and Linux it can be computed from Qt5Core_INCLUDE_DIRS, which contains
    # a list of include directories. We take the first one.
    list(GET Qt5Core_INCLUDE_DIRS 0 QT_INCLUDE_DIR)
endif()
message(STATUS "*** Qt ${QT_MAJOR_VERSION}, QT_INCLUDE_DIR= ${QT_INCLUDE_DIR}")

# On macOS, check if Qt is a framework build. This affects how include paths should be handled.
get_target_property(QtCore_is_framework Qt${QT_MAJOR_VERSION}::Core FRAMEWORK)

if (QtCore_is_framework)
    # Get the path to the framework dir.
    get_filename_component(QT_FRAMEWORK_INCLUDE_DIR "${QT_INCLUDE_DIR}/../" ABSOLUTE)
    message(STATUS "*** QT_FRAMEWORK_INCLUDE_DIR is ${QT_FRAMEWORK_INCLUDE_DIR}")

    # QT_INCLUDE_DIR points to the QtCore.framework directory, so we need to adjust this to point
    # to the actual include directory, which has include files for non-framework parts of Qt.
    get_filename_component(QT_INCLUDE_DIR "${QT_INCLUDE_DIR}/../../include" ABSOLUTE)
endif()

set_cmake_cxx_flags()

message(STATUS "*** computed QT_INCLUDE_DIR as ${QT_INCLUDE_DIR}")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE ${SHIBOKEN_BUILD_TYPE})
endif()

set(BINDING_NAME PySide2)

set(BINDING_API_VERSION "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}" CACHE STRING "PySide2 version" FORCE)
set(PYSIDE_SO_VERSION ${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION})
if (BINDING_API_PRE_RELEASE_VERSION_TYPE STREQUAL "")
    set(BINDING_API_VERSION_FULL "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}"
        CACHE STRING "PySide2 version [full]" FORCE)
else()
    set(BINDING_API_VERSION_FULL "${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}.${BINDING_API_MICRO_VERSION}~${BINDING_API_PRE_RELEASE_VERSION_TYPE}${BINDING_API_PRE_RELEASE_VERSION}"
        CACHE STRING "PySide2 version [full]" FORCE)
endif()

compute_config_py_values(BINDING_API_VERSION)

include(PySideModules)

# Set default values for pyside2_global.h
set (Qt${QT_MAJOR_VERSION}X11Extras_FOUND "0")
set (Qt${QT_MAJOR_VERSION}Test_FOUND "0")
set (Qt${QT_MAJOR_VERSION}Widgets_FOUND "0")

collect_essential_modules()
collect_optional_modules()

# Modules to be built unless specified by -DMODULES on command line
if (NOT MODULES)
    set(MODULES "${ALL_ESSENTIAL_MODULES};${ALL_OPTIONAL_MODULES}")
endif()

# This will contain the set of modules for which bindings are not built.
set(DISABLED_MODULES "${ALL_ESSENTIAL_MODULES};${ALL_OPTIONAL_MODULES}")

remove_skipped_modules()

# Mark all non-collected modules as disabled. This is used for disabling tests
# that depend on the disabled modules.
foreach(m ${DISABLED_MODULES})
    set(DISABLE_Qt${m} 1)
endforeach()


string(REGEX MATCHALL "[0-9]+" qt_version_helper "${Qt${QT_MAJOR_VERSION}Core_VERSION}")

list(GET qt_version_helper 0 QT_VERSION_MAJOR)
list(GET qt_version_helper 1 QT_VERSION_MINOR)
list(GET qt_version_helper 2 QT_VERSION_PATCH)
unset(qt_version_helper)

set(PYSIDE_QT_VERSION "${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}" CACHE STRING "Qt version used to compile PySide" FORCE)
if(ENABLE_VERSION_SUFFIX)
      set(pyside_SUFFIX "-${BINDING_API_MAJOR_VERSION}.${BINDING_API_MINOR_VERSION}")
endif()

# no more supported: include(${QT_USE_FILE})

# Configure OS support
check_os()
message(STATUS "Detected OS: ${AUTO_OS}")

# Define supported Qt Version
set(SUPPORTED_QT_VERSION "${QT_VERSION_MAJOR}.${QT_VERSION_MINOR}")

# uninstall target
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
               IMMEDIATE @ONLY)
add_custom_target(uninstall "${CMAKE_COMMAND}"
                  -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")


if (NOT PYTHON_SITE_PACKAGES)
    execute_process(
        COMMAND ${SHIBOKEN_PYTHON_INTERPRETER} -c "if True:
            from distutils import sysconfig
            from os.path import sep
            print(sysconfig.get_python_lib(1, 0, prefix='${CMAKE_INSTALL_PREFIX}').replace(sep, '/'))
            "
        OUTPUT_VARIABLE PYTHON_SITE_PACKAGES
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if (NOT PYTHON_SITE_PACKAGES)
        message(FATAL_ERROR "Could not detect Python module installation directory.")
    elseif (APPLE)
        message(STATUS "!!! The generated bindings will be installed on ${PYTHON_SITE_PACKAGES}, is it right!?")
    endif()
endif()

set(GENERATOR_EXTRA_FLAGS --generator-set=shiboken
                          --enable-parent-ctor-heuristic
                          --enable-pyside-extensions
                          --enable-return-value-heuristic
                          --use-isnull-as-nb_nonzero)
use_protected_as_public_hack()

# Build with Address sanitizer enabled if requested. This may break things, so use at your own risk.
if (SANITIZE_ADDRESS AND NOT MSVC)
    setup_sanitize_address()
endif()

#####################################################################
#  Adding sub-directories to build
#####################################################################

add_subdirectory(libpyside)
find_package(Qt${QT_MAJOR_VERSION}Designer)
if(${QT_MAJOR_VERSION} LESS 6 AND Qt${QT_MAJOR_VERSION}UiTools_FOUND
   AND Qt${QT_MAJOR_VERSION}Designer_FOUND)
    add_subdirectory(plugins)
endif()

# project directories
add_subdirectory(PySide2)
if (BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

find_program(SPHINX_BUILD sphinx-build)
find_program(DOT_EXEC dot)

if (QT_SRC_DIR AND SPHINX_BUILD AND DOT_EXEC)
    add_subdirectory(doc)
else ()
    set(DOCS_TARGET_DISABLED_MESSAGE "apidoc generation targets disabled.")
    if (NOT QT_SRC_DIR)
        message(STATUS "QT_SRC_DIR variable not set, ${DOCS_TARGET_DISABLED_MESSAGE}")
    elseif (NOT SPHINX_BUILD)
        message(STATUS "sphinx-build command not found, ${DOCS_TARGET_DISABLED_MESSAGE}")
    elseif (NOT DOT_EXEC)
        message(STATUS "graphviz not found, ${DOCS_TARGET_DISABLED_MESSAGE}")
    else()
        message(STATUS "Unknown issue occurred, ${DOCS_TARGET_DISABLED_MESSAGE}")
    endif()
endif()
