cmake_minimum_required(VERSION 3.5.1)

project(MoonShot)

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
else()
  add_definitions(-D_WIN32_WINNT=0x600)
endif()

find_package(Threads REQUIRED)
#find_package(Boost REQUIRED COMPONENTS system)

include(../cmake/grpc.cmake)
include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 11)

# if(GRPC_FETCHCONTENT)
# Another way is to use CMake's FetchContent module to clone gRPC at
# configure time. This makes gRPC's source code available to your project,
# similar to a git submodule.
message(STATUS "Using gRPC via add_subdirectory (FetchContent).")
include(FetchContent)
FetchContent_Declare(
  gRPC
  GIT_REPOSITORY https://github.com/grpc/grpc
  GIT_TAG        v1.35.0 # e.g v1.28.0
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

message (BOOST, ${Boost_INCLUDE_DIRS} ${Boost_LIBRARIES})

# Proto file
#get_filename_component(PROTO_FILES "MatchService.proto" ABSOLUTE)

list(APPEND PROTO_FILES 
        MatchService.proto
    )

    set(CMAKE_CXX_STANDARD 11)

GRPC_GENERATE_CPP(
    PROTO_SRCS
    GRPC_SRCS
    ${CMAKE_CURRENT_BINARY_DIR}
    ${PROTO_FILES}
)
aux_source_directory(. SOURCEFILES)

include_directories(
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../thirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

set(MOONSHOT MoonShot)
foreach(_target
    MoonShot
)
    add_executable(${_target} 
        ${SOURCEFILES}
        ${GRPC_SRCS}
        ${PROTO_SRCS})
    target_link_libraries(${_target}
        ${_REFLECTION}
        ${_GRPC_GRPCPP}
        ${_PROTOBUF_LIBPROTOBUF})
endforeach()

##add_executable(${MoonShot} ${PROTO_SRCS} ${GRPC_SRCS})
#target_link_libraries(${MOONSHOT}
#    ${_REFLECTION}
##    ${_GRPC_GRPCPP}
#    ${_PROTOBUF_LIBPROTOBUF}
#)
#add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../Executor)

#include_directories(
#    PUBLIC
#    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/include"
#    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
#    "${CMAKE_CURRENT_LIST_DIR}"
#    "${CMAKE_CURRENT_BINARY_DIR}"
#)
#target_include_directories(MoonShot
#    PUBLIC
#    ${CMAKE_CURRENT_BINARY_DIR}
#)

#add_executable(MoonShot ${SOURCEFILES})

target_link_libraries(MoonShot grpc++)

#add_executable()
# Generated sources
#set(hw_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.pb.cpp")
##set(hw_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.pb.h")
#set(hw_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.grpc.pb.cpp")
#set(hw_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.grpc.pb.h")
#add_custom_command(
#      OUTPUT "${hw_proto_srcs}" "${hw_proto_hdrs}" "${hw_grpc_srcs}" "${hw_grpc_hdrs}"
#      COMMAND ${_PROTOBUF_PROTOC}
#      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
#        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
#        -I "${hw_proto_path}"
#        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
##        "${hw_proto}"
#      DEPENDS "${hw_proto}")

# Include generated *.pb.h files
