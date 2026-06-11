cmake_minimum_required(VERSION 3.10)

find_package(ICU REQUIRED COMPONENTS uc i18n)

aux_source_directory(. SOURCEFILES)

add_library(Tokenizer ${SOURCEFILES})

target_include_directories(Tokenizer PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    ${ICU_INCLUDE_DIRS}
)

target_link_libraries(Tokenizer PRIVATE ${ICU_LIBRARIES})
