cmake_minimum_required(VERSION 3.10)

enable_testing()

file(GLOB SOURCEFILES "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
list(FILTER SOURCEFILES EXCLUDE REGEX ".*/Search\\.cpp$")

add_executable(UnitTestRunner ${SOURCEFILES})
add_executable(Search Search.cpp)

target_include_directories(UnitTestRunner PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Executor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Compiler"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../IndexAccess"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Tokenizer"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Embeddings"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Configuration"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Utils"
)

target_include_directories(Search PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Executor"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Compiler"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../IndexAccess"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Tokenizer"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Embeddings"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Configuration"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../Utils"
)

set(common_libs IndexAccess Executor Tokenizer Utils)

target_link_libraries(UnitTestRunner PRIVATE ${common_libs})
target_link_libraries(Search        PRIVATE ${common_libs})
