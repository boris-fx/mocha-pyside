project(shibokengenerator)

find_package(Qt5Xml)
find_package(Qt5XmlPatterns)

set(shiboken2_SRC
generator.cpp
shiboken2/cppgenerator.cpp
shiboken2/headergenerator.cpp
shiboken2/overloaddata.cpp
shiboken2/shibokengenerator.cpp
main.cpp
)

add_executable(shiboken2 ${shiboken2_SRC})
add_executable(Shiboken2::shiboken2 ALIAS shiboken2)
add_dependencies(shiboken2 apiextractor)
set_target_properties(shiboken2 PROPERTIES OUTPUT_NAME shiboken2${shiboken2_SUFFIX})
target_include_directories(shiboken2 PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/shiboken2
                    ${CMAKE_CURRENT_SOURCE_DIR}/qtdoc
                    ${CMAKE_CURRENT_SOURCE_DIR}
                    ${CMAKE_CURRENT_BINARY_DIR}
                    ${apiextractor_SOURCE_DIR}
                    )
target_link_libraries(shiboken2 apiextractor Qt${QT_MAJOR_VERSION}::Core)
if (NOT DISABLE_DOCSTRINGS)
    target_sources(shiboken2 PRIVATE qtdoc/qtdocgenerator.cpp)
    target_compile_definitions(shiboken2 PUBLIC DOCSTRINGS_ENABLED)
endif()


if(APPLE)
    # Add relative rpath to LLVM's libclang.dylib.
    # If we add an abolute path to libclang, it will be removed during the CMake install phase.
    file(RELATIVE_PATH EXTRA_RPATH "${CMAKE_INSTALL_PREFIX}/bin" "${CLANG_EXTRA_LIBRARIES}")
    get_filename_component(EXTRA_RPATH ${EXTRA_RPATH} DIRECTORY)
    set(EXTRA_RPATH "@executable_path/${EXTRA_RPATH}")

    message(STATUS "Extra RPATH for the clang lib(s): ${EXTRA_RPATH}")
    add_custom_command(TARGET shiboken2 
        POST_BUILD COMMAND 
            ${CMAKE_INSTALL_NAME_TOOL} -add_rpath "${EXTRA_RPATH}" $<TARGET_FILE:shiboken2>
        COMMENT "Adding extra RPATH for the clang libs: ${EXTRA_RPATH}"
    )
endif()

if(WIN32)
    # See full discussion here https://borisfx.slack.com/archives/C2DTQDHF1/p1559287558003800
    unset(found_libs)

    foreach(qt_module_name Core;Network;XmlPatterns)
        unset(library_location)         # Both calls to unset() are required
        unset(library_location CACHE)
        get_target_property(library_location Qt5::${qt_module_name} LOCATION)
        if(NOT library_location)
            message(FATAL_ERROR "Could not find: Qt5 ${qt_module_name}")
            continue()
        endif()
        list(APPEND found_libs ${library_location})
    endforeach()

    # For the remaining libs, search for them in the PATH environment variable
    foreach(lib_name libclang;libcrypto-3-x64;libssl-3-x64)
        unset(library_location)         # Both calls to unset() are required
        unset(library_location CACHE)
        find_file(
            library_location
            NAMES ${lib_name}.dll
            HINTS ENV PATH
            NO_DEFAULT_PATH NO_CACHE)
        if(NOT library_location)
            message(FATAL_ERROR "Could not find: ${lib_name}")
            continue()
        endif()
        list(APPEND found_libs ${library_location})
    endforeach()

    foreach(found_lib ${found_libs})
        add_custom_command(
            TARGET shiboken2
            POST_BUILD 
            COMMAND ${CMAKE_COMMAND}
            ARGS -E copy "${found_lib}" $<TARGET_FILE_DIR:shiboken2>
            COMMENT "Copying ${found_lib}..."
        )
        install(
            FILES "${found_lib}"
            DESTINATION bin
        )
    endforeach()
endif()

configure_file(shibokenconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/shibokenconfig.h" @ONLY)

install(TARGETS shiboken2
        EXPORT Shiboken2Targets
        DESTINATION "${CMAKE_INSTALL_PREFIX}/bin")

set(shiboken_generator_package_name "shiboken2_generator")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/_config.py.in"
               "${CMAKE_CURRENT_BINARY_DIR}/_config.py" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_config.py"
        DESTINATION "${PYTHON_SITE_PACKAGES}/${shiboken_generator_package_name}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/__init__.py.in"
               "${CMAKE_CURRENT_BINARY_DIR}/__init__.py" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/__init__.py"
       DESTINATION "${PYTHON_SITE_PACKAGES}/${shiboken_generator_package_name}")

# shiboken2 setuptools entry point
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/../shiboken_tool.py
        DESTINATION bin
        PERMISSIONS
        OWNER_EXECUTE OWNER_WRITE OWNER_READ
        GROUP_EXECUTE GROUP_READ
        WORLD_EXECUTE WORLD_READ)

# Use absolute path instead of relative path, to avoid ninja build errors due to
# duplicate file dependency inconsistency.
set(shiboken_version_relative_path "${CMAKE_CURRENT_SOURCE_DIR}/../shiboken_version.py")
get_filename_component(shiboken_version_path ${shiboken_version_relative_path} ABSOLUTE)
configure_file("${shiboken_version_path}"
               "${CMAKE_CURRENT_BINARY_DIR}/_git_shiboken_generator_version.py" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_git_shiboken_generator_version.py"
        DESTINATION "${PYTHON_SITE_PACKAGES}/${shiboken_generator_package_name}")
