cmake_minimum_required(VERSION 3.14)

# ── moon_inspect ─────────────────────────────────────────────────────────────
add_executable(moon_inspect moon_inspect.cpp)

if(MSVC)
    target_compile_options(moon_inspect PRIVATE /W3 /wd4996)
endif()

# ── moon_wasm (Rust → WASM viewer) ───────────────────────────────────────────
find_program(WASM_PACK wasm-pack
    HINTS "$ENV{HOME}/.cargo/bin" "$ENV{USERPROFILE}/.cargo/bin"
    DOC "wasm-pack tool")

if(NOT WASM_PACK)
    message(FATAL_ERROR "wasm-pack not found. Install it with: cargo install wasm-pack")
else()
    message(STATUS "wasm-pack found: ${WASM_PACK}")

    get_filename_component(CARGO_BIN_DIR "${CARGO}" DIRECTORY)

    if(WIN32)
        # Generate a wrapper that prepends cargo bin to the LIVE PATH at build time.
        # cmake -E env would bake in the configure-time PATH, missing MSVC link.exe.
        file(GENERATE
            OUTPUT "${CMAKE_BINARY_DIR}/run_wasm_pack.$<CONFIG>.bat"
            CONTENT "@echo off\r\nset PATH=${CARGO_BIN_DIR};%PATH%\r\n\"${WASM_PACK}\" %*\r\n"
        )
        set(WASM_PACK_COMMAND "${CMAKE_BINARY_DIR}/run_wasm_pack.$<CONFIG>.bat")
    else()
        set(WASM_PACK_COMMAND ${CMAKE_COMMAND} -E env "PATH=${CARGO_BIN_DIR}:$ENV{PATH}" "${WASM_PACK}")
    endif()

    add_custom_target(moon_wasm ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${MOONSHOT_OUTPUT_ROOT}/$<CONFIG>/moon_wasm/pkg"
        COMMAND ${WASM_PACK_COMMAND}
                build
                --target web
                --release
            --out-dir "${MOONSHOT_OUTPUT_ROOT}/$<CONFIG>/moon_wasm/pkg"
                .
                --
            --target-dir "${MOONSHOT_OUTPUT_ROOT}/$<CONFIG>/rust"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/moon_wasm/index.html"
            "${MOONSHOT_OUTPUT_ROOT}/$<CONFIG>/moon_wasm/index.html"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_CURRENT_SOURCE_DIR}/moon_wasm/serve.py"
            "${MOONSHOT_OUTPUT_ROOT}/$<CONFIG>/moon_wasm/serve.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/moon_wasm"
        COMMENT "Building moon_wasm (Rust → WASM viewer)"
        VERBATIM
    )

    # RustBlade must build first so proc-macro host binaries are cached
    # in the config-specific Rust target dir before wasm-pack compiles.
    if(TARGET rustblade)
        add_dependencies(moon_wasm rustblade)
    endif()
endif()
