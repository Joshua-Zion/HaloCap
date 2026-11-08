add_executable(halocap_app main.cpp screenshot_overlay.cpp pin_window.cpp ocr_result_window.cpp ocr_worker.cpp settings_window.cpp radial_menu.cpp)
set_target_properties(halocap_app PROPERTIES OUTPUT_NAME "HaloCap")
target_link_libraries(halocap_app PRIVATE halocap_platform_win32 gdiplus)
halocap_copy_onnxruntime(halocap_app)
