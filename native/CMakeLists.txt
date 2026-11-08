cmake_minimum_required(VERSION 3.25)

project(halocap_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

enable_testing()

# 头文件依赖的坑见 build.cmd 顶部：本机 MSVC 的 /showIncludes 是本地化文本，ninja 收不到
# 头依赖，所以 build.cmd 在「有头文件比 build.ninja 新」时直接全量重建。
# 源文件里有中文注释，必须加 /utf-8，否则 MSVC 按系统代码页读，注释会吞掉后面的代码。
add_compile_options(/utf-8)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

add_subdirectory(core)
add_subdirectory(platform/win32)
add_subdirectory(app)
add_subdirectory(tools/frame_cost)
add_subdirectory(tools/surface_probe)
add_subdirectory(tools/capture_probe)
add_subdirectory(tools/encoder_probe)
add_subdirectory(tools/audio_probe)
add_subdirectory(tools/input_probe)
add_subdirectory(tools/store_probe)
add_subdirectory(tools/tray_probe)
add_subdirectory(tools/record_probe)
add_subdirectory(tools/wgc_probe)
add_subdirectory(tools/screenshot_probe)
add_subdirectory(tools/rect_copy_test)
add_subdirectory(tests)
