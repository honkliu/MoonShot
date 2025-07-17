#project(Executor)

find_package(ICU REQUIRED COMPONENTS uc i18n)

aux_source_directory(. SOURCEFILES)

add_library(Tokenizer ${SOURCEFILES})

message(STATUS "ICU libraries: ${ICU_LIBRARIES}")

target_include_directories(Tokenizer PRIVATE
    ${ICU_INCLUDE_DIRS}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(Tokenizer PRIVATE 
    ${ICU_LIBRARIES}
)
