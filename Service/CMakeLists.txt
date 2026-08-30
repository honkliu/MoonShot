find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(shennong
    ShennongHttp.cpp
)

add_executable(shennong_cli
    ShennongCli.cpp
)

target_link_libraries(shennong PRIVATE
    MoonShot::MoonShot
    httplib::httplib
    nlohmann_json::nlohmann_json
)

target_link_libraries(shennong_cli PRIVATE
    httplib::httplib
    nlohmann_json::nlohmann_json
)

add_custom_target(shennong_web
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/web"
            "${MOONSHOT_CONFIG_OUTPUT_DIR}/web"
)
add_dependencies(shennong shennong_web)

if(WIN32)
    target_link_libraries(shennong PRIVATE ws2_32)
endif()
