cmake_minimum_required(VERSION 3.10)

foreach(example quickstart fulltext_search demo moon)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE MoonShot::MoonShot)
endforeach()

if(WIN32)
    target_link_libraries(moon PRIVATE ws2_32)
endif()
