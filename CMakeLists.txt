cmake_minimum_required(VERSION 3.10)
project(MoonShot)

#include(FetchContent)
#FetchContent_Declare(
#  gRPC
#  GIT_REPOSITORY https://github.com/grpc/grpc
#  GIT_TAG        v1.35.0 # e.g v1.28.0
#)
#set(FETCHCONTENT_QUIET OFF)
#FetchContent_MakeAvailable(gRPC)

add_subdirectory(Executor)
add_subdirectory(IndexAccess)
add_subdirectory(Service)

include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/include"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}"

)
