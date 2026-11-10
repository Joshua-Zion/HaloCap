#include "win32_tray.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

namespace {

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr wchar_t kClassName[] = L"HaloCap.TrayHost";
constexpr int kIconSize = 32;

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

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

// ---- 应用 logo：白色圆角底板 + 设置页标题栏那套图形 ----
//
// 图形本身与设置页一致：外环 1（黑 alpha 34）+ 内环 1.3（#007AFF）+ 圆点 3（#F5B963），
// 比例按 18 DIP 设计稿。用作应用/托盘图标时垫一张白色圆角底板（圆角 22%），这样在深色
// 任务栏上也清楚。录制时只把中心圆点换成红色（#E5484D），其它不变。
// 逐像素数学 + 4×4 超采样，不依赖 GDI+ 或图标资源。

struct Layer {
    float r = 0;
    float g = 0;
    float b = 0;
    float a = 0;
};

void Over(const Layer& source, Layer& dest) {
    const float outAlpha = source.a + dest.a * (1.0f - source.a);
    if (outAlpha <= 0.0f) {
        return;
    }
    dest.r = (source.r * source.a + dest.r * dest.a * (1.0f - source.a)) / outAlpha;
    dest.g = (source.g * source.a + dest.g * dest.a * (1.0f - source.a)) / outAlpha;
    dest.b = (source.b * source.a + dest.b * dest.a * (1.0f - source.a)) / outAlpha;
    dest.a = outAlpha;
}

std::vector<std::uint8_t> RenderLogoPixels(int size, bool recording) {
    const float s = static_cast<float>(size);
    const float tileRadius = 0.22f * s;              // 底板圆角
    const float center = s * 0.5f;
    const float scale = (0.70f * s) / 18.0f;         // logo 占底板 70%
    const float outerRadius = 9.0f * scale;
    const float outerStroke = 1.0f * scale;
    const float innerRadius = 6.0f * scale;
    const float innerStroke = 1.3f * scale;
    const float dotRadius = 1.5f * scale;

    const Layer white{ 1.0f, 1.0f, 1.0f, 1.0f };
    const Layer outer{ 0.0f, 0.0f, 0.0f, 34.0f / 255.0f };
    const Layer blue{ 0.0f, 122.0f / 255.0f, 1.0f, 1.0f };      // #007AFF
    const Layer dot = recording
        ? Layer{ 229.0f / 255.0f, 72.0f / 255.0f, 77.0f / 255.0f, 1.0f }    // #E5484D
        : Layer{ 245.0f / 255.0f, 185.0f / 255.0f, 99.0f / 255.0f, 1.0f };  // #F5B963

    const auto insideTile = [&](float x, float y) {
        if (x < 0.0f || y < 0.0f || x > s || y > s) {
            return false;
        }
        const float cx = std::clamp(x, tileRadius, s - tileRadius);
        const float cy = std::clamp(y, tileRadius, s - tileRadius);
        const float dx = x - cx;
        const float dy = y - cy;
        return dx * dx + dy * dy <= tileRadius * tileRadius;
    };

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(size) * size * 4, 0);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Layer accumulated;
            constexpr int kSamples = 4;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const float px = static_cast<float>(x) + (static_cast<float>(sx) + 0.5f) / kSamples;
                    const float py = static_cast<float>(y) + (static_cast<float>(sy) + 0.5f) / kSamples;

                    Layer sample;
                    if (insideTile(px, py)) {
                        Over(white, sample);
                        const float dx = px - center;
                        const float dy = py - center;
                        const float distance = std::sqrt(dx * dx + dy * dy);
                        if (std::fabs(distance - outerRadius) <= outerStroke * 0.5f) {
                            Over(outer, sample);
                        }
                        if (std::fabs(distance - innerRadius) <= innerStroke * 0.5f) {
                            Over(blue, sample);
                        }
                        if (distance <= dotRadius) {
                            Over(dot, sample);
                        }
                    }

                    accumulated.r += sample.r * sample.a;
                    accumulated.g += sample.g * sample.a;
                    accumulated.b += sample.b * sample.a;
                    accumulated.a += sample.a;
                }
            }

            const float sampleCount = static_cast<float>(kSamples * kSamples);
            const float alpha = accumulated.a / sampleCount;
            if (alpha <= 0.0f) {
                continue;
            }

            // 取「按 alpha 加权」的平均色，避免边缘被透明样本拉向黑色。
            const float weight = accumulated.a;
            const float r = accumulated.r / weight;
            const float g = accumulated.g / weight;
            const float b = accumulated.b / weight;

            const std::size_t offset = (static_cast<std::size_t>(y) * size + x) * 4;
            pixels[offset + 0] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, b) * 255.0f));
            pixels[offset + 1] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, g) * 255.0f));
            pixels[offset + 2] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, r) * 255.0f));
            pixels[offset + 3] = static_cast<std::uint8_t>(std::lround(std::min(1.0f, alpha) * 255.0f));
        }
    }

    return pixels;
}

