add_library(halocap_platform_win32 STATIC
    d2d_render_backend.cpp
    d2d_font_rasterizer.cpp
    win32_surface_window.cpp
)

target_include_directories(halocap_platform_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(halocap_platform_win32 PUBLIC halocap_core d3d11 dxgi d2d1 dwrite dcomp user32)
