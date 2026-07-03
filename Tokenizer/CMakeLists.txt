cmake_minimum_required(VERSION 3.10)

find_package(ICU REQUIRED COMPONENTS uc i18n)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(PC_LIBSTEMMER QUIET libstemmer)
endif()

find_path(LIBSTEMMER_INCLUDE_DIR
    NAMES libstemmer.h
    HINTS ${PC_LIBSTEMMER_INCLUDE_DIRS}
)

find_library(LIBSTEMMER_LIBRARY_RELEASE
    NAMES stemmer
    HINTS
        ${PC_LIBSTEMMER_LIBRARY_DIRS}
        "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib"
)

find_library(LIBSTEMMER_LIBRARY_DEBUG
    NAMES stemmer
    HINTS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib"
)

if(NOT LIBSTEMMER_INCLUDE_DIR OR NOT LIBSTEMMER_LIBRARY_RELEASE)
    message(FATAL_ERROR
        "libstemmer was not found. Install the system package instead of relying on vcpkg on Linux, e.g. "
        "'sudo apt-get install libstemmer-dev' (Debian/Ubuntu), or install the Snowball/libstemmer development package for your distro.")
endif()

if(NOT LIBSTEMMER_LIBRARY_DEBUG)
    set(LIBSTEMMER_LIBRARY_DEBUG ${LIBSTEMMER_LIBRARY_RELEASE})
endif()

aux_source_directory(. SOURCEFILES)

add_library(Tokenizer ${SOURCEFILES})

target_include_directories(Tokenizer PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${ICU_INCLUDE_DIRS}>
    $<BUILD_INTERFACE:${LIBSTEMMER_INCLUDE_DIR}>
)

target_link_libraries(Tokenizer PUBLIC
    ICU::uc
    ICU::i18n
    optimized ${LIBSTEMMER_LIBRARY_RELEASE}
    debug ${LIBSTEMMER_LIBRARY_DEBUG}
)