void* MakeIcon(std::vector<std::uint8_t>& pixels, int size) {
    HBITMAP color = CreateBitmap(size, size, 1, 32, pixels.data());
    if (color == nullptr) {
        return nullptr;
    }

    // 单色掩码：透明像素置 1，让老系统与高对比度主题也拿得到正确的轮廓。
    const std::size_t maskStride = (static_cast<std::size_t>(size) + 7) / 8;
    std::vector<std::uint8_t> mask(maskStride * static_cast<std::size_t>(size), 0);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::uint8_t alpha = pixels[(static_cast<std::size_t>(y) * size + x) * 4 + 3];
            if (alpha < 128) {
                mask[static_cast<std::size_t>(y) * maskStride + static_cast<std::size_t>(x) / 8] |=
                    static_cast<std::uint8_t>(1u << (7 - (x % 8)));
            }
        }
    }

    HBITMAP monochrome = CreateBitmap(size, size, 1, 1, mask.data());
    if (monochrome == nullptr) {
        DeleteObject(color);
        return nullptr;
    }

    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = monochrome;
    HICON icon = CreateIconIndirect(&info);

    DeleteObject(color);
    DeleteObject(monochrome);
    return icon;
}

void* CreateTrayIcon(bool recording) {
    std::vector<std::uint8_t> pixels = RenderLogoPixels(kIconSize, recording);
    return MakeIcon(pixels, kIconSize);
}

}  // namespace

void* CreateHalocapLogoIcon(int sizePx, bool recording) {
    if (sizePx <= 0) {
        return nullptr;
    }
    std::vector<std::uint8_t> pixels = RenderLogoPixels(sizePx, recording);
    return MakeIcon(pixels, sizePx);
}

std::vector<std::uint8_t> RenderHalocapLogoBgra(int sizePx, bool recording) {
    if (sizePx <= 0) {
        return {};
    }
    return RenderLogoPixels(sizePx, recording);
}

Win32Tray::Win32Tray() = default;

Win32Tray::~Win32Tray() { Close(); }

bool Win32Tray::EnsureClassRegistered() {
    static const bool registered = [] {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = &Win32Tray::WindowProc;
        windowClass.hInstance = GetModuleHandleW(nullptr);
        windowClass.lpszClassName = kClassName;
        // 没有背景刷、不显示：托盘宿主只是个消息落脚点。
        return RegisterClassExW(&windowClass) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool Win32Tray::Start() {
    if (!EnsureClassRegistered()) {
        SetError("class", static_cast<std::int32_t>(GetLastError()));
        return false;
    }

    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (window_ == nullptr) {
        // 普通隐藏窗口，不是 message-only：托盘图标要收 Shell 的 `TaskbarCreated` 广播，
        // 而 message-only 窗口收不到广播消息（资源管理器重启后图标就再也回不来了）。
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"HaloCap",
                                  WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                  GetModuleHandleW(nullptr), this);
        if (window_ == nullptr) {
            SetError("window", static_cast<std::int32_t>(GetLastError()));
            return false;
        }
    }

    if (iconIdle_ == nullptr) {
        iconIdle_ = CreateTrayIcon(false);
    }
    if (iconRecording_ == nullptr) {
        iconRecording_ = CreateTrayIcon(true);
    }
    if (iconIdle_ == nullptr || iconRecording_ == nullptr) {
        SetError("icon", static_cast<std::int32_t>(GetLastError()));
        return false;
    }

    currentIcon_ = iconIdle_;
    tooltip_ = core::TrayTooltip(core::TrayMenuState{});
    return AddIcon();
}

void Win32Tray::Close() {
    RemoveIcon();
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
    if (iconIdle_ != nullptr) {
        DestroyIcon(static_cast<HICON>(iconIdle_));
        iconIdle_ = nullptr;
    }
    if (iconRecording_ != nullptr) {
        DestroyIcon(static_cast<HICON>(iconRecording_));
        iconRecording_ = nullptr;
    }
    currentIcon_ = nullptr;
    recording_ = false;
}

bool Win32Tray::AddIcon() {
    if (window_ == nullptr) {
        SetError("window", 0);
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(window_);
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = static_cast<HICON>(currentIcon_);

    const std::wstring tooltip = Utf8ToWide(tooltip_);
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
        // 没有资源管理器、或图标已被加过（同一进程重复 Start）。
        SetError("denied", static_cast<std::int32_t>(GetLastError()));
        added_ = false;
        return false;
    }

    added_ = true;
    SetError("ok", 0);
    return true;
}

void Win32Tray::RemoveIcon() {
    if (!added_ || window_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(window_);
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}

bool Win32Tray::ModifyIcon() {
    if (!added_) {
        return AddIcon();
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(window_);
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_TIP;
    data.hIcon = static_cast<HICON>(currentIcon_);

    const std::wstring tooltip = Utf8ToWide(tooltip_);
    wcsncpy_s(data.szTip, tooltip.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        SetError("denied", static_cast<std::int32_t>(GetLastError()));
        return false;
    }
    SetError("ok", 0);
    return true;
}

bool Win32Tray::Apply(const core::TrayMenuState& state) {
    recording_ = state.recording;
    currentIcon_ = recording_ ? iconRecording_ : iconIdle_;
    tooltip_ = core::TrayTooltip(state);
    return ModifyIcon();
}

bool Win32Tray::Notify(std::string_view title, std::string_view message) {
    if (!added_ && !AddIcon()) {
        return false;
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = static_cast<HWND>(window_);
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_INFO;
    data.uTimeout = 3500;

    const std::wstring wideTitle = Utf8ToWide(title);
    const std::wstring wideMessage = Utf8ToWide(message);
    wcsncpy_s(data.szInfoTitle, wideTitle.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, wideMessage.c_str(), _TRUNCATE);

    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
        SetError("denied", static_cast<std::int32_t>(GetLastError()));
        return false;
    }
    SetError("ok", 0);
    return true;
}

void* Win32Tray::BuildMenuHandle(const core::TrayMenuState& state) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        SetError("menu", static_cast<std::int32_t>(GetLastError()));
        return nullptr;
    }

    for (const core::TrayMenuItem& item : core::BuildTrayMenu(state)) {
        if (item.separator) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }

        UINT flags = MF_STRING;
        if (!item.enabled) {
            flags |= MF_GRAYED;
        }
        if (item.checked) {
            flags |= MF_CHECKED;
        }

        std::wstring text = Utf8ToWide(item.label);
        if (!item.shortcut.empty()) {
            // 标准菜单用 \t 把后半段右对齐——1.x 的自绘菜单里那是一列独立的快捷键文本。
            text += L'\t';
            text += Utf8ToWide(item.shortcut);
        }
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.command), text.c_str());
    }

    return menu;
}

