add_executable(halocap_app main.cpp)
set_target_properties(halocap_app PROPERTIES OUTPUT_NAME "HaloCap")
target_link_libraries(halocap_app PRIVATE halocap_platform_win32)
