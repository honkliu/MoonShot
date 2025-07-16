#project(Executor)

aux_source_directory(. SOURCEFILES)

add_library(Executor ${SOURCEFILES})

include_directories(
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../Compiler"
    "${CMAKE_CURRENT_LIST_DIR}/../IndexAccess"
)