Include(icecc.cmake)

cmake_minimum_required(VERSION 3.1)
cmake_policy(VERSION 3.1)

if(ISL_QT_NAMESPACE)
    add_definitions(-DQT_NAMESPACE=${ISL_QT_NAMESPACE})
endif()
if(ISL_QT_NAMESPACE_MAC_CRC)
    add_definitions(-DQT_NAMESPACE_MAC_CRC=${ISL_QT_NAMESPACE_MAC_CRC})
endif()

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

set(CMAKE_BUILD_TYPE Release CACHE STRING "Build Type")

project(shiboken2)
include(CheckIncludeFileCXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../cmake_helpers/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/data/")

include(helpers)
include(shiboken_helpers)

add_definitions(${Qt5Core_DEFINITIONS})

option(BUILD_TESTS "Build tests." FALSE)
option(USE_PYTHON_VERSION "Use specific python version to build shiboken2." "")
option(DISABLE_DOCSTRINGS "Disable documentation extraction." FALSE)

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

# These regexes extract major/minor/patch version from a full version dir name
# of the form "3.9.12.f78a9707ea70f329". The first capture group is the one
# whose value we care about, whereas the second and final group is there to
# accept (but not require) the SCM version suffix.
string(REGEX REPLACE "^([0-9]+)\\.[0-9]+\\.[0-9]+(\\.[a-f0-9]+)?$" "\\1" PYTHON_VERSION_MAJOR "${ISL_PYTHON_VERSION}")
string(REGEX REPLACE "^[0-9]+\\.([0-9]+)\\.[0-9]+(\\.[a-f0-9]+)?$" "\\1" PYTHON_VERSION_MINOR "${ISL_PYTHON_VERSION}")
string(REGEX REPLACE "^[0-9]+\\.[0-9]+\\.([0-9]+)(\\.[a-f0-9]+)?$" "\\1" PYTHON_VERSION_PATCH "${ISL_PYTHON_VERSION}")
set(PYTHON_SUFFIX "-python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}")

find_library(PYTHON_LIBRARIES
    NAMES "python${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}" 
    "python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}" "python"
    "python${PYTHON_VERSION_MAJOR}"
    PATHS "${PYTHON_DIRECTORY}/lib"
    NO_DEFAULT_PATH)
find_program(PYTHON_EXECUTABLE
    REQUIRED
    NAMES "python${PYTHON_VERSION_MAJOR}${PYTHON_VERSION_MINOR}" 
    "python${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}" "python" 
    PATHS "${PYTHON_DIRECTORY}" "${PYTHON_DIRECTORY}/bin"
    NO_SYSTEM_ENVIRONMENT_PATH NO_DEFAULT_PATH)
set(PYTHON_DEBUG_LIBRARIES ${PYTHON_LIBRARIES})

if (NOT PYTHON_ARCH)
    get_python_arch()
endif()

set (QT_MAJOR_VERSION 5)
message(STATUS "Using Qt ${QT_MAJOR_VERSION}")
find_package(Qt${QT_MAJOR_VERSION} 5.12 REQUIRED COMPONENTS Core Xml XmlPatterns)

if (QUIET_BUILD)
    set_quiet_build()
endif()

setup_clang()

set(SHIBOKEN_VERSION_FILE_PATH "${CMAKE_SOURCE_DIR}/shiboken_version.py")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  ${SHIBOKEN_VERSION_FILE_PATH}
)
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} "${SHIBOKEN_VERSION_FILE_PATH}"
  OUTPUT_VARIABLE SHIBOKEN_VERSION_OUTPUT
  ERROR_VARIABLE SHIBOKEN_VERSION_OUTPUT_ERROR
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if (NOT SHIBOKEN_VERSION_OUTPUT)
    message(FATAL_ERROR "Could not identify shiboken version. \
                         Error: ${SHIBOKEN_VERSION_OUTPUT_ERROR}")
endif()

list(GET SHIBOKEN_VERSION_OUTPUT 0 shiboken_MAJOR_VERSION)
list(GET SHIBOKEN_VERSION_OUTPUT 1 shiboken_MINOR_VERSION)
list(GET SHIBOKEN_VERSION_OUTPUT 2 shiboken_MICRO_VERSION)
# a - alpha, b - beta, rc - rc
list(GET SHIBOKEN_VERSION_OUTPUT 3 shiboken_PRE_RELEASE_VERSION_TYPE)
# the number of the pre release (alpha1, beta3, rc7, etc.)
list(GET SHIBOKEN_VERSION_OUTPUT 4 shiboken_PRE_RELEASE_VERSION)

set(shiboken2_VERSION "${shiboken_MAJOR_VERSION}.${shiboken_MINOR_VERSION}.${shiboken_MICRO_VERSION}")
set(shiboken2_library_so_version "${shiboken_MAJOR_VERSION}.${shiboken_MINOR_VERSION}")

compute_config_py_values(shiboken2_VERSION)

## For debugging the PYTHON* variables
message(STATUS "PYTHONLIBS_FOUND:       " ${PYTHONLIBS_FOUND})
message(STATUS "PYTHON_LIBRARIES:       " ${PYTHON_LIBRARIES})
message(STATUS "PYTHON_INCLUDE_DIRS:    " ${PYTHON_INCLUDE_DIRS})
message(STATUS "PYTHON_DEBUG_LIBRARIES: " ${PYTHON_DEBUG_LIBRARIES})
message(STATUS "PYTHONINTERP_FOUND:     " ${PYTHONINTERP_FOUND})
message(STATUS "PYTHON_EXECUTABLE:      " ${PYTHON_EXECUTABLE})
message(STATUS "PYTHON_VERSION:         " ${PYTHON_VERSION_MAJOR}.${PYTHON_VERSION_MINOR}.${PYTHON_VERSION_PATCH})

