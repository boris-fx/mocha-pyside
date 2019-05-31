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
target_link_libraries(shiboken2 apiextractor Qt5::Core)
if (NOT DISABLE_DOCSTRINGS)
    target_sources(shiboken2 PRIVATE qtdoc/qtdocgenerator.cpp)
    target_compile_definitions(shiboken2 PUBLIC DOCSTRINGS_ENABLED)
endif()


if(APPLE)
    file(RELATIVE_PATH EXTRA_RPATH "${CMAKE_INSTALL_PREFIX}/bin" "${CLANG_EXTRA_LIBRARIES}")
    get_filename_component(EXTRA_RPATH ${EXTRA_RPATH} DIRECTORY)
    set(EXTRA_RPATH "@executable_path/${EXTRA_RPATH}")

    message(STATUS "Extra RPATH for the clang lib(s): ${EXTRA_RPATH}")
    add_custom_command(TARGET shiboken2 
        POST_BUILD COMMAND 
            ${CMAKE_INSTALL_NAME_TOOL} -add_rpath "${EXTRA_RPATH}" $<TARGET_FILE:shiboken2>)
endif()

if(WIN32)
    # See full discussion here https://borisfx.slack.com/archives/C2DTQDHF1/p1559287558003800
    set(LIBS_TO_COPY libclang;libcrypto;libssl;Qt5Core;Qt5Network;Qt5XmlPatterns)
    foreach(LIB_NAME ${LIBS_TO_COPY})
        unset(FOUND_LIB CACHE)
        find_file(
            FOUND_LIB
            NAMES ${LIB_NAME}.dll ${LIB_NAME}d.dll
            HINTS ENV PATH
            NO_DEFAULT_PATH)
        if(NOT FOUND_LIB)
            message(AUTHOR_WARNING "Coundn't find ${LIB_NAME}")
            continue()
        endif()
        add_custom_command(
            TARGET 
                shiboken2
            POST_BUILD 
                COMMAND 
                    ${CMAKE_COMMAND}
                ARGS
                    -E copy "${FOUND_LIB}" $<TARGET_FILE_DIR:shiboken2>
            COMMENT "Copying ${LIB_NAME}..."
            )
        install(
            FILES 
                "${FOUND_LIB}"
            DESTINATION
                bin
            )
    endforeach()
endif()

configure_file(shibokenconfig.h.in "${CMAKE_CURRENT_BINARY_DIR}/shibokenconfig.h" @ONLY)

install(TARGETS shiboken2
        EXPORT Shiboken2Targets
        DESTINATION bin)

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
