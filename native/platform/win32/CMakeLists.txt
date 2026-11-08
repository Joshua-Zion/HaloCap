add_library(halocap_platform_win32 STATIC
    d2d_render_backend.cpp
    d2d_font_rasterizer.cpp
    win32_surface_window.cpp
    dxgi_desktop_duplication.cpp
    nv12_converter.cpp
    mf_h264_encoder.cpp
    wasapi_audio_source.cpp
    win32_input_host.cpp
    win32_store.cpp
    win32_capability.cpp
    win32_tray.cpp
)

target_include_directories(halocap_platform_win32 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# mf* 是编码 seam 的四件套：mfplat 起 Foundation 与建样本，mf 是文件流与 DXGI 缓冲，
# mfreadwrite 提供 SinkWriter，mfuuid 提供那些 DEFINE_GUID 声明出来的属性与格式 GUID。
# ole32 同时给 Media Foundation 与 WASAPI 用（CoCreateInstance / CoTaskMemFree / PropVariantClear）；
# imm32 是输入宿主断开与恢复输入法用的（ImmAssociateContext）。
# shell32 是 store 的已知文件夹（SHGetKnownFolderPath / SHCreateDirectoryExW），
# psapi 给能力 seam 量私有内存；windowsapp + runtimeobject 是 C++/WinRT 问 WGC 支不支持。
target_link_libraries(halocap_platform_win32 PUBLIC
    halocap_core
    d3d11 dxgi d2d1 dwrite dcomp user32 shell32 psapi
    mfplat mf mfreadwrite mfuuid ole32 imm32 windowsapp runtimeobject)
