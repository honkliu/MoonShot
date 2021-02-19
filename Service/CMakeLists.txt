cmake_minimum_required(VERSION 2.8)
project(MoonShot)
aux_source_directory(. SOURCEFILES)

#add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../Executor)

include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/include"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
)
#add_executable(MoonShot ${SOURCEFILES})

#target_link_libraries(MoonShot Executor)