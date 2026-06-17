cmake_minimum_required(VERSION 3.10)

if(DEFINED ENV{VCPKG_ROOT} AND NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
endif()

project(MoonShot VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default install prefix to <build>/sdk so that
# "cmake --install build" works without --prefix.
# An explicit --prefix on the command line overrides this.
if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/sdk"
        CACHE PATH "Install prefix (default: <build>/sdk)" FORCE)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(MSVC)
    # Prevent windows.h from defining min/max macros.
    # Silence MSVC deprecation warnings on C standard functions.
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
    add_compile_options(/W3 /wd4996)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# -------------------------------------------------------------------------
# mimalloc — mandatory high-performance allocator override.
# -------------------------------------------------------------------------
find_package(mimalloc REQUIRED)
message(STATUS "mimalloc ${mimalloc_VERSION} found")

find_package(Threads REQUIRED)

# -------------------------------------------------------------------------
# Component libraries
# -------------------------------------------------------------------------
add_subdirectory(Utils)
add_subdirectory(Tokenizer)
add_subdirectory(Executor)
add_subdirectory(IndexAccess)

# -------------------------------------------------------------------------
# MoonShot — the single library target that downstream projects link.
#
# It is an INTERFACE target that bundles all four component libraries.
# Consumers only need:
#
#   target_link_libraries(my_app PRIVATE MoonShot::MoonShot)
# -------------------------------------------------------------------------
add_library(MoonShot INTERFACE)
add_library(MoonShot::MoonShot ALIAS MoonShot)

target_link_libraries(MoonShot INTERFACE
    IndexAccess
    Executor
    Tokenizer
    Utils
)

if(TARGET mimalloc-static)
    target_link_libraries(MoonShot INTERFACE mimalloc-static)
else()
    target_link_libraries(MoonShot INTERFACE mimalloc)
endif()

target_link_libraries(MoonShot INTERFACE Threads::Threads)

target_include_directories(MoonShot INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/IndexAccess>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Executor>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Compiler>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Tokenizer>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Embeddings>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Configuration>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/Utils>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/moonshot>
)

# -------------------------------------------------------------------------
# Examples (always built when this is the top-level project)
# -------------------------------------------------------------------------
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    add_subdirectory(examples)
    add_subdirectory(RustBlade)
    add_subdirectory(Service)
    add_subdirectory(Test/UnitTest)
    add_subdirectory(Tools)
endif()

# -------------------------------------------------------------------------
# Install rules
# -------------------------------------------------------------------------

# Public headers
install(FILES include/moonshot.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

install(FILES
    IndexAccess/PostingStore.h
    IndexAccess/IndexReader.h
    IndexAccess/IndexWriter.h
    IndexAccess/AdvancedIndexWriter.h
    IndexAccess/AdvancedIndexReader.h
    IndexAccess/IndexReaderImpl.h
    IndexAccess/IsrImpl.h
    IndexAccess/BlockTable.h
    IndexAccess/FileBlockManager.h
    IndexAccess/ElementFilter.h
    IndexAccess/HashFunctions.h
    IndexAccess/MemOperation.h
    IndexAccess/IndexSerializer.h
    IndexAccess/SearchResult.h
    IndexAccess/UnifiedDecoder.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

install(FILES
    Compiler/EvalExpression.h
    Compiler/IndexSearchCompiler.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

install(FILES
    Executor/IndexSearchExecutor.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

install(FILES
    Tokenizer/Tokenizer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

install(FILES
    Embeddings/Embeddings.h
    Configuration/ConfigParameters.h
    Utils/FileAccess.h
    Utils/Constants.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/moonshot)

# Libraries
install(TARGETS Utils Tokenizer Executor IndexAccess
    EXPORT MoonShotTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS MoonShot
    EXPORT MoonShotTargets)

# CMake package config
install(EXPORT MoonShotTargets
    FILE MoonShotTargets.cmake
    NAMESPACE MoonShot::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonShot)

configure_package_config_file(
    cmake/MoonShotConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/MoonShotConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonShot)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/MoonShotConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/MoonShotConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/MoonShotConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/MoonShot)
