aux_source_directory(. SOURCEFILES)

add_library(IndexAccess ${SOURCEFILES})

target_include_directories(IndexAccess PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Executor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Compiler"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Tokenizer"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Utils"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Embeddings"
    "${CMAKE_CURRENT_SOURCE_DIR}/../Configuration"
)

target_link_libraries(IndexAccess PRIVATE Utils Tokenizer)
