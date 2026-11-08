#include "win32_capability.h"

#include <windows.h>

#include <psapi.h>
#include <roapi.h>

#include <winrt/Windows.Graphics.Capture.h>

#include <string>
#include <string_view>

namespace halocap::platform::win32 {

namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

// GetVersionEx 在不带兼容性清单的进程里会撒谎（报 6.2），所以问 RtlGetVersion。
// 这个函数在 ntdll 里从 Win2000 起就有，签名稳定；仓库里没有清单的探针也拿得到真数字。
int CurrentOsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    static const RtlGetVersionFn rtlGetVersion = [] {
        HMODULE module = GetModuleHandleW(L"ntdll.dll");
        if (module == nullptr) {
            return static_cast<RtlGetVersionFn>(nullptr);
        }
        return reinterpret_cast<RtlGetVersionFn>(GetProcAddress(module, "RtlGetVersion"));
    }();

    if (rtlGetVersion == nullptr) {
        return 0;
    }

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0 /* STATUS_SUCCESS */) {
        return 0;
    }
    return static_cast<int>(info.dwBuildNumber);
}

// WGC 是窗口录制的前提，而它是否可用是运行时才知道的。WinRT 的静态方法要求先初始化
// WinRT——宿主（托盘应用）通常已经初始化过，那时 RoInitialize 返回 RPC_E_CHANGED_MODE，
// 这里不能覆盖它的模式，只能照用。
bool GraphicsCaptureSupported() {
    static const bool supported = [] {
        const HRESULT initialized = ::RoInitialize(RO_INIT_MULTITHREADED);
        (void)initialized;

        try {
            return winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported();
        } catch (...) {
            // 投影层抛错（缺系统组件、代理被裁剪）按「不可用」处理：窗口录制降级，
            // 区域录制仍走桌面复制，不该因为一次探测失败让整个功能消失。
            return false;
        }
    }();
    return supported;
}

std::int64_t PrivateMemoryBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return 0;
    }
    return static_cast<std::int64_t>(counters.PrivateUsage);
}

// 路径可能还没创建（第一次录制前保存目录不存在），GetDiskFreeSpaceEx 对不存在的目录会失败，
// 所以退到卷根再问一次。
bool QueryFreeBytes(const std::wstring& path, ULARGE_INTEGER& out) {
    if (path.empty()) {
        return false;
    }
    if (GetDiskFreeSpaceExW(path.c_str(), &out, nullptr, nullptr)) {
        return true;
    }

    wchar_t volume[MAX_PATH] = {};
    if (GetVolumePathNameW(path.c_str(), volume, MAX_PATH) &&
        GetDiskFreeSpaceExW(volume, &out, nullptr, nullptr)) {
        return true;
    }

    // 卷信息也拿不到时，按 "X:\" 前缀兜底。
    if (path.size() >= 2 && path[1] == L':') {
        const std::wstring root = { path[0], L':', L'\\' };
        return GetDiskFreeSpaceExW(root.c_str(), &out, nullptr, nullptr) != 0;
    }
    return false;
}

}  // namespace

core::PlatformFacts Win32Capability::Probe() {
    core::PlatformFacts facts;
    facts.osBuild = CurrentOsBuild();
    facts.graphicsCaptureSupported = GraphicsCaptureSupported();
    facts.privateMemoryBytes = PrivateMemoryBytes();
    return facts;
}

std::int64_t Win32Capability::AvailableBytes(const char* utf8Path) {
    if (utf8Path == nullptr || *utf8Path == '\0') {
        return -1;
    }

    ULARGE_INTEGER available{};
    if (!QueryFreeBytes(Utf8ToWide(utf8Path), available)) {
        return -1;
    }
    return static_cast<std::int64_t>(available.QuadPart);
}

}
