add_library(halocap_platform_win32 STATIC
    d2d_render_backend.cpp
    d2d_font_rasterizer.cpp
    win32_surface_window.cpp
    dxgi_desktop_duplication.cpp
    nv12_converter.cpp
    mf_h264_encoder.cpp
)

target_include_directories(halocap_platform_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# mf* 是编码 seam 的四件套：mfplat 起 Foundation 与建样本，mf 是文件流与 DXGI 缓冲，
# mfreadwrite 提供 SinkWriter，mfuuid 提供那些 DEFINE_GUID 声明出来的属性与格式 GUID。
target_link_libraries(halocap_platform_win32 PUBLIC
    halocap_core
    d3d11 dxgi d2d1 dwrite dcomp user32
    mfplat mf mfreadwrite mfuuid ole32)