if (NOT PYTHON_EXTENSION_SUFFIX)
  get_python_extension_suffix()
endif()

option(FORCE_LIMITED_API "Enable the limited API." "yes")
set(PYTHON_LIMITED_API 0)

shiboken_check_if_limited_api()

if (PYTHON_LIMITED_API)
    set_limited_api()
endif()

if (NOT PYTHON_CONFIG_SUFFIX)
    set_python_config_suffix()
endif()

if (NOT PYTHON_SHARED_LIBRARY_SUFFIX)
    set_python_shared_library_suffix()
endif()

# Currently we don't need this ugly suffix.
set(PYTHON_EXTENSION_SUFFIX "")

message(STATUS "PYTHON_EXTENSION_SUFFIX:      ${PYTHON_EXTENSION_SUFFIX}")
message(STATUS "PYTHON_CONFIG_SUFFIX:         ${PYTHON_CONFIG_SUFFIX}")
message(STATUS "PYTHON_SHARED_LIBRARY_SUFFIX: ${PYTHON_SHARED_LIBRARY_SUFFIX}")


if (NOT PYTHON_SITE_PACKAGES)
    set_python_site_packages()
endif()

set_cmake_cxx_flags()
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D QT_NO_CAST_FROM_ASCII -D QT_NO_CAST_TO_ASCII")

# Force usage of the C++11 standard, without a silent fallback
# to C++98 if the compiler does not support C++11.
if(${QT_MAJOR_VERSION} GREATER_EQUAL 6)
    set(CMAKE_CXX_STANDARD 17)
else()
    set(CMAKE_CXX_STANDARD 11)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LIB_SUFFIX "" CACHE STRING "Define suffix of directory name (32/64)" )
set(LIB_INSTALL_DIR "lib${LIB_SUFFIX}" CACHE PATH "The subdirectory relative to the install \
    prefix where libraries will be installed (default is /lib${LIB_SUFFIX})" FORCE)
set(BIN_INSTALL_DIR "bin" CACHE PATH "The subdirectory relative to the install prefix where \
    dlls will be installed (default is /bin)" FORCE)

if (WIN32)
    set(PATH_SEP "\;")
else()
    set(PATH_SEP ":")
endif()

if(CMAKE_HOST_APPLE)
    set(OSX_USE_LIBCPP "OFF" CACHE BOOL "Explicitly link the libc++ standard library \
        (useful for osx deployment targets lower than 10.9.")
    if(OSX_USE_LIBCPP)
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
    endif()
endif()

# Build with Address sanitizer enabled if requested.
# This may break things, so use at your own risk.
if (SANITIZE_ADDRESS AND NOT MSVC)
    set_sanitize_address()
endif()

# Detect if the python libs were compiled in debug mode
# On Linux distros there is no standard way to check that.
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        is_py_debug = False
        import sys
        try:
            sys_py_debug = sys.pydebug
            if sys_py_debug:
                is_py_debug = True
        except:
            pass

        try:
            from distutils import sysconfig
            config_py_debug = sysconfig.get_config_var('Py_DEBUG')
            if config_py_debug:
                is_py_debug = True
        except:
            pass

        print(bool(is_py_debug))
        "
    OUTPUT_VARIABLE PYTHON_WITH_DEBUG
    OUTPUT_STRIP_TRAILING_WHITESPACE)

# Detect if python interpeter was compiled with COUNT_ALLOCS define
# Linux distros are inconsistent in setting the sysconfig.get_config_var('COUNT_ALLOCS') value
execute_process(
    COMMAND ${PYTHON_EXECUTABLE} -c "if True:
        count_allocs = False
        import sys
        try:
            if sys.getcounts:
                count_allocs = True
        except:
            pass

        print(bool(count_allocs))
        "
    OUTPUT_VARIABLE PYTHON_WITH_COUNT_ALLOCS
    OUTPUT_STRIP_TRAILING_WHITESPACE)

set(SHIBOKEN_BUILD_TYPE "${CMAKE_BUILD_TYPE}")

# We do not want to link against the python shared / static library on Linux and macOS.
# The dynamic linker will pick up the python symbols at runtime automatically.
# On Windows we do need to link against the python.lib import library.
if (NOT SBK_PYTHON_LIBRARIES)
    set(SBK_PYTHON_LIBRARIES "")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set_debug_build()
endif()

######################################################################
# Adding sub directories to build
######################################################################
add_subdirectory(ApiExtractor)

set(generator_plugin_DIR ${LIB_INSTALL_DIR}/generatorrunner${generator_SUFFIX})

# uninstall target
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake"
               "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
               IMMEDIATE @ONLY)
add_custom_target(uninstall "${CMAKE_COMMAND}"
                  -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake")

add_subdirectory(libshiboken)
add_subdirectory(doc)

# deps found, compile the generator.
if (Qt${QT_MAJOR_VERSION}Core_FOUND)
    add_subdirectory(generator)
    add_subdirectory(shibokenmodule)

    if (BUILD_TESTS)
        enable_testing()
        add_subdirectory(tests)
    endif()
else()
    message(WARNING "Some dependencies were not found: shiboken2 generator compilation disabled!")
endif()

add_subdirectory(data)