Win32Tray::Click Win32Tray::PollClick() {
    const Click click = pendingClick_;
    pendingClick_ = Click::None;
    return click;
}

core::TrayCommand Win32Tray::PollCommand() {
    const core::TrayCommand command = pendingCommand_;
    pendingCommand_ = core::TrayCommand::None;
    return command;
}

bool Win32Tray::PollBalloonClick() {
    const bool clicked = pendingBalloonClick_;
    pendingBalloonClick_ = false;
    return clicked;
}

void Win32Tray::PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool Win32Tray::DescribeMenu(const core::TrayMenuState& state, MenuSnapshot& out) {
    HMENU menu = static_cast<HMENU>(BuildMenuHandle(state));
    if (menu == nullptr) {
        return false;
    }

    out = MenuSnapshot{};
    const int count = GetMenuItemCount(menu);
    for (int index = 0; index < count; ++index) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_STRING;
        wchar_t text[256] = {};
        info.dwTypeData = text;
        info.cch = 255;

        if (!GetMenuItemInfoW(menu, static_cast<UINT>(index), TRUE, &info)) {
            DestroyMenu(menu);
            SetError("menu", static_cast<std::int32_t>(GetLastError()));
            return false;
        }

        const bool separator = (info.fType & MFT_SEPARATOR) != 0;
        out.separators.push_back(separator);
        out.commands.push_back(separator ? 0 : static_cast<std::int32_t>(info.wID));
        out.labels.push_back(WideToUtf8(text));
        out.enabled.push_back((info.fState & (MFS_GRAYED | MFS_DISABLED)) == 0);
        out.checked.push_back((info.fState & MFS_CHECKED) != 0);
    }

    DestroyMenu(menu);
    SetError("ok", 0);
    return true;
}

void Win32Tray::PostFakeClick(bool rightButton) {
    if (window_ == nullptr) {
        return;
    }
    PostMessageW(static_cast<HWND>(window_), kTrayMessage, kTrayIconId,
                 rightButton ? WM_RBUTTONUP : WM_LBUTTONUP);
}

void Win32Tray::SetError(const char* kind, std::int32_t code) {
    errorKind_ = kind;
    lastError_ = code;
}

LRESULT CALLBACK Win32Tray::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    Win32Tray* self = reinterpret_cast<Win32Tray*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Tray*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
    }

    if (self != nullptr) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Win32Tray::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        // 资源管理器重启之后图标会被 Shell 丢掉，唯一的补救是重新加一次。
        added_ = false;
        AddIcon();
        return 0;
    }

    switch (message) {
    case kTrayMessage:
        // 没有调用 NIM_SETVERSION，所以是经典布局：wParam = 图标 id，lParam = 鼠标消息。
        if (static_cast<UINT>(wParam) != kTrayIconId) {
            return 0;
        }
        switch (LOWORD(lParam)) {
        case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK:
            pendingClick_ = Click::Left;
            break;
        case WM_RBUTTONUP:
        case WM_CONTEXTMENU:
            pendingClick_ = Click::Right;
            break;
        case NIN_BALLOONUSERCLICK:
            // 用户点了气泡：app 侧据此打开「已保存」对应的文件位置。
            pendingBalloonClick_ = true;
            break;
        default:
            break;
        }
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (core::IsTrayCommand(id)) {
            pendingCommand_ = static_cast<core::TrayCommand>(id);
            return 0;
        }
        return 0;
    }

    default:
        break;
    }

    return DefWindowProcW(static_cast<HWND>(window_), message, wParam, lParam);
}

}
