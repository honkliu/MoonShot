aux_source_directory(. SOURCEFILES)

add_library(Executor ${SOURCEFILES})

target_include_directories(Executor PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Compiler"
    "${CMAKE_CURRENT_SOURCE_DIR}/../IndexAccess"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Embeddings"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils"
)
