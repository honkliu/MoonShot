aux_source_directory(. SOURCEFILES)

add_library(Utils ${SOURCEFILES})

target_include_directories(Utils PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
