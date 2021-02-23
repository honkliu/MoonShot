cmake_minimum_required(VERSION 2.8)
project(MoonShot)
aux_source_directory(. SOURCEFILES)

#add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../Executor)

include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/include"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}"

)

add_executable(MoonShot ${SOURCEFILES})

target_link_libraries(MoonShot Executor)

# Copyright 2018 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# cmake build file for C++ helloworld example.
# Assumes protobuf and gRPC have been installed using cmake.
# See cmake_externalproject/CMakeLists.txt for all-in-one cmake build
# that automatically builds all the dependencies before building helloworld.

cmake_minimum_required(VERSION 3.5.1)

project(HelloWorld C CXX)

if(NOT MSVC)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
else()
  add_definitions(-D_WIN32_WINNT=0x600)
endif()

find_package(Threads REQUIRED)


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

# Proto file
get_filename_component(hw_proto "MatchService.proto" ABSOLUTE)
get_filename_component(hw_proto_path "${hw_proto}" PATH)

# Generated sources
set(hw_proto_srcs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.pb.cpp")
set(hw_proto_hdrs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.pb.h")
set(hw_grpc_srcs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.grpc.pb.cpp")
set(hw_grpc_hdrs "${CMAKE_CURRENT_BINARY_DIR}/ShenNong.grpc.pb.h")
add_custom_command(
      OUTPUT "${hw_proto_srcs}" "${hw_proto_hdrs}" "${hw_grpc_srcs}" "${hw_grpc_hdrs}"
      COMMAND ${_PROTOBUF_PROTOC}
      ARGS --grpc_out "${CMAKE_CURRENT_BINARY_DIR}"
        --cpp_out "${CMAKE_CURRENT_BINARY_DIR}"
        -I "${hw_proto_path}"
        --plugin=protoc-gen-grpc="${_GRPC_CPP_PLUGIN_EXECUTABLE}"
        "${hw_proto}"
      DEPENDS "${hw_proto}")

# Include generated *.pb.h files
include_directories("${CMAKE_CURRENT_BINARY_DIR}")
