cmake_minimum_required(VERSION 3.19)

find_program(CARGO cargo REQUIRED
    HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    DOC "Rust cargo package manager")

message(STATUS "cargo found: ${CARGO}")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CARGO_PROFILE "")
    set(CARGO_SUBDIR  "debug")
else()
    set(CARGO_PROFILE "--release")
    set(CARGO_SUBDIR  "release")
endif()

set(CARGO_TARGET_DIR "${MOONSHOT_CONFIG_OUTPUT_DIR}/rust")
set(RUST_BIN_DIR "${CARGO_TARGET_DIR}/${CARGO_SUBDIR}")

add_custom_target(rustblade ALL
    COMMAND ${CARGO} build ${CARGO_PROFILE}
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
    COMMAND ${CARGO} build ${CARGO_PROFILE} --example demo
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
