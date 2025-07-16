
include(CheckFunctionExists)
include(CheckCXXSourceCompiles)
include(CheckLibraryExists)
include(CPack)

enable_testing()

# Create library from all sources except Search.cpp
aux_source_directory(. SOURCEFILES)
list(REMOVE_ITEM SOURCEFILES "${CMAKE_CURRENT_SOURCE_DIR}/Search.cpp")

add_library(UnitTest ${SOURCEFILES})

# Create executable from Search.cpp
add_executable(Search Search.cpp)

include_directories(
    PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../thirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../../ThirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../../Executor"
    "${CMAKE_CURRENT_LIST_DIR}/../../Compiler"
    "${CMAKE_CURRENT_LIST_DIR}/../../IndexAccess"
    "${CMAKE_CURRENT_LIST_DIR}/../../Tokenizer"
    "${CMAKE_CURRENT_LIST_DIR}/../../Embeddings"
    "${CMAKE_CURRENT_LIST_DIR}/../../Configuration"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

# Apply include directories to both targets
target_include_directories(UnitTest PUBLIC
    "${CMAKE_CURRENT_LIST_DIR}/../../thirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../../ThirdParty/boost_1_75_0"
    "${CMAKE_CURRENT_LIST_DIR}/../../Executor"
    "${CMAKE_CURRENT_LIST_DIR}/../../Compiler"
    "${CMAKE_CURRENT_LIST_DIR}/../../IndexAccess"
    "${CMAKE_CURRENT_LIST_DIR}/../../Tokenizer"
    "${CMAKE_CURRENT_LIST_DIR}/../../Embeddings"
    "${CMAKE_CURRENT_LIST_DIR}/../../Configuration"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)


