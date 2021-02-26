#project(Executor)

aux_source_directory(. SOURCEFILES)

add_library(IndexAccess ${SOURCEFILES})

include_directories(
    "${CMAKE_CURRENT_LIST_DIR}/../thirdParty/include"
    "${CMAKE_CURRENT_LIST_DIR}/../Executor"
    "${CMAKE_CURRENT_LIST_DIR}"

)