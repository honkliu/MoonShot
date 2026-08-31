find_package(httplib CONFIG QUIET)
if(TARGET httplib::httplib)
    set(SHENNONG_HTTPLIB_TARGET httplib::httplib)
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(CPPHTTPLIB QUIET IMPORTED_TARGET cpp-httplib)
    endif()
    if(TARGET PkgConfig::CPPHTTPLIB)
        set(SHENNONG_HTTPLIB_TARGET PkgConfig::CPPHTTPLIB)
    else()
        find_path(CPPHTTPLIB_INCLUDE_DIR NAMES httplib.h REQUIRED)
        find_library(CPPHTTPLIB_LIBRARY NAMES cpp-httplib httplib REQUIRED)
        add_library(shennong_httplib UNKNOWN IMPORTED)
        set_target_properties(shennong_httplib PROPERTIES
            IMPORTED_LOCATION "${CPPHTTPLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${CPPHTTPLIB_INCLUDE_DIR}"
        )
        set(SHENNONG_HTTPLIB_TARGET shennong_httplib)
    endif()
endif()

find_package(nlohmann_json CONFIG QUIET)
if(TARGET nlohmann_json::nlohmann_json)
    set(SHENNONG_JSON_TARGET nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp REQUIRED)
    add_library(shennong_nlohmann_json INTERFACE)
    target_include_directories(shennong_nlohmann_json INTERFACE "${NLOHMANN_JSON_INCLUDE_DIR}")
    set(SHENNONG_JSON_TARGET shennong_nlohmann_json)
endif()

add_executable(shennong
    ShennongHttp.cpp
)

add_executable(shennong_cli
    ShennongCli.cpp
)

target_link_libraries(shennong PRIVATE
    MoonShot::MoonShot
    ${SHENNONG_HTTPLIB_TARGET}
    ${SHENNONG_JSON_TARGET}
)

target_link_libraries(shennong_cli PRIVATE
    ${SHENNONG_HTTPLIB_TARGET}
    ${SHENNONG_JSON_TARGET}
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
