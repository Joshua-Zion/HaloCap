set(HALOCAP_APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/app.ico")
configure_file(app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)

add_executable(halocap_app main.cpp screenshot_overlay.cpp pin_window.cpp ocr_result_window.cpp ocr_worker.cpp settings_window.cpp radial_menu.cpp recording_hud.cpp tray_menu_window.cpp window_picker.cpp window_at_point.cpp soft_shadow.cpp "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
set_target_properties(halocap_app PROPERTIES OUTPUT_NAME "HaloCap" WIN32_EXECUTABLE TRUE)
if (MSVC)
    # GUI 子系统：双击启动不再分配控制台窗口。入口仍用 main（CRT 的 mainCRTStartup），
    # 命令行/自检调用时由 main 里的 AttachConsole 挂到父控制台。
    target_link_options(halocap_app PRIVATE /ENTRY:mainCRTStartup)
endif()
target_link_libraries(halocap_app PRIVATE halocap_platform_win32 gdiplus dwmapi shcore)
halocap_copy_onnxruntime(halocap_app)
