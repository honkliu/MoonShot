#project(Executor)

find_package(ICU REQUIRED COMPONENTS uc i18n)

aux_source_directory(. SOURCEFILES)

add_library(Tokenizer ${SOURCEFILES})

target_link_libraries(Tokenizer PRIVATE 
    ${ICU_LIBRARIES}
)
