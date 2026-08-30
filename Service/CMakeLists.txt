find_package(httplib CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(shennong
    ShennongHttp.cpp
)

target_link_libraries(shennong PRIVATE
    MoonShot::MoonShot
    httplib::httplib
    nlohmann_json::nlohmann_json
)

if(WIN32)
    target_link_libraries(shennong PRIVATE ws2_32)
endif()
