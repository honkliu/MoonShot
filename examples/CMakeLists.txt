cmake_minimum_required(VERSION 3.10)

foreach(example quickstart fulltext_search)
    add_executable(${example} ${example}.cpp)
    target_link_libraries(${example} PRIVATE MoonShot::MoonShot)
endforeach()
