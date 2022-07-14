macro(FETCH_GRPC)

    cmake_minimum_required(VERSION 3.15 FATAL_ERROR)

    # Use CMake's FetchContent module to clone gRPC at configure time. 
    # This makes gRPC's source code available to your project, similar 
    # to a git submodule.
    message(STATUS "Using gRPC via add_subdirectory (FetchContent).")
    include(FetchContent)
    FetchContent_Declare(
        gRPC
        GIT_REPOSITORY https://github.com/grpc/grpc
        GIT_TAG        v1.35.0
    )
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_MakeAvailable(gRPC)

    # Since FetchContent uses add_subdirectory under the hood, we can use
    # the grpc targets directly from this build.
    set(_PROTOBUF_LIBPROTOBUF libprotobuf)
    set(_REFLECTION grpc++_reflection)
    set(_PROTOBUF_PROTOC $<TARGET_FILE:protoc>)
    set(_GRPC_GRPCPP grpc++)
    if(CMAKE_CROSSCOMPILING)
        find_program(_GRPC_CPP_PLUGIN_EXECUTABLE grpc_cpp_plugin)
    else()
        set(_GRPC_CPP_PLUGIN_EXECUTABLE $<TARGET_FILE:grpc_cpp_plugin>)
    endif()

endmacro()


# Generates C++ sources from the .proto files
#
# GRPC_GENERATE_CPP (<PROTO_SRCS> <GRPC_SRCS> <DEST> [<ARGN>...])
#
#  SRCS - variable to define with autogenerated source files
#  HDRS - variable to define with autogenerated header files
#  DEST - directory where the source files will be created
#  ARGN - .proto files
#
function(GRPC_GENERATE_CPP 
    PROTO_SRCS
    GRPC_SRCS
    DEST
    )
  
    if(NOT ARGN)
        message(SEND_ERROR "Error: GRPC_GENERATE_CPP() called without any proto files")
        return()
    endif()

    foreach(FIL ${ARGN})
        get_filename_component(ABS_FIL ${FIL} ABSOLUTE)
        get_filename_component(FIL_WE ${FIL} NAME_WE)
        get_filename_component(ABS_PATH ${ABS_FIL} PATH)

        list(APPEND ${PROTO_SRCS} "${DEST}/${FIL_WE}.pb.cc")
        list(APPEND ${PROTO_HDRS} "${DEST}/${FIL_WE}.pb.h")
        list(APPEND ${GRPC_SRCS} "${DEST}/${FIL_WE}.grpc.pb.cc")
        list(APPEND ${GRPC_HDRS} "${DEST}/${FIL_WE}.grpc.pb.h")

        add_custom_command(
            OUTPUT
                "${DEST}/${FIL_WE}.pb.cc"
                "${DEST}/${FIL_WE}.pb.h" 
                "${DEST}/${FIL_WE}.grpc.pb.cc"
                "${DEST}/${FIL_WE}.grpc.pb.h"
            COMMAND 
                ${_PROTOBUF_PROTOC}
                ARGS 
                    --grpc_out "${DEST}" 
                    --cpp_out "${DEST}"
                    -I "${ABS_PATH}"
                    --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN_EXECUTABLE}
                ${ABS_FIL}
            DEPENDS 
                ${ABS_FIL} 
                ${_GRPC_CPP_PLUGIN_EXECUTABLE}
            COMMENT "Running C++ gRPC compiler on ${FIL}"
            VERBATIM 
        )
    endforeach()

    set(${PROTO_SRCS} ${${PROTO_SRCS}} PARENT_SCOPE)
    set(${GRPC_SRCS} ${${GRPC_SRCS}} PARENT_SCOPE)

endfunction()