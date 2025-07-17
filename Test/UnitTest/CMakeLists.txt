
include(CheckFunctionExists)
include(CheckCXXSourceCompiles)
include(CheckLibraryExists)
include(CPack)

enable_testing()

file(GLOB SOURCEFILES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
# 显式过滤掉 Search.cpp
list(FILTER SOURCEFILES EXCLUDE REGEX ".*/Search\.cpp$")
#list(FILTER SOURCEFILES EXCLUDE REGEX ".*/UnitTestMain\.cpp$")

#add_library(UnitTest ${SOURCEFILES})

# Create executable from Search.cpp
add_executable(Search Search.cpp)

add_executable(UnitTestRunner ${SOURCEFILES})

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

set(common_libs
    Tokenizer           # 确保在 Tokenizer/CMakeLists.txt 中有定义
    IndexAccess         # 确保在 IndexAccess/CMakeLists.txt 中有定义
    Executor            # 确保在 Executor/CMakeLists.txt 中有定义
    Utils
    stdc++fs            # 文件系统库 (GCC)
)

target_link_libraries(Search PRIVATE ${common_libs})
target_link_libraries(UnitTestRunner PRIVATE ${common_libs})

