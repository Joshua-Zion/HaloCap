cmake_minimum_required(VERSION 3.25)

project(halocap_native LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(/utf-8)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

enable_testing()

add_subdirectory(core)
add_subdirectory(platform/win32)
add_subdirectory(tools/frame_cost)
add_subdirectory(tools/surface_probe)
add_subdirectory(tools/capture_probe)
add_subdirectory(tools/encoder_probe)
add_subdirectory(tools/audio_probe)
add_subdirectory(tools/rect_copy_test)
add_subdirectory(tests)
