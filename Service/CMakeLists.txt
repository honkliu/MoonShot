add_executable(shennong
    ShennongHttp.cpp
)

target_link_libraries(shennong PRIVATE MoonShot::MoonShot)

if(WIN32)
    target_link_libraries(shennong PRIVATE ws2_32)
endif()
