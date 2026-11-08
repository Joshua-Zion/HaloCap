add_library(halocap_core STATIC
    display_list.cpp
    frame_scheduler.cpp
    capture_policy.cpp
    audio_policy.cpp
    input_policy.cpp
    encoder_policy.cpp
    settings_policy.cpp
    store_policy.cpp
    capability_policy.cpp
    tray_menu.cpp
    recording_session.cpp
    annotation.cpp
    selection.cpp
    mosaic.cpp
    ocr.cpp
    update.cpp
    clipboard.cpp
)

target_include_directories(halocap_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# §3.3 规则三（可机检）：core/ 不许出现平台头文件。configure 阶段直接失败，不依赖 CI 是否存在。
set(_platform_include_prefixes
    windows. winnt winuser winbase windef wincon winreg
    d3d11 d3d12 d3dcompiler dxgi d2d1 dwrite dcomp
    mf mfapi mfidl mfreadwrite mftransform mferror mfuuid strmif evr
    audio audioclient mmdeviceapi audevcmd uiaudio xaudio2
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

# 按行看，而不是对整个文件做一次 MATCHES：前一版是整文件匹配，于是 core 自己的
# #include "audio_source.h" 也被前缀 audio 抓住了——名字撞车不是假警报，是判据本身
# 没区分「尖括号的系统头」与「引号的自家头」。现在三条分开判：
#   1) 任何指向 platform/ 或越出 core/ 的相对引入，直接违规；
#   2) 引号形式且能在 core/ 目录里找到同一个文件，那是本项目自己的头，放行；
#   3) 剩下的按平台头前缀判。
foreach(_src IN LISTS _core_sources)
    file(READ "${_src}" _raw)
    string(TOLOWER "${_raw}" _text)
    string(REGEX REPLACE "[\r\n]" ";" _lines "${_text}")

    foreach(_line IN LISTS _lines)
        string(STRIP "${_line}" _line)
        if(NOT _line MATCHES "^# *[a-z]*include *[<\"]([^>\"]+)")
            continue()
        endif()

        set(_name "${CMAKE_MATCH_1}")
        set(_hit "")

        if(_name MATCHES "platform/" OR _name MATCHES "^\\.\\./")
            set(_hit "越出 core/ 的相对引入")
        elseif(_line MATCHES "^# *[a-z]*include *\"" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_name}")
            # 自家头文件，放行。
        else()
            foreach(_prefix IN LISTS _platform_include_prefixes)
                if(_name MATCHES "^${_prefix}")
                    set(_hit "平台头前缀 ${_prefix}")
                    break()
                endif()
            endforeach()
        endif()

        if(_hit)
            string(APPEND _violations "\n  ${_src}: ${_line}  (${_hit})")
        endif()
    endforeach()
endforeach()

if(_violations)
    message(FATAL_ERROR
        "core/ 引入了平台头文件，违反 2.0 计划的 §3.3 规则三。请把这条依赖挪到 platform/ 的某个 seam 后面。${_violations}")
endif()
