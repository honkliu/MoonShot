cmake_minimum_required(VERSION 3.10)

find_package(ICU OPTIONAL_COMPONENTS uc i18n)

aux_source_directory(. SOURCEFILES)

add_library(Tokenizer ${SOURCEFILES})

if(ICU_FOUND)
    message(STATUS "ICU found: ${ICU_LIBRARIES}")
    target_compile_definitions(Tokenizer PUBLIC HAVE_ICU)
    target_include_directories(Tokenizer PRIVATE ${ICU_INCLUDE_DIRS})
    target_link_libraries(Tokenizer PRIVATE ${ICU_LIBRARIES})
else()
    message(STATUS "ICU not found -- using built-in SimpleTokenizer fallback")
endif()

target_include_directories(Tokenizer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
