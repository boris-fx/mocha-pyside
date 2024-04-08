project(shibokenmodule)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/shibokenmodule.txt.in"
               "${CMAKE_CURRENT_BINARY_DIR}/shibokenmodule.txt" @ONLY)

set(sample_SRC ${CMAKE_CURRENT_BINARY_DIR}/shiboken2/shiboken2_module_wrapper.cpp)

set(shibokenmodule_TYPESYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/typesystem_shiboken.xml)

if(UNIX)
   set(ENV_VARS "LD_LIBRARY_PATH=$ENV{LD_LIBRARY_PATH}")
   if(APPLE)
      set(ENV_VARS "${ENV_VARS} DYLD_LIBRARY_PATH=$ENV{DYLD_LIBRARY_PATH}")
      set(ENV_VARS "${ENV_VARS} DYLD_FRAMEWORK_PATH=$ENV{DYLD_FRAMEWORK_PATH}")
   endif()
   add_custom_command(OUTPUT ${sample_SRC}
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mjb_rejected_classes.log"
      BYPRODUCTS ${sample_SRC}
      COMMAND ${CMAKE_COMMAND} -E env ${ENV_VARS} $<TARGET_FILE:shiboken2> --project-file=${CMAKE_CURRENT_BINARY_DIR}/shibokenmodule.txt ${GENERATOR_EXTRA_FLAGS}
      DEPENDS ${shibokenmodule_TYPESYSTEM}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Running generator for 'shiboken2'..."
   )
elseif(WIN32)
   # Not using EXPORT_ENV_CMD because stupid CMake would interpret each PATH element as list item
   # and escape it with quotes in the command.
   # E.g
   # set(EXPORT_ENV_CMD "set PATH=$ENV{PATH}")
   # add_custom_command(... "${EXPORT_ENV_CMD}"...
   # leads to '"set PATH=path1";"path2";"path3"' command in the project file
   # Replacing ; with $<SEMICOLON> and/or passing VERBATIM flag didn't help
   # the command got invalid in any case
   # Omitting the quotes around ${EXPORT_ENV_CMD} within add_custom_command
   # leads to 'set PATH=path1 path2 path3' command in the project file
   add_custom_command(OUTPUT ${sample_SRC}
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mjb_rejected_classes.log"
      BYPRODUCTS ${sample_SRC}
      COMMAND set "\"PATH=$ENV{PATH}\"" && $<TARGET_FILE:shiboken2> --project-file=${CMAKE_CURRENT_BINARY_DIR}/shibokenmodule.txt ${GENERATOR_EXTRA_FLAGS}
      DEPENDS ${shibokenmodule_TYPESYSTEM}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Running generator for 'shiboken2'..."
   )
endif()



add_library(shibokenmodule MODULE ${sample_SRC})
target_include_directories(shibokenmodule PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                  ${CMAKE_SOURCE_DIR})
set_property(TARGET shibokenmodule PROPERTY PREFIX "")
set_property(TARGET shibokenmodule PROPERTY OUTPUT_NAME "shiboken2${PYTHON_EXTENSION_SUFFIX}")

if(WIN32)
    set_property(TARGET shibokenmodule PROPERTY SUFFIX ".pyd")
endif()
target_link_libraries(shibokenmodule PUBLIC libshiboken)

create_generator_target(shibokenmodule)

install(TARGETS shibokenmodule DESTINATION ${PYTHON_SITE_PACKAGES}/shiboken2)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/_config.py.in"
               "${CMAKE_CURRENT_BINARY_DIR}/_config.py" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_config.py"
        DESTINATION "${PYTHON_SITE_PACKAGES}/shiboken2")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/__init__.py.in"
               "${CMAKE_CURRENT_BINARY_DIR}/__init__.py" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Shiboken.pyi"
               "${CMAKE_CURRENT_BINARY_DIR}/Shiboken.pyi" @ONLY)
# typing support for mypy
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/py.typed.in"
               "${CMAKE_CURRENT_BINARY_DIR}/py.typed" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/Shiboken.pyi"
              "${CMAKE_CURRENT_BINARY_DIR}/py.typed"
        DESTINATION "${PYTHON_SITE_PACKAGES}/shiboken2")

# Variable from enclosing scope.
foreach(item IN LISTS shiboken_python_files)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/files.dir/shibokensupport/${item}"
                   "${CMAKE_CURRENT_BINARY_DIR}/files.dir/shibokensupport/${item}" COPYONLY)
endforeach()

install(DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/files.dir"
        DESTINATION "${PYTHON_SITE_PACKAGES}/shiboken2")

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/__init__.py"
       DESTINATION "${PYTHON_SITE_PACKAGES}/shiboken2")

# Use absolute path instead of relative path, to avoid ninja build errors due to
# duplicate file dependency inconsistency.
set(shiboken_version_relative_path "${CMAKE_CURRENT_SOURCE_DIR}/../shiboken_version.py")
get_filename_component(shiboken_version_path ${shiboken_version_relative_path} ABSOLUTE)
configure_file("${shiboken_version_path}"
               "${CMAKE_CURRENT_BINARY_DIR}/_git_shiboken_module_version.py" @ONLY)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/_git_shiboken_module_version.py"
        DESTINATION "${PYTHON_SITE_PACKAGES}/shiboken2")
