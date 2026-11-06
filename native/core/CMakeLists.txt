add_library(halocap_core STATIC
    display_list.cpp
    frame_scheduler.cpp
    capture_policy.cpp
    encoder_policy.cpp
)

target_include_directories(halocap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# §3.3 规则三（可机检）：core/ 不许出现平台头文件。configure 阶段直接失败，不依赖 CI 是否存在。
set(_platform_include_prefixes
    windows. winnt winuser winbase windef wincon winreg
    d3d11 d3d12 d3dcompiler dxgi d2d1 dwrite dcomp
    mf mfapi mfidl mfreadwrite mftransform mferror mfuuid strmif evr
    audioclient mmdeviceapi audevcmd uiaudio audiomedatype xaudio2
    objbase combaseapi ole2 oleauto shellapi shlobj shobjidl psapi shlwapi imm.h
    winsock2 ws2tcpip
    winrt/ wrl/
)

set(_violations "")
file(GLOB_RECURSE _core_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)

foreach(_src IN LISTS _core_sources)
    file(READ "${_src}" _raw)
    string(TOLOWER "${_raw}" _text)
    foreach(_prefix IN LISTS _platform_include_prefixes)
        if("${_text}" MATCHES "#include *[<\"]${_prefix}")
            string(APPEND _violations "\n  ${_src}: #include <${_prefix}...>")
        endif()
    endforeach()
endforeach()

if(_violations)
    message(FATAL_ERROR
        "core/ 引入了平台头文件，违反 2.0 计划的 §3.3 规则三。请把这条依赖挪到 platform/ 的某个 seam 后面。${_violations}")
endif()
