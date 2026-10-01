cmake_minimum_required(VERSION 3.19)

find_program(CARGO cargo REQUIRED
    HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    DOC "Rust cargo package manager")

message(STATUS "cargo found: ${CARGO}")

set(CARGO_PROFILE "$<IF:$<CONFIG:Debug>,dev,release>")
set(CARGO_SUBDIR "$<IF:$<CONFIG:Debug>,debug,release>")

set(CARGO_TARGET_DIR "${MOONSHOT_CONFIG_OUTPUT_DIR}/rust")
set(MOONSHOT_RUST_TARGET "" CACHE STRING "Rust target triple (required for non-Windows cross builds)")
if(MOONSHOT_RUST_TARGET)
    set(CARGO_TARGET_TRIPLE "${MOONSHOT_RUST_TARGET}")
elseif(WIN32)
    if(MOONSHOT_TARGET_ARCH STREQUAL "x64")
        set(rust_arch x86_64)
    elseif(MOONSHOT_TARGET_ARCH STREQUAL "x86")
        set(rust_arch i686)
    elseif(MOONSHOT_TARGET_ARCH STREQUAL "arm64")
        set(rust_arch aarch64)
    else()
        message(FATAL_ERROR "No default Rust Windows target for ${MOONSHOT_TARGET_ARCH}")
    endif()
    if(MSVC)
        set(CARGO_TARGET_TRIPLE "${rust_arch}-pc-windows-msvc")
    elseif(MINGW)
        set(CARGO_TARGET_TRIPLE "${rust_arch}-pc-windows-gnu")
    else()
        message(FATAL_ERROR "Set MOONSHOT_RUST_TARGET for this Windows toolchain")
    endif()
else()
    find_program(RUSTC rustc REQUIRED
        HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin")
    execute_process(COMMAND "${RUSTC}" -vV
        OUTPUT_VARIABLE rust_version ERROR_VARIABLE rust_error RESULT_VARIABLE rust_result)
    if(NOT rust_result EQUAL 0 OR NOT rust_version MATCHES "host: ([^\r\n]+)")
        message(FATAL_ERROR "Could not determine the Rust host target: ${rust_error}")
    endif()
    set(CARGO_TARGET_TRIPLE "${CMAKE_MATCH_1}")
    if(CMAKE_CROSSCOMPILING)
        message(FATAL_ERROR
            "Set MOONSHOT_RUST_TARGET to match the C++ cross-compilation toolchain.")
    endif()
endif()
string(REGEX REPLACE "-.*$" "" rust_arch "${CARGO_TARGET_TRIPLE}")
moonshot_normalize_arch("${rust_arch}" rust_arch)
if(NOT rust_arch STREQUAL MOONSHOT_TARGET_ARCH)
    message(FATAL_ERROR
        "Rust target '${CARGO_TARGET_TRIPLE}' does not match C++ target "
        "'${MOONSHOT_TARGET_ARCH}'. Set MOONSHOT_RUST_TARGET to a matching triple.")
endif()
message(STATUS "MoonShot Rust target: ${CARGO_TARGET_TRIPLE}")
set(CARGO_TARGET_ARGUMENTS --target "${CARGO_TARGET_TRIPLE}")
set(RUST_BIN_DIR "${CARGO_TARGET_DIR}/${CARGO_TARGET_TRIPLE}/${CARGO_SUBDIR}")

add_custom_target(rustblade ALL
    COMMAND ${CARGO} build --profile ${CARGO_PROFILE} ${CARGO_TARGET_ARGUMENTS}
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --target-dir    "${CARGO_TARGET_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building RustBlade (Rust / cargo)"
    VERBATIM
)

add_custom_command(TARGET rustblade POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MOONSHOT_CONFIG_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RUST_BIN_DIR}/moon_rs${CMAKE_EXECUTABLE_SUFFIX}"
        "${MOONSHOT_CONFIG_OUTPUT_DIR}/moon_rs${CMAKE_EXECUTABLE_SUFFIX}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RUST_BIN_DIR}/shennong_rs${CMAKE_EXECUTABLE_SUFFIX}"
        "${MOONSHOT_CONFIG_OUTPUT_DIR}/shennong_rs${CMAKE_EXECUTABLE_SUFFIX}"
    VERBATIM
)

add_custom_target(rustblade_demo ALL
    COMMAND ${CARGO} build --profile ${CARGO_PROFILE} ${CARGO_TARGET_ARGUMENTS} --example demo
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
            --target-dir    "${CARGO_TARGET_DIR}"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building RustBlade demo example"
    VERBATIM
    DEPENDS rustblade
)

add_custom_command(TARGET rustblade_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MOONSHOT_CONFIG_OUTPUT_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RUST_BIN_DIR}/examples/demo${CMAKE_EXECUTABLE_SUFFIX}"
            "${MOONSHOT_CONFIG_OUTPUT_DIR}/rustblade_demo${CMAKE_EXECUTABLE_SUFFIX}"
    VERBATIM
)
