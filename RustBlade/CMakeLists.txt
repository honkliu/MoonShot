cmake_minimum_required(VERSION 3.19)

find_program(CARGO cargo REQUIRED
    HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    DOC "Rust cargo package manager")

message(STATUS "cargo found: ${CARGO}")

set(CARGO_TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/target")

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CARGO_PROFILE "")
    set(CARGO_OUT_DIR "${CARGO_TARGET_DIR}/debug")
else()
    set(CARGO_PROFILE "--release")
    set(CARGO_OUT_DIR "${CARGO_TARGET_DIR}/release")
endif()

add_custom_target(rustblade ALL
    COMMAND ${CARGO} build ${CARGO_PROFILE}
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building RustBlade (Rust / cargo)"
    VERBATIM
)

add_custom_target(rustblade_demo ALL
    COMMAND ${CARGO} build ${CARGO_PROFILE} --example demo
            --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/Cargo.toml"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Building RustBlade demo example"
    VERBATIM
    DEPENDS rustblade
)
