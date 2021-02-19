cmake_minimum_required(VERSION 2.8)
project(MoonShot)

add_subdirectory(Executor)
add_subdirectory(Service)

include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/include"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}"

)
