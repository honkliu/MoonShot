#project(Executor)

aux_source_directory(. SOURCEFILES)

add_library(IndexAccess ${SOURCEFILES})

set(CMAKE_CXX_STANDARD 11)

include_directories(
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../thirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../ThirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}/../Compiler"
    "${CMAKE_CURRENT_LIST_DIR}/../Tokenizer"
    "${CMAKE_CURRENT_LIST_DIR}/../Configuration"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
