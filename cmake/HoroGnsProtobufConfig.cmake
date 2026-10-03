# Narrow build-tree package for GNS's protobuf_generate_cpp call.  The targets
# are provided by the exact-revision protobuf source in Dependencies.cmake.
if(NOT TARGET protobuf::libprotobuf OR NOT TARGET protobuf::protoc)
    message(FATAL_ERROR "The optional GNS target requires its in-tree protobuf and protoc targets")
endif()
set(Protobuf_FOUND TRUE)
set(Protobuf_VERSION "3.21.12")

function(protobuf_generate_cpp out_sources out_headers)
    set(_sources)
    set(_headers)
    foreach(_proto IN LISTS ARGN)
        get_filename_component(_absolute "${_proto}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        get_filename_component(_name "${_proto}" NAME_WE)
        set(_generated_directory "${CMAKE_CURRENT_BINARY_DIR}")
        set(_source "${_generated_directory}/${_name}.pb.cc")
        set(_header "${_generated_directory}/${_name}.pb.h")
        add_custom_command(OUTPUT "${_source}" "${_header}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_generated_directory}"
            COMMAND "$<TARGET_FILE:protobuf::protoc>" "--cpp_out=${CMAKE_CURRENT_BINARY_DIR}"
                    "-I${CMAKE_CURRENT_SOURCE_DIR}/common" "-I${CMAKE_CURRENT_SOURCE_DIR}" "${_absolute}"
            DEPENDS protobuf::protoc "${_absolute}"
            VERBATIM)
        list(APPEND _sources "${_source}")
        list(APPEND _headers "${_header}")
    endforeach()
    set(${out_sources} "${_sources}" PARENT_SCOPE)
    set(${out_headers} "${_headers}" PARENT_SCOPE)
endfunction()
