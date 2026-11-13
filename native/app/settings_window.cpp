#include "settings_window.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#include "update.h"
#include "version.h"
#include "ocr_model.h"
#include "soft_shadow.h"
#include "win32_tray.h"
#include "win32_update_fetch.h"
#include "win32_autostart.h"
#include "win32_capability.h"

namespace halocap::app {

namespace {

using namespace Gdiplus;

constexpr wchar_t kClassName[] = L"HaloCapSettings";
// 目录选择线程把结果（堆上的 std::wstring*）通过这条消息交回主线程。
constexpr UINT kMsgDirectoryPicked = WM_APP + 0x51;
// 更新流程：检查结果 / 下载进度 / 下载完成（都从工作线程 PostMessage 回来）。
constexpr UINT kMsgUpdateChecked = WM_APP + 0x60;
constexpr UINT kMsgUpdateProgress = WM_APP + 0x61;   // wParam = 0..100
constexpr UINT kMsgUpdateDownloaded = WM_APP + 0x62;
// OCR 模型下载：进度与结果。
constexpr UINT kMsgOcrModelProgress = WM_APP + 0x70;
constexpr UINT kMsgOcrModelDownloaded = WM_APP + 0x71;

struct OcrModelOutcome {
    std::wstring error;
    std::wstring folder;
};

std::string Utf8FromWide(std::wstring_view text);

// 模型根目录：用户放入或下载的模型都按「一个子目录一套模型」摆放。
std::wstring OcrModelRoot() {
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0) {
        return {};
    }
    return std::wstring(localAppData) + L"\\HaloCap2\\models";
}

// 枚举一个目录里的文件名，交给 core 按关键字挑 det/rec/字典。
core::OcrModelFiles FilesInDirectory(const std::wstring& directory) {
    std::vector<std::string> names;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
        if (entry.is_regular_file(error)) {
            names.push_back(Utf8FromWide(entry.path().filename().wstring()));
        }
    }
    std::sort(names.begin(), names.end());
    return core::DetectOcrModelFiles(names);
}

struct UpdateCheckOutcome {
    std::wstring tag;
    std::wstring error;
};

struct UpdateDownloadOutcome {
    std::wstring path;
    std::wstring error;
};
constexpr float kWindowW = 916.0f;   // 820 卡片 + 48 DIP/侧 透明边给阴影
constexpr float kWindowH = 716.0f;
constexpr float kMargin = 48.0f;    // 卡片 inset：48 DIP 阴影余量
constexpr float kTitleBar = 50.0f;
constexpr float kSidebar = 218.0f;
constexpr float kFooter = 60.0f;
constexpr float kRadius = 18.0f;
constexpr float kContentX = kMargin + kSidebar;

// 控件/导航的点击 id。
enum : int {
    kIdNone = 0,
    kIdClose = 10,
    kIdSave = 11,
    kIdCancel = 12,
    kIdNavBase = 20,
    kIdToggleAutoStart = 30,
    kIdToggleConvertFiles = 31,
    kIdTogglePinGlow = 32,
    kIdToggleAutoCopy = 33,
    kIdToggleElevate = 34,
    kIdToggleSystemAudio = 35,
    kIdToggleMicrophone = 36,
    kIdToggleRecordCursor = 37,
    kIdToggleOcrAutoStart = 38,
    kIdToggleRecordAutoStart = 39,
    kIdSegmentBase = 100,
    kIdHotkeyBase = 200,
    kIdDirectory = 210,
    kIdUpdate = 211,
    kIdCopySummary = 212,
    kIdOcrModelDownload = 40,
    kIdOcrModelOpen = 41,
    kIdOcrModelBase = 300,
};

// 分段控件的组号（id = kIdSegmentBase + group * 10 + option）。
enum : int {
    kGroupMenuSize = 0,
    kGroupScreenshotFormat = 1,
    kGroupOcrEngine = 2,
    kGroupResolution = 3,
    kGroupFrameRate = 4,
    kGroupQuality = 5,
    kGroupRange = 6,
    kGroupCountdown = 7,
};

SettingsWindow* g_hotkeyOwner = nullptr;
HHOOK g_hotkeyHook = nullptr;

// 低级钩子里物理按键报的是左右分开的虚拟码（VK_LCONTROL 等），不是 VK_CONTROL。
bool IsModifierVirtualKey(DWORD virtualKey) {
    switch (virtualKey) {
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
    case VK_LMENU:
    case VK_RMENU:
    case VK_MENU:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

bool EnsureGdiplus() {
    static ULONG_PTR token = 0;
    if (token == 0) {
        GdiplusStartupInput input;
        if (GdiplusStartup(&token, &input, nullptr) != Ok) {
            return false;
        }
    }
    return true;
}

struct Fonts {
    Font title{ L"Microsoft YaHei UI", 21.0f, FontStyleBold, UnitPixel };
    Font subtitle{ L"Microsoft YaHei UI", 12.0f, FontStyleRegular, UnitPixel };
    Font group{ L"Microsoft YaHei UI", 11.0f, FontStyleBold, UnitPixel };
    Font setting{ L"Microsoft YaHei UI", 13.0f, FontStyleRegular, UnitPixel };
    Font settingBold{ L"Microsoft YaHei UI", 13.0f, FontStyleBold, UnitPixel };
    Font desc{ L"Microsoft YaHei UI", 11.0f, FontStyleRegular, UnitPixel };
    Font nav{ L"Microsoft YaHei UI", 13.0f, FontStyleRegular, UnitPixel };
    Font navBold{ L"Microsoft YaHei UI", 13.0f, FontStyleBold, UnitPixel };
    Font field{ L"Microsoft YaHei UI", 12.0f, FontStyleRegular, UnitPixel };
    Font glyph{ L"Segoe MDL2 Assets", 13.0f, FontStyleRegular, UnitPixel };
    Font button{ L"Microsoft YaHei UI", 12.0f, FontStyleRegular, UnitPixel };
    Font buttonBold{ L"Microsoft YaHei UI", 12.0f, FontStyleBold, UnitPixel };
};

Fonts* GetFonts() {
    static std::unique_ptr<Fonts> fonts;
    if (!fonts) {
        fonts = std::make_unique<Fonts>();
    }
    return fonts.get();
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

GraphicsPath* RoundRect(float x, float y, float w, float h, float radius);

void DrawNavIcon(Graphics& g, int index, const PointF& center, const Color& color) {
    Pen pen(color, 1.4f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    const float x = center.X;
    const float y = center.Y;
    switch (index) {
    case 0: {   // 通用：滑杆
        g.DrawLine(&pen, x - 8.0f, y - 5.0f, x + 8.0f, y - 5.0f);
        g.DrawLine(&pen, x - 8.0f, y + 1.0f, x + 8.0f, y + 1.0f);
        g.DrawLine(&pen, x - 8.0f, y + 7.0f, x + 8.0f, y + 7.0f);
        SolidBrush knob(color);
        g.FillEllipse(&knob, x - 1.5f, y - 7.5f, 4.0f, 5.0f);
        g.FillEllipse(&knob, x - 5.5f, y - 1.5f, 4.0f, 5.0f);
        g.FillEllipse(&knob, x + 1.5f, y + 4.5f, 4.0f, 5.0f);
        break;
    }
    case 1: {   // 快捷键：键盘
        GraphicsPath* body = RoundRect(x - 9.0f, y - 6.0f, 18.0f, 12.0f, 2.5f);
        g.DrawPath(&pen, body);
        delete body;
        for (int i = -1; i <= 1; ++i) {
            SolidBrush keyBrush(color);
            g.FillRectangle(&keyBrush, x + i * 4.0f - 1.0f, y - 2.5f, 2.0f, 2.0f);
        }
        g.DrawLine(&pen, x - 4.0f, y + 3.0f, x + 4.0f, y + 3.0f);
        break;
    }
    case 2: {   // 截图：四角
        g.DrawLine(&pen, x - 8.0f, y - 3.0f, x - 8.0f, y - 8.0f);
        g.DrawLine(&pen, x - 8.0f, y - 8.0f, x - 3.0f, y - 8.0f);
        g.DrawLine(&pen, x + 8.0f, y - 3.0f, x + 8.0f, y - 8.0f);
        g.DrawLine(&pen, x + 8.0f, y - 8.0f, x + 3.0f, y - 8.0f);
        g.DrawLine(&pen, x - 8.0f, y + 3.0f, x - 8.0f, y + 8.0f);
        g.DrawLine(&pen, x - 8.0f, y + 8.0f, x - 3.0f, y + 8.0f);
        g.DrawLine(&pen, x + 8.0f, y + 3.0f, x + 8.0f, y + 8.0f);
        g.DrawLine(&pen, x + 8.0f, y + 8.0f, x + 3.0f, y + 8.0f);
        break;
    }
    case 3: {   // OCR：T
        g.DrawRectangle(&pen, x - 8.0f, y - 7.0f, 16.0f, 14.0f);
        g.DrawLine(&pen, x - 4.5f, y - 3.0f, x + 4.5f, y - 3.0f);
        g.DrawLine(&pen, x, y - 3.0f, x, y + 4.0f);
        break;
    }
    case 4: {   // 录屏：相机
        GraphicsPath* body = RoundRect(x - 9.0f, y - 5.0f, 13.0f, 10.0f, 3.0f);
        g.DrawPath(&pen, body);
        delete body;
        g.DrawLine(&pen, x + 4.5f, y, x + 9.0f, y - 4.0f);
        g.DrawLine(&pen, x + 9.0f, y - 4.0f, x + 9.0f, y + 4.0f);
        g.DrawLine(&pen, x + 9.0f, y + 4.0f, x + 4.5f, y);
        break;
    }
    default: {  // 关于：信息
        g.DrawEllipse(&pen, x - 8.0f, y - 8.0f, 16.0f, 16.0f);
        SolidBrush infoDot(color);
        g.FillEllipse(&infoDot, x - 1.0f, y - 4.5f, 2.0f, 2.0f);
        g.DrawLine(&pen, x, y - 0.5f, x, y + 4.5f);
        break;
    }
    }
}

std::string Utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr);
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), narrow.data(),
                        length, nullptr, nullptr);
    return narrow;
}

std::wstring Ellipsize(Graphics& g, const std::wstring& text, Font* font, float maxWidth) {
    if (text.empty()) {
        return text;
    }
    RectF bounds;
    g.MeasureString(text.c_str(), -1, font, PointF(0, 0), &bounds);
    if (bounds.Width <= maxWidth) {
        return text;
    }
    std::wstring result = text;
    while (!result.empty()) {
        result.pop_back();
        std::wstring candidate = result + L"…";
        g.MeasureString(candidate.c_str(), -1, font, PointF(0, 0), &bounds);
        if (bounds.Width <= maxWidth) {
            return candidate;
        }
    }
    return L"…";
}

void DrawText(Graphics& g, const std::wstring& text, Font* font, const RectF& rect,
              const Color& color, StringAlignment align = StringAlignmentNear,
              StringAlignment line = StringAlignmentCenter) {
    StringFormat format;
    format.SetAlignment(align);
    format.SetLineAlignment(line);
    format.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush brush(color);
    g.DrawString(text.c_str(), -1, font, rect, &format, &brush);
}

GraphicsPath* RoundRect(float x, float y, float w, float h, float radius) {
    auto* path = new GraphicsPath();
    const float d = radius * 2.0f;
    path->AddArc(x, y, d, d, 180.0f, 90.0f);
    path->AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path->AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path->AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

void FillRoundRect(Graphics& g, const RectF& rect, float radius, const Color& color) {
    GraphicsPath* path = RoundRect(rect.X, rect.Y, rect.Width, rect.Height, radius);
    SolidBrush brush(color);
    g.FillPath(&brush, path);
    delete path;
}

void StrokeRoundRect(Graphics& g, const RectF& rect, float radius, const Color& color,
                     float width = 1.0f) {
    GraphicsPath* path = RoundRect(rect.X, rect.Y, rect.Width, rect.Height, radius);
    Pen pen(color, width);
    g.DrawPath(&pen, path);
    delete path;
}

Color WithAlpha(const Color& color, int alpha) {
    return Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), color.GetR(), color.GetG(),
                 color.GetB());
}

const wchar_t* HotkeySlotLabel(int slot) {
    switch (slot) {
    case 0: return L"呼出光圈";
    case 1: return L"截图";
    case 2: return L"文字识别";
    case 3: return L"录屏";
    case 4: return L"贴图";
    default: return L"未知动作";
    }
}

int CountdownOption(core::RecordingCountdown value) {
    switch (value) {
    case core::RecordingCountdown::ThreeSeconds: return 1;
    case core::RecordingCountdown::FiveSeconds: return 2;
    default: return 0;
    }
}

core::KeySymbol KeyFromVirtualKey(DWORD vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::Num0) + (vk - '0'));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::F1) + (vk - VK_F1));
    }
    switch (vk) {
    case VK_ESCAPE: return core::KeySymbol::Escape;
    case VK_RETURN: return core::KeySymbol::Enter;
    case VK_SPACE: return core::KeySymbol::Space;
    case VK_TAB: return core::KeySymbol::Tab;
    case VK_BACK: return core::KeySymbol::Backspace;
    case VK_INSERT: return core::KeySymbol::Insert;
    case VK_DELETE: return core::KeySymbol::Delete;
    case VK_HOME: return core::KeySymbol::Home;
    case VK_END: return core::KeySymbol::End;
    case VK_PRIOR: return core::KeySymbol::PageUp;
    case VK_NEXT: return core::KeySymbol::PageDown;
    case VK_UP: return core::KeySymbol::Up;
    case VK_DOWN: return core::KeySymbol::Down;
    case VK_LEFT: return core::KeySymbol::Left;
    case VK_RIGHT: return core::KeySymbol::Right;
    case VK_SNAPSHOT: return core::KeySymbol::PrintScreen;
    default: return core::KeySymbol::None;
    }
}

std::string ReadUpdateUrl() {
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0) {
        return {};
    }
    const std::wstring path = std::wstring(localAppData) + L"\\HaloCap2\\update.url";
    std::ifstream stream{ std::filesystem::path(path) };
    if (!stream) {
        return {};
    }
    std::string line;
    std::getline(stream, line);
    return line;
}

std::string ReadLocalUpdateManifest() {
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0) {
        return {};
    }
    const std::wstring path = std::wstring(localAppData) + L"\\HaloCap2\\update.json";
    std::ifstream stream{ std::filesystem::path(path), std::ios::binary };
    if (!stream) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

void CopyTextToClipboard(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

}  // namespace

struct SettingsBackgroundCache {
    std::unique_ptr<Gdiplus::Bitmap> image;
};

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() { Close(); }

bool SettingsWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &SettingsWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool SettingsWindow::Create(void* owner, const core::AppSettings& settings,
                            const core::PlatformFacts& facts, const std::string& modelsDirectory,
                            SaveHandler onSave) {
    Close();
    if (!EnsureClass() || !EnsureGdiplus()) {
        return false;
    }
    settings_ = settings;
    facts_ = facts;
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    comInitialized_ = SUCCEEDED(comResult);
    modelsDirectory_ = modelsDirectory;
    RefreshOcrModels();
    onSave_ = std::move(onSave);
    saved_ = false;
    page_ = Page::General;
    scroll_ = 0;

    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        dpiX = 96;
    }
    scale_ = dpiX / 96.0f;
    widthPx_ = static_cast<int>(std::lround(kWindowW * scale_));
    heightPx_ = static_cast<int>(std::lround(kWindowH * scale_));

    HDC screen = GetDC(nullptr);
    memory_ = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = widthPx_;
    info.bmiHeader.biHeight = -heightPx_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(memory_, &info, DIB_RGB_COLORS, &bits_, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap_ == nullptr || bits_ == nullptr) {
        Close();
        return false;
    }
    SelectObject(memory_, bitmap_);

    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    RECT work{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    }
    const int left = work.left + ((work.right - work.left) - widthPx_) / 2;
    const int top = work.top + ((work.bottom - work.top) - heightPx_) / 2;

    // 设置是一个独立的应用窗口：进任务栏、Alt+Tab 也能切回来（TOOLWINDOW 会把它藏起来）。
    // 层级和普通窗口一致：不置顶，被其他窗口盖住是正常的（与 1.x 的普通设置窗一致）。
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, kClassName,
                              L"HaloCap 设置", WS_POPUP, left, top, widthPx_, heightPx_, nullptr,
                              nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        Close();
        return false;
    }

    // 任务栏 / Alt+Tab 用同一个 logo（与标题栏、托盘一致）。
    iconBig_ = halocap::platform::win32::CreateHalocapLogoIcon(32, false);
    iconSmall_ = halocap::platform::win32::CreateHalocapLogoIcon(16, false);
    if (iconBig_ != nullptr) {
        SendMessageW(window_, WM_SETICON, ICON_BIG,
                     reinterpret_cast<LPARAM>(static_cast<HICON>(iconBig_)));
    }
    if (iconSmall_ != nullptr) {
        SendMessageW(window_, WM_SETICON, ICON_SMALL,
                     reinterpret_cast<LPARAM>(static_cast<HICON>(iconSmall_)));
    }

    ShowWindow(window_, SW_SHOW);
    // 普通窗口：打开时到前台一次（和正常应用一致），但不再置顶，随后可被其他窗口盖住。
    SetForegroundWindow(window_);
    Render();
    return true;
}

void SettingsWindow::ShowPageForTest(int index) {
    page_ = static_cast<Page>(std::clamp(index, 0, 5));
    if (page_ == Page::Ocr) {
        RefreshOcrModels();
    }
    scroll_ = 0;
    Render();
}

void SettingsWindow::Close() {
    if (keyHook_ != nullptr) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = nullptr;
    }
    if (g_hotkeyOwner == this) {
        g_hotkeyOwner = nullptr;
    }
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (iconBig_ != nullptr) {
        DestroyIcon(static_cast<HICON>(iconBig_));
        iconBig_ = nullptr;
    }
    if (iconSmall_ != nullptr) {
        DestroyIcon(static_cast<HICON>(iconSmall_));
        iconSmall_ = nullptr;
    }
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (memory_ != nullptr) {
        DeleteDC(memory_);
        memory_ = nullptr;
    }
    bits_ = nullptr;
    if (comInitialized_) {
        CoUninitialize();
        comInitialized_ = false;
    }
}

LRESULT CALLBACK SettingsWindow::HotkeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_hotkeyOwner == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    ++g_hotkeyOwner->hookEvents_;
    const DWORD vk = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode;
    const bool isDown = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
    const bool isUp = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
    if (!isDown && !isUp) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 修饰键状态自己记，不读 GetAsyncKeyState：钩子里那个状态可能还没跟上
    // （注入的事件尤其明显，实测会把 Ctrl+Alt+D 读成裸 D），组合键就被当成单键了。
    if (IsModifierVirtualKey(vk)) {
        g_hotkeyOwner->TrackModifier(vk, isDown);
        return 1;
    }
    if (isUp) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (vk == VK_ESCAPE) {
        g_hotkeyOwner->CancelHotkeyCapture();
        return 1;
    }
    core::HotkeySpec spec;
    spec.key = KeyFromVirtualKey(vk);
    if (g_hotkeyOwner->ctrlDown_) {
        spec.modifiers |= core::kModControl;
    }
    if (g_hotkeyOwner->altDown_) {
        spec.modifiers |= core::kModAlt;
    }
    if (g_hotkeyOwner->shiftDown_) {
        spec.modifiers |= core::kModShift;
    }
    if (g_hotkeyOwner->winDown_) {
        spec.modifiers |= core::kModWin;
    }
    if (spec.key == core::KeySymbol::None) {
        // 映射不到的键（例如输入法专用键）以前是静默吞掉，用户完全不知道发生了什么。
        g_hotkeyOwner->ShowError(L"这个按键不能用作快捷键，请换一个。");
        return 1;
    }
    const core::HotkeyCheck check = core::CheckHotkey(spec);
    if (!check.valid) {
        // 1.x 在这里只弹横幅、不改值、也不退出捕获态：用户接着按下一把就行。
        g_hotkeyOwner->ShowError(
            (spec.modifiers & core::kStateModifiers) == 0
                ? L"该单键不适合用作全局快捷键，请改用 F1–F24、字母、数字，或按住 Ctrl / Alt / Shift 再按主键。"
                : L"无法使用该组合键，请换一个。");
        return 1;
    }
    g_hotkeyOwner->FinishHotkeyCapture(spec);
    return 1;
}

void SettingsWindow::StartHotkeyCapture(int index) {
    if (index < 0 || index >= core::kHotkeySlotCount) {
        return;
    }
    // 再点同一行 = 取消（1.x 的 HotkeyButton_Click）；点别的行则换一行继续捕获。
    if (captureIndex_ == index) {
        CancelHotkeyCapture();
        return;
    }
    if (keyHook_ != nullptr) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = nullptr;
    }
    captureIndex_ = index;
    g_hotkeyOwner = this;
    hookEvents_ = 0;
    // 起捕获时可能已经有修饰键按着（用户先按住再点行不算常见，但按错了常见）。
    // 之后状态全靠钩子自己维护，这里只做一次种子。
    ctrlDown_ = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    altDown_ = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    shiftDown_ = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    winDown_ = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    ClearError();
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &SettingsWindow::HotkeyHook,
                                 GetModuleHandleW(nullptr), 0);
    Render();
}

void SettingsWindow::FinishHotkeyCapture(const core::HotkeySpec& spec) {
    core::HotkeySpec* slot = core::HotkeySlot(settings_, captureIndex_);
    if (slot == nullptr) {
        CancelHotkeyCapture();
        return;
    }
    // 先试着写进去再查冲突：FindHotkeyConflict 比的是「设置里现在的五把」，
    // 这样不用在 UI 里再拼一份临时 AppSettings。冲突就把原值放回去。
    const core::HotkeySpec previous = *slot;
    *slot = spec;
    const int conflict = core::FindHotkeyConflict(settings_, captureIndex_);
    if (conflict >= 0) {
        *slot = previous;
        ShowError(std::wstring(L"该快捷键已用于「") + HotkeySlotLabel(conflict) +
                  L"」，请换一个。");
        return;
    }
    CancelHotkeyCapture();
}

void SettingsWindow::TrackModifier(DWORD virtualKey, bool down) {
    switch (virtualKey) {
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_CONTROL:
        ctrlDown_ = down;
        break;
    case VK_LMENU:
    case VK_RMENU:
    case VK_MENU:
        altDown_ = down;
        break;
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_SHIFT:
        shiftDown_ = down;
        break;
    case VK_LWIN:
    case VK_RWIN:
        winDown_ = down;
        break;
    default:
        break;
    }
}

void SettingsWindow::CancelHotkeyCapture() {
    captureIndex_ = -1;
    if (keyHook_ != nullptr) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = nullptr;
    }
    g_hotkeyOwner = nullptr;
    ClearError();
    Render();
}

void SettingsWindow::ShowError(const std::wstring& message) {
    error_ = message;
    Render();
}

void SettingsWindow::ClearError() {
    error_.clear();
}

void SettingsWindow::Save() {
    // 捕获中不许保存：这一把还没定下来（1.x 直接把保存按钮禁用）。
    // 但必须给反馈——静默返回会让用户以为已经保存了。
    if (captureIndex_ >= 0) {
        ShowError(L"正在等待新的快捷键：按下组合键或按 Esc 取消后，再点保存。");
        return;
    }
    // 捕获时已经挡过冲突，但手改/1.x 迁移进来的文件可能本来就带着两把相同的键。
    for (int slot = 0; slot < core::kHotkeySlotCount; ++slot) {
        const int conflict = core::FindHotkeyConflict(settings_, slot);
        if (conflict >= 0) {
            page_ = Page::Hotkeys;
            ShowError(std::wstring(L"「") + HotkeySlotLabel(slot) + L"」与「" +
                      HotkeySlotLabel(conflict) + L"」使用了相同的快捷键，请更换其中一个。");
            return;
        }
    }
    if (!onSave_) {
        Close();
        return;
    }
    std::wstring error;
    if (onSave_(settings_, error)) {
        saved_ = true;
        Close();
        return;
    }
    ShowError(error.empty() ? L"保存失败，详见日志。" : error);
}

std::wstring SettingsWindow::SummaryText() const {
    std::wostringstream stream;
    stream << L"HaloCap " << Utf8ToWide(halocap::app::kHaloCapVersion) << L"\n";
    stream << L"OS build: " << facts_.osBuild << L"\n";
    stream << L"运行权限: " << (facts_.processElevated ? L"管理员" : L"标准用户") << L"\n";
    stream << L"前台窗口提权: "
           << (halocap::platform::win32::IsForegroundElevated() ? L"是" : L"否") << L"\n";

    stream << L"Models: " << Utf8ToWide(modelsDirectory_) << L"\n";
    return stream.str();
}

void SettingsWindow::CopySummary() {
    CopyTextToClipboard(SummaryText());
    MessageBoxW(window_, L"环境摘要已复制到剪贴板。", L"HaloCap", MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::CheckUpdate() {
    if (updateBusy_ || window_ == nullptr) {
        return;
    }
    updateBusy_ = true;
    error_.clear();
    status_ = L"正在检查更新…";
    Render();

    // update.url 可以指向镜像的 atom 源；没配就用 GitHub Releases 的公开 feed。
    const std::string overrideUrl = ReadUpdateUrl();
    const std::wstring feed =
        overrideUrl.empty()
            ? std::wstring(L"https://github.com/Joshua-Zion/HaloCap/releases.atom")
            : Utf8ToWide(overrideUrl);
    const HWND window = window_;

    // 网络请求放到独立线程：WinHTTP 超时 / 慢网都不能卡住设置窗口。
    std::thread([window, feed]() {
        auto* outcome = new UpdateCheckOutcome();
        std::string body;
        std::string networkError;
        if (!halocap::platform::win32::HttpGetUtf8(feed, &body, &networkError)) {
            outcome->error = L"检查更新失败：" + Utf8ToWide(networkError);
        } else {
            std::string tag;
            std::string parseError;
            if (core::ParseLatestTagFromAtom(body, &tag, &parseError)) {
                outcome->tag = Utf8ToWide(tag);
            } else {
                outcome->error = L"更新源格式无法识别：" + Utf8ToWide(parseError);
            }
        }
        if (PostMessageW(window, kMsgUpdateChecked, 0, reinterpret_cast<LPARAM>(outcome)) == 0) {
            delete outcome;
        }
    }).detach();
}

void SettingsWindow::BeginUpdateDownload(const std::wstring& version) {
    if (updateBusy_ || window_ == nullptr || version.empty()) {
        return;
    }
    updateBusy_ = true;
    error_.clear();
    status_ = L"正在下载更新 0%…";
    Render();

    const HWND window = window_;
    std::thread([window, version]() {
        wchar_t localAppData[MAX_PATH] = {};
        std::wstring directory;
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) != 0) {
            directory = std::wstring(localAppData) + L"\\HaloCap2\\update\\v" + version;
        } else {
            wchar_t temporary[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, temporary);
            directory = std::wstring(temporary) + L"HaloCapUpdate\\v" + version;
        }
        std::error_code ignored;
        std::filesystem::create_directories(directory, ignored);

        const std::wstring file = directory + L"\\HaloCap-" + version + L"-win-x64.zip";
        const std::wstring url =
            L"https://github.com/Joshua-Zion/HaloCap/releases/download/v" + version +
            L"/HaloCap-" + version + L"-win-x64.zip";

        auto* outcome = new UpdateDownloadOutcome();
        outcome->path = file;
        std::string error;
        const bool ok = halocap::platform::win32::HttpDownloadToFile(
            url, file,
            [window](std::int64_t received, std::int64_t total) {
                if (total <= 0) {
                    return;
                }
                int percent = static_cast<int>(received * 100 / total);
                percent = std::clamp(percent, 0, 100);
                PostMessageW(window, kMsgUpdateProgress, static_cast<WPARAM>(percent), 0);
            },
            &error);
        if (!ok) {
            outcome->error = L"下载失败：" + Utf8ToWide(error);
        }
        if (PostMessageW(window, kMsgUpdateDownloaded, 0, reinterpret_cast<LPARAM>(outcome)) == 0) {
            delete outcome;
        }
    }).detach();
}

void SettingsWindow::OpenFolder(const std::wstring& path) {
    if (!path.empty()) {
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void SettingsWindow::RefreshOcrModels() {
    ocrModels_.clear();
    // 内置模型：路径直接来自 app（打包后是程序目录的 models\ppocrv6-tiny-1）。
    if (!modelsDirectory_.empty()) {
        if (FilesInDirectory(Utf8ToWide(modelsDirectory_)).Complete()) {
            OcrModelEntry entry;
            entry.title = L"PP-OCRv6 极速（内置）";
            entry.detail = L"随程序提供";
            entry.builtin = true;
            ocrModels_.push_back(entry);
        }
    }
    // 用户目录里的每个子目录都是一套候选模型；可来自下载，也可手工放入。
    const std::wstring root = OcrModelRoot();
    std::vector<std::wstring> folders;
    std::error_code error;
    if (!root.empty()) {
        for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
            if (!entry.is_directory(error)) {
                continue;
            }
            if (!FilesInDirectory(entry.path().wstring()).Complete()) {
                continue;
            }
            folders.push_back(entry.path().filename().wstring());
        }
    }
    std::sort(folders.begin(), folders.end());
    for (const std::wstring& folder : folders) {
        OcrModelEntry entry;
        entry.id = folder;
        entry.title = folder == Utf8ToWide(core::kOcrModelSmallFolder)
                          ? L"PP-OCRv6 高精度"
                          : folder;
        entry.detail = L"用户模型 · " + folder;
        ocrModels_.push_back(entry);
    }
}

void SettingsWindow::BeginOcrModelDownload() {
    if (updateBusy_ || window_ == nullptr) {
        return;
    }
    const core::OcrModelDownload& download = core::OcrModelSmallDownload();
    const std::wstring directory =
        OcrModelRoot().empty() ? std::wstring()
                               : OcrModelRoot() + L"\\" + Utf8ToWide(download.folder);
    {
        std::error_code error;
        if (!directory.empty() && std::filesystem::exists(directory, error) &&
            FilesInDirectory(directory).Complete()) {
            status_ = L"官方模型已下载";
            Render();
            return;
        }
    }
    updateBusy_ = true;
    error_.clear();
    status_ = L"正在下载模型 0%…";
    Render();

    const HWND window = window_;
    std::thread([window, directory]() {
        auto* outcome = new OcrModelOutcome();
        if (directory.empty()) {
            outcome->error = L"无法定位本地模型目录";
        } else {
            std::error_code ignored;
            std::filesystem::create_directories(directory, ignored);
            const core::OcrModelDownload& files = core::OcrModelSmallDownload();
            const long long total = files.TotalBytes();
            long long done = 0;
            std::string error;
            struct Item {
                const char* url;
                const char* name;
                long long bytes;
            };
            const Item items[3] = {
                { files.detUrl, "PP-OCRv6_det_small.onnx", files.detBytes },
                { files.recUrl, "PP-OCRv6_rec_small.onnx", files.recBytes },
                { files.dictUrl, "ppocrv6_dict.txt", files.dictBytes },
            };
            bool ok = true;
            // 进度只在百分比变化时回主线程：每收一个数据块就 Render 一次会把
            // 消息队列塞满，窗口看起来像卡死。三个文件共用同一个百分比。
            auto lastPercent = std::make_shared<int>(-1);
            for (const Item& item : items) {
                const std::wstring target = directory + L"\\" + Utf8ToWide(item.name);
                const long long doneBefore = done;
                const bool fileOk = halocap::platform::win32::HttpDownloadToFile(
                    Utf8ToWide(item.url), target,
                    [window, doneBefore, total, lastPercent](std::int64_t received,
                                                             std::int64_t) {
                        if (total <= 0) {
                            return;
                        }
                        int percent = static_cast<int>((doneBefore + received) * 100 / total);
                        percent = std::clamp(percent, 0, 100);
                        if (percent == *lastPercent) {
                            return;
                        }
                        *lastPercent = percent;
                        PostMessageW(window, kMsgOcrModelProgress,
                                     static_cast<WPARAM>(percent), 0);
                    },
                    &error);
                if (!fileOk) {
                    ok = false;
                    break;
                }
                done = doneBefore + item.bytes;
            }
            if (ok) {
                outcome->folder = std::filesystem::path(directory).filename().wstring();
            } else {
                outcome->error = L"模型下载失败：" + Utf8ToWide(error);
            }
        }
        if (PostMessageW(window, kMsgOcrModelDownloaded, 0, reinterpret_cast<LPARAM>(outcome)) ==
            0) {
            delete outcome;
        }
    }).detach();
}

void SettingsWindow::ChooseDirectory() {
    if (pickerOpen_ || window_ == nullptr) {
        return;
    }
    pickerOpen_ = true;
    SetForegroundWindow(window_);
    const HWND window = window_;
    // 默认打开当前保存目录（已经是真正的路径；不存在时 SetFolder 会失败，退回系统默认位置）。
    const std::wstring initial = Utf8ToWide(settings_.recordingDirectory);

    // 文件夹选择放到独立 STA 线程。模态 Shell 对话框与主线程已有的 COM/MF/低级键盘钩子
    // 搅在一起时，会把主线程卡在 Show 的模态循环里（用户看到的「点目录卡死」）。独立
    // 线程有自己的干净 STA，主循环照常跑；结果用 PostMessage 把堆上的字符串交回来。
    // 线程只捕获 HWND 与初始路径的副本，不碰 SettingsWindow 对象，窗口先关掉也不会悬空。
    std::thread([window, initial]() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        std::wstring chosen;
        bool dialogAvailable = false;
        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&dialog)))) {
            dialogAvailable = true;
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            dialog->SetTitle(L"选择录制保存目录");
            if (!initial.empty()) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr,
                                                          IID_PPV_ARGS(&folder)))) {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            // 无 owner：对话框在本线程是顶层窗口，主线程的设置窗不被禁用、也不会被卡住。
            if (SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        chosen = path;
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (!dialogAvailable) {
            // 现代对话框建不出来才退回旧式浏览框（同样在这个线程的 STA 里）。
            PIDLIST_ABSOLUTE root = nullptr;
            if (!initial.empty()) {
                SHParseDisplayName(initial.c_str(), nullptr, &root, 0, nullptr);
            }
            BROWSEINFOW browse{};
            browse.hwndOwner = nullptr;
            browse.pidlRoot = root;   // 默认打开当前保存目录
            browse.lpszTitle = L"选择录制保存目录";
            browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
            PIDLIST_ABSOLUTE idList = SHBrowseForFolderW(&browse);
            if (idList != nullptr) {
                wchar_t path[MAX_PATH] = {};
                if (SHGetPathFromIDListW(idList, path)) {
                    chosen = path;
                }
                CoTaskMemFree(idList);
            }
            if (root != nullptr) {
                CoTaskMemFree(root);
            }
        }

        if (SUCCEEDED(com)) {
            CoUninitialize();
        }

        auto* result = new std::wstring(std::move(chosen));
        if (PostMessageW(window, kMsgDirectoryPicked, 0, reinterpret_cast<LPARAM>(result)) == 0) {
            delete result;
        }
    }).detach();
}
void SettingsWindow::AddHotspot(const RECT& rect, int id) {
    hotspots_.push_back(Hotspot{ rect, id });
}

int SettingsWindow::HitTest(POINT client) const {
    const float x = client.x / scale_;
    const float y = client.y / scale_;
    for (const Hotspot& spot : hotspots_) {
        if (x >= spot.rect.left && x < spot.rect.right && y >= spot.rect.top &&
            y < spot.rect.bottom) {
            return spot.id;
        }
    }
    return kIdNone;
}

void SettingsWindow::Click(int id) {
    if (id >= kIdNavBase && id < kIdNavBase + 6) {
        if (captureIndex_ >= 0) {
            CancelHotkeyCapture();
        }
        page_ = static_cast<Page>(id - kIdNavBase);
        if (page_ == Page::Ocr) {
            RefreshOcrModels();
        }
        scroll_ = 0;
        Render();
        return;
    }
    if (id == kIdClose || id == kIdCancel) {
        Close();
        return;
    }
    if (id == kIdSave) {
        Save();
        return;
    }
    if (id == kIdUpdate) {
        CheckUpdate();
        return;
    }
    if (id == kIdCopySummary) {
        CopySummary();
        return;
    }
    if (id == kIdDirectory) {
        ChooseDirectory();
        return;
    }
    if (id == kIdOcrModelDownload) {
        BeginOcrModelDownload();
        return;
    }
    if (id == kIdOcrModelOpen) {
        const std::wstring root = OcrModelRoot();
        if (!root.empty()) {
            std::error_code ignored;
            std::filesystem::create_directories(root, ignored);
            OpenFolder(root);
        }
        return;
    }
    if (id >= kIdOcrModelBase) {
        const std::size_t index = static_cast<std::size_t>(id - kIdOcrModelBase);
        if (index < ocrModels_.size()) {
            settings_.ocr.model = Utf8FromWide(ocrModels_[index].id);
            status_.clear();
            Render();
        }
        return;
    }
    if (id >= kIdHotkeyBase && id < kIdHotkeyBase + 5) {
        StartHotkeyCapture(id - kIdHotkeyBase);
        return;
    }
    if (id >= kIdSegmentBase) {
        const int group = (id - kIdSegmentBase) / 10;
        const int option = (id - kIdSegmentBase) % 10;
        switch (group) {
        case kGroupMenuSize:
            settings_.menuSize = static_cast<core::MenuSize>(option);
            break;
        case kGroupScreenshotFormat:
            settings_.screenshotFormat =
                option == 1 ? core::ScreenshotFormat::Jpeg : core::ScreenshotFormat::Png;
            break;
        case kGroupOcrEngine:
            settings_.ocr.engine =
                option == 0 ? core::OcrEngine::Native : core::OcrEngine::Model;
            break;
        case kGroupResolution:
            settings_.recording.resolution = static_cast<core::RecordingResolution>(option);
            break;
        case kGroupFrameRate:
            settings_.recording.frameRate = option == 1 ? core::RecordingFrameRate::Sixty
                                                         : core::RecordingFrameRate::Thirty;
            break;
        case kGroupQuality:
            settings_.recording.quality = static_cast<core::RecordingQuality>(option);
            break;
        case kGroupRange:
            settings_.recording.range = static_cast<core::RecordingRange>(option);
            break;
        case kGroupCountdown:
            settings_.recording.countdown = option == 0   ? core::RecordingCountdown::Off
                                            : option == 1 ? core::RecordingCountdown::ThreeSeconds
                                                          : core::RecordingCountdown::FiveSeconds;
            break;
        default:
            break;
        }
        Render();
        return;
    }
    switch (id) {
    case kIdToggleAutoStart: settings_.autoStart = !settings_.autoStart; break;
    case kIdToggleElevate: settings_.elevateOnStart = !settings_.elevateOnStart; break;
    case kIdToggleConvertFiles:
        settings_.convertFilePathsToImages = !settings_.convertFilePathsToImages;
        break;
    case kIdTogglePinGlow: settings_.pinGlowEnabled = !settings_.pinGlowEnabled; break;
    case kIdToggleAutoCopy: settings_.ocr.autoCopy = !settings_.ocr.autoCopy; break;
    case kIdToggleSystemAudio:
        settings_.recording.systemAudio = !settings_.recording.systemAudio;
        break;
    case kIdToggleMicrophone:
        settings_.recording.microphone = !settings_.recording.microphone;
        break;
    case kIdToggleRecordCursor:
        settings_.recording.recordCursor = !settings_.recording.recordCursor;
        break;
    case kIdToggleOcrAutoStart: settings_.ocr.autoStart = !settings_.ocr.autoStart; break;
    case kIdToggleRecordAutoStart:
        settings_.recording.autoStart = !settings_.recording.autoStart;
        break;
    default:
        return;
    }
    Render();
}

void SettingsWindow::BuildBackgroundCache() {
    backgroundCache_ = std::make_unique<SettingsBackgroundCache>();
    auto image = std::make_unique<Gdiplus::Bitmap>(widthPx_, heightPx_, PixelFormat32bppPARGB);
    Gdiplus::Graphics g(image.get());
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.Clear(Color(0, 0, 0, 0));
    Matrix backgroundTransform(scale_, 0.0f, 0.0f, scale_, 0.0f, 0.0f);
    g.SetTransform(&backgroundTransform);

    const Color textPrimary(255, 0x1D, 0x1D, 0x1F);
    const Color textTertiary(255, 0x8E, 0x8E, 0x93);
    const Color accent(255, 0x00, 0x7A, 0xFF);
    const Color sidebar(255, 0xED, 0xED, 0xF1);
    const Color content(255, 0xF7, 0xF7, 0xF9);
    const Color hairline(20, 0, 0, 0);
    const RectF frame(kMargin, kMargin, kWindowW - 2 * kMargin, kWindowH - 2 * kMargin);
    Fonts* fonts = GetFonts();

    // 真软阴影：形状 → 降采样 → 两次盒式模糊 → 拉伸合成（缓存里只做一次）。
    DrawSoftShadow(g, frame, kRadius, 0.45f, 20.0f, 6.0f, false);    FillRoundRect(g, frame, kRadius, Color(255, 255, 255, 255));
    FillRoundRect(g, frame, kRadius, content);
    StrokeRoundRect(g, frame, kRadius, Color(38, 0, 0, 0));

    GraphicsPath topBand;
    topBand.AddArc(frame.X, frame.Y, kRadius * 2, kRadius * 2, 180.0f, 90.0f);
    topBand.AddArc(frame.X + frame.Width - kRadius * 2, frame.Y, kRadius * 2, kRadius * 2, 270.0f,
                   90.0f);
    topBand.AddLine(frame.X + frame.Width, frame.Y + kTitleBar, frame.X, frame.Y + kTitleBar);
    topBand.CloseFigure();
    SolidBrush bandBrush(sidebar);
    g.FillPath(&bandBrush, &topBand);

    GraphicsPath sidebarPath;
    sidebarPath.AddLine(frame.X, frame.Y + kTitleBar, frame.X + kSidebar, frame.Y + kTitleBar);
    sidebarPath.AddLine(frame.X + kSidebar, frame.Y + kTitleBar, frame.X + kSidebar,
                        frame.Y + frame.Height);
    sidebarPath.AddLine(frame.X + kSidebar, frame.Y + frame.Height, frame.X + kRadius,
                        frame.Y + frame.Height);
    // 左下角必须是窗口的左下圆角；之前这里误用了 frame.X + frame.Width 的右角弧。
    sidebarPath.AddArc(frame.X, frame.Y + frame.Height - kRadius * 2, kRadius * 2, kRadius * 2,
                       90.0f, 90.0f);
    sidebarPath.CloseFigure();
    g.SetClip(RectF(frame.X, frame.Y, kSidebar, frame.Height));
    g.FillPath(&bandBrush, &sidebarPath);
    g.ResetClip();

    Pen sidebarBorder(Color(15, 0, 0, 0), 1.0f);
    g.DrawLine(&sidebarBorder, frame.X + kSidebar, frame.Y + kTitleBar, frame.X + kSidebar,
               frame.Y + frame.Height);
    Pen hairlinePen(hairline, 1.0f);
    g.DrawLine(&hairlinePen, frame.X + kSidebar, frame.Y + kTitleBar, frame.X + frame.Width,
               frame.Y + kTitleBar);

    const float logoX = frame.X + 20.0f;
    const float logoY = frame.Y + (kTitleBar - 18.0f) * 0.5f;
    Pen logoOuter(Color(34, 0, 0, 0), 1.0f);
    g.DrawEllipse(&logoOuter, logoX, logoY, 18.0f, 18.0f);
    Pen logoInner(accent, 1.3f);
    g.DrawEllipse(&logoInner, logoX + 3.0f, logoY + 3.0f, 12.0f, 12.0f);
    SolidBrush gold(Color(255, 0xF5, 0xB9, 0x63));
    g.FillEllipse(&gold, logoX + 7.5f, logoY + 7.5f, 3.0f, 3.0f);
    DrawText(g, L"HaloCap", &fonts->settingBold, RectF(logoX + 27.0f, logoY, 70.0f, 18.0f),
             textPrimary);
    DrawText(g, L"光圈", &fonts->desc, RectF(logoX + 96.0f, logoY + 1.0f, 40.0f, 16.0f),
             textTertiary);

    backgroundCache_->image = std::move(image);
}
void SettingsWindow::Render() {
    if (bits_ == nullptr || window_ == nullptr) {
        return;
    }
    Fonts* fonts = GetFonts();
    hotspots_.clear();

    Bitmap canvas(widthPx_, heightPx_, PixelFormat32bppPARGB);
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.Clear(Color(0, 0, 0, 0));
    Matrix transform(scale_, 0.0f, 0.0f, scale_, 0.0f, 0.0f);
    g.SetTransform(&transform);

    const Color textPrimary(255, 0x1D, 0x1D, 0x1F);
    const Color textSecondary(255, 0x6E, 0x6E, 0x73);
    const Color textTertiary(255, 0x8E, 0x8E, 0x93);
    const Color accent(255, 0x00, 0x7A, 0xFF);
    const Color sidebar(255, 0xED, 0xED, 0xF1);
    const Color content(255, 0xF7, 0xF7, 0xF9);
    const Color card(255, 255, 255, 255);
    const Color cardBorder(255, 0xE7, 0xE7, 0xEC);
    const Color hairline(20, 0, 0, 0);

    const RectF frame(kMargin, kMargin, kWindowW - 2 * kMargin, kWindowH - 2 * kMargin);

    if (backgroundCache_ == nullptr) {
        BuildBackgroundCache();
    }
    if (backgroundCache_ != nullptr && backgroundCache_->image) {
        g.DrawImage(backgroundCache_->image.get(), RectF(0.0f, 0.0f, kWindowW, kWindowH));
    }
    // 侧栏导航。
    const wchar_t* navTitles[6] = { L"通用", L"快捷键", L"截图与贴图", L"文字识别", L"录屏",
                                    L"关于" };

    float navY = frame.Y + kTitleBar + 14.0f;
    for (int i = 0; i < 6; ++i) {
        const RectF item(frame.X + 18.0f, navY, kSidebar - 32.0f, 34.0f);
        AddHotspot(RECT{ static_cast<LONG>(item.X), static_cast<LONG>(item.Y),
                         static_cast<LONG>(item.X + item.Width),
                         static_cast<LONG>(item.Y + item.Height) },
                   kIdNavBase + i);
        const bool selected = static_cast<int>(page_) == i;
        if (selected) {
            FillRoundRect(g, item, 8.0f, Color(255, 0xE0, 0xE0, 0xE6));
        } else if (hoverId_ == kIdNavBase + i) {
            FillRoundRect(g, item, 8.0f, Color(10, 0, 0, 0));
        }
        DrawNavIcon(g, i, PointF(item.X + 20.0f, item.Y + item.Height * 0.5f),
                    selected ? accent : Color(255, 0x8E, 0x8E, 0x93));
        DrawText(g, navTitles[i], selected ? &fonts->navBold : &fonts->nav,
                 RectF(item.X + 38.0f, item.Y, item.Width - 44.0f, item.Height),
                 selected ? textPrimary : Color(255, 0x6E, 0x6E, 0x73));
        navY += 36.0f;
    }

    // 内容区标题。
    const wchar_t* pageTitles[6] = { L"通用", L"快捷键", L"截图与贴图", L"文字识别", L"录屏",
                                     L"关于" };
    const wchar_t* pageSubtitles[6] = {
        L"启动方式、光圈大小与贴图外观",
        L"呼出光圈与四个动作的全局按键，单键会独占该键",
        L"图片格式与剪贴板行为",
        L"选择识别引擎、模型与结果偏好",
        L"录制范围、画面与声音",
        L"版本、更新与诊断信息",
    };
    const float contentLeft = frame.X + kSidebar;
    const float headerX = contentLeft + 30.0f;
    const float headerY = frame.Y + kTitleBar + 26.0f;
    DrawText(g, pageTitles[static_cast<int>(page_)], &fonts->title,
             RectF(headerX, headerY, 400.0f, 28.0f), textPrimary);
    DrawText(g, pageSubtitles[static_cast<int>(page_)], &fonts->subtitle,
             RectF(headerX, headerY + 31.0f, 460.0f, 18.0f), textSecondary);

    // 标题栏右上角的关闭按钮（与 WM_NCHITTEST 里留出的 inClose 区域一致）。
    {
        const RectF closeRect(kWindowW - kMargin - 14.0f - 28.0f, kMargin, 28.0f, kTitleBar);
        if (hoverId_ == kIdClose) {
            SolidBrush hover(Color(255, 0xEC, 0xEC, 0xF0));
            g.FillEllipse(&hover, closeRect.X + 4.0f, closeRect.Y + 11.0f, 20.0f, 20.0f);
        }
        Pen cross(hoverId_ == kIdClose ? textPrimary : textTertiary, 1.4f);
        const float cx = closeRect.X + 14.0f;
        const float cy = closeRect.Y + 21.0f;
        g.DrawLine(&cross, cx - 5.0f, cy - 5.0f, cx + 5.0f, cy + 5.0f);
        g.DrawLine(&cross, cx + 5.0f, cy - 5.0f, cx - 5.0f, cy + 5.0f);
        AddHotspot(RECT{ static_cast<LONG>(closeRect.X), static_cast<LONG>(closeRect.Y),
                         static_cast<LONG>(closeRect.X + closeRect.Width),
                         static_cast<LONG>(closeRect.Y + closeRect.Height) },
                   kIdClose);
    }

    const float scrollTop = headerY + 62.0f;
    const float scrollBottom = frame.Y + frame.Height - kFooter;
    const float scrollX = headerX;
    const float scrollW = frame.X + frame.Width - 30.0f - scrollX;

    g.SetClip(RectF(contentLeft, scrollTop - 6.0f, frame.Width - kSidebar,
                    scrollBottom - scrollTop + 8.0f));

    float y = scrollTop - scroll_;
    const float cardPad = 16.0f;
    const float rowTitle = 18.0f;

    const auto groupLabel = [&](const wchar_t* text) {
        DrawText(g, text, &fonts->group, RectF(scrollX + 4.0f, y, scrollW, 16.0f),
                 Color(255, 0x86, 0x86, 0x8B));
        y += 24.0f;
    };
    const auto cardStart = [&](float height) {
        const float top = y;
        FillRoundRect(g, RectF(scrollX, top, scrollW, height), 12.0f, card);
        StrokeRoundRect(g, RectF(scrollX, top, scrollW, height), 12.0f, cardBorder);
        y = top;
        return top;
    };
    const auto divider = [&](float cardX, float cardW) {
        Pen dividerPen(hairline, 1.0f);
        g.DrawLine(&dividerPen, cardX + 16.0f, y, cardX + cardW - 16.0f, y);
    };
    const auto switchAt = [&](float rowX, float rowW, float rowY, float rowH, int id,
                              bool on) {
        const float sw = 44.0f;
        const float sh = 24.0f;
        const float sx = rowX + rowW - cardPad - sw;
        const float sy = rowY + (rowH - sh) * 0.5f;
        AddHotspot(RECT{ static_cast<LONG>(sx), static_cast<LONG>(sy),
                         static_cast<LONG>(sx + sw), static_cast<LONG>(sy + sh) },
                   id);
        FillRoundRect(g, RectF(sx, sy, sw, sh), 12.0f,
                      on ? accent : Color(255, 0xE3, 0xE3, 0xE8));
        StrokeRoundRect(g, RectF(sx, sy, sw, sh), 12.0f,
                        on ? accent : Color(255, 0xD5, 0xD5, 0xDB));
        SolidBrush knob(card);
        g.FillEllipse(&knob, on ? sx + sw - 21.0f : sx + 3.0f, sy + 3.0f, 18.0f, 18.0f);
    };
    const auto segmentAt = [&](float rightX, float rowY, float rowH,
                               std::initializer_list<const wchar_t*> options, int group,
                               int selected, float optionWidth) {
        const int count = static_cast<int>(options.size());
        const float totalWidth = optionWidth * count + 4.0f;
        const float sx = rightX - totalWidth;
        const float sy = rowY + (rowH - 30.0f) * 0.5f;
        FillRoundRect(g, RectF(sx, sy, totalWidth, 30.0f), 8.0f, Color(255, 0xF0, 0xF0, 0xF3));
        StrokeRoundRect(g, RectF(sx, sy, totalWidth, 30.0f), 8.0f, Color(255, 0xE2, 0xE2, 0xE7));
        int index = 0;
        for (const wchar_t* option : options) {
            const float ox = sx + 2.0f + index * optionWidth;
            const RectF optionRect(ox, sy + 2.0f, optionWidth, 26.0f);
            AddHotspot(RECT{ static_cast<LONG>(optionRect.X), static_cast<LONG>(optionRect.Y),
                             static_cast<LONG>(optionRect.X + optionRect.Width),
                             static_cast<LONG>(optionRect.Y + optionRect.Height) },
                       kIdSegmentBase + group * 10 + index);
            if (index == selected) {
                FillRoundRect(g, optionRect, 6.0f, Color(255, 255, 255, 255));
                StrokeRoundRect(g, optionRect, 6.0f, Color(255, 0xDC, 0xDC, 0xE1));
            }
            DrawText(g, option, index == selected ? &fonts->settingBold : &fonts->setting,
                     optionRect, index == selected ? textPrimary : textSecondary,
                     StringAlignmentCenter);
            ++index;
        }
    };

    switch (page_) {
    case Page::General: {
        groupLabel(L"启动");
        cardStart(124.0f);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleAutoStart, settings_.autoStart);
        DrawText(g, L"开机自启", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, rowTitle),
                 textPrimary);
        DrawText(g, L"进入桌面后静默运行，仅驻留托盘", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 320, 16), textSecondary);
        y += 62.0f;
        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleElevate, settings_.elevateOnStart);
        DrawText(g, L"以管理员身份启动", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 300, rowTitle), textPrimary);
        DrawText(g, L"启动时请求提权，热键可覆盖任务管理器等窗口", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 340, 16), textSecondary);
        y += 62.0f;

        y += 20.0f;
        groupLabel(L"外观");
        cardStart(62.0f);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"小", L"中", L"大" }, kGroupMenuSize,
                  static_cast<int>(settings_.menuSize), 44.0f);
        DrawText(g, L"光圈尺寸", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 260, rowTitle), textPrimary);
        DrawText(g, L"以光标为中心展开，默认中号", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 280, 16), textSecondary);
        y += 62.0f;
        break;
    }
    case Page::Hotkeys: {
        groupLabel(L"全局快捷键");
        cardStart(5 * 56.0f);
        for (int i = 0; i < core::kHotkeySlotCount; ++i) {
            const float rowTop = y;
            if (i > 0) {
                divider(scrollX, scrollW);
            }
            DrawText(g, HotkeySlotLabel(i), &fonts->setting,
                     RectF(scrollX + 16.0f, rowTop + 12.0f, 260, 18), textPrimary);
            const float fieldW = 160.0f;
            const float fieldH = 34.0f;
            const float fieldX = scrollX + scrollW - cardPad - fieldW;
            const float fieldY = rowTop + (56.0f - fieldH) * 0.5f;
            const bool capturing = captureIndex_ == i;
            FillRoundRect(g, RectF(fieldX, fieldY, fieldW, fieldH), 7.0f,
                          Color(255, 255, 255, 255));
            StrokeRoundRect(g, RectF(fieldX, fieldY, fieldW, fieldH), 7.0f,
                            capturing ? accent : Color(255, 0xD9, 0xD9, 0xDE));
            const std::wstring text =
                capturing ? L"请按下快捷键…"
                          : Utf8ToWide(core::FormatHotkeyText(*core::HotkeySlot(settings_, i)));
            DrawText(g, text, &fonts->field, RectF(fieldX, fieldY, fieldW, fieldH),
                     capturing ? accent : textPrimary, StringAlignmentCenter);
            AddHotspot(RECT{ static_cast<LONG>(fieldX), static_cast<LONG>(fieldY),
                             static_cast<LONG>(fieldX + fieldW),
                             static_cast<LONG>(fieldY + fieldH) },
                       kIdHotkeyBase + i);
            y = rowTop + 56.0f;
        }

        // 1.x 的 UpdateHotkeyHint：有单键就点名哪几把，否则给通用说明。
        y += 12.0f;
        std::wstring hint;
        if (captureIndex_ >= 0) {
            hint = L"等待按键…  按 Esc 取消";
        } else {
            std::wstring singleKeys;
            for (int i = 0; i < core::kHotkeySlotCount; ++i) {
                if ((core::HotkeySlot(settings_, i)->modifiers & core::kStateModifiers) == 0) {
                    if (!singleKeys.empty()) {
                        singleKeys += L"、";
                    }
                    singleKeys += HotkeySlotLabel(i);
                }
            }
            hint = singleKeys.empty()
                       ? L"单击按键区域后按下新的组合键；单键会独占该键并优先于输入法，Esc 取消"
                       : singleKeys + L" 使用单键，会独占该键并优先于输入法；点击对应按键可重新设置。";
        }
        DrawText(g, hint, &fonts->desc, RectF(scrollX + 4.0f, y, scrollW - 8.0f, 32.0f),
                 textTertiary, StringAlignmentNear, StringAlignmentNear);
        y += 32.0f;
        break;
    }
    case Page::Capture: {
        groupLabel(L"图片");
        cardStart(124.0f);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"PNG", L"JPG" },
                  kGroupScreenshotFormat,
                  settings_.screenshotFormat == core::ScreenshotFormat::Jpeg ? 1 : 0, 64.0f);
        DrawText(g, L"保存格式", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 240, 18),
                 textPrimary);
        DrawText(g, L"截图与另存贴图时使用；PNG 无损，JPG 体积更小", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 360, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleConvertFiles,
                 settings_.convertFilePathsToImages);
        DrawText(g, L"图片文件转为图像", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 300, 18), textPrimary);
        DrawText(g, L"复制的图片文件以图像贴出；关闭后显示文件路径", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 360, 16), textSecondary);
        y += 62.0f;

        y += 20.0f;
        groupLabel(L"贴图");
        cardStart(62.0f);
        switchAt(scrollX, scrollW, y, 62.0f, kIdTogglePinGlow, settings_.pinGlowEnabled);
        DrawText(g, L"贴图边缘光晕", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 300, 18), textPrimary);
        DrawText(g, L"新贴图带一圈柔和光晕；也可以随时在右键菜单里切换", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 380, 16), textSecondary);
        y += 62.0f;
        break;
    }
    case Page::Ocr: {
        groupLabel(L"识别与结果");
        cardStart(186.0f);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"原生识别", L"模型识别" },
                  kGroupOcrEngine, settings_.ocr.engine == core::OcrEngine::Model ? 1 : 0,
                  100.0f);
        DrawText(g, L"识别引擎", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 200, 18),
                 textPrimary);
        DrawText(g, L"原生轻快，模型对中英混排更稳", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 240, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleAutoCopy, settings_.ocr.autoCopy);
        DrawText(g, L"自动复制", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, 18),
                 textPrimary);
        DrawText(g, L"识别成功后自动把全部文字复制到剪贴板", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 360, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleOcrAutoStart, settings_.ocr.autoStart);
        DrawText(g, L"选好区域后自动识别", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 300, 18), textPrimary);
        DrawText(g, L"关闭后，选好区域需按回车或点「识别」才开始", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 380, 16), textSecondary);
        y += 62.0f;

        // 模型目录只对「模型识别」有意义：原生引擎下不显示这一节。
        if (settings_.ocr.engine == core::OcrEngine::Model) {
            y += 20.0f;
            groupLabel(L"模型");
            {
                const int rows = std::max(1, static_cast<int>(ocrModels_.size()));
                const float rowHeight = 58.0f;
                cardStart(rowHeight * static_cast<float>(rows) + 62.0f + 28.0f);
                std::wstring selected = Utf8ToWide(settings_.ocr.model);
                const bool selectedFound =
                    std::any_of(ocrModels_.begin(), ocrModels_.end(),
                                [&](const OcrModelEntry& entry) { return entry.id == selected; });
                if (!selectedFound) {
                    selected.clear();
                }
                if (ocrModels_.empty()) {
                    DrawText(g, L"模型目录里还没有可用的模型", &fonts->setting,
                             RectF(scrollX + 16.0f, y + 12.0f, 300, 18), textPrimary);
                    DrawText(g, L"可下载官方模型，或把模型目录手动放入", &fonts->desc,
                             RectF(scrollX + 16.0f, y + 32.0f, 380, 16), textSecondary);
                    y += rowHeight;
                }
                for (int i = 0; i < static_cast<int>(ocrModels_.size()); ++i) {
                    const OcrModelEntry& entry = ocrModels_[static_cast<std::size_t>(i)];
                    const bool active = entry.id == selected;
                    const float rowTop = y;
                    if (i > 0) {
                        divider(scrollX, scrollW);
                    }
                    const float cx = scrollX + 28.0f;
                    const float cy = rowTop + 29.0f;
                    SolidBrush white(card);
                    g.FillEllipse(&white, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                    Pen ring(active ? accent : Color(255, 0xC9, 0xC9, 0xCF), 1.4f);
                    g.DrawEllipse(&ring, cx - 7.0f, cy - 7.0f, 14.0f, 14.0f);
                    if (active) {
                        SolidBrush dot(accent);
                        g.FillEllipse(&dot, cx - 3.5f, cy - 3.5f, 7.0f, 7.0f);
                    }
                    DrawText(g, entry.title, &fonts->setting,
                             RectF(scrollX + 46.0f, rowTop + 11.0f, 380.0f, 18.0f), textPrimary);
                    DrawText(g, entry.detail, &fonts->desc,
                             RectF(scrollX + 46.0f, rowTop + 32.0f, 430.0f, 16.0f),
                             textSecondary);
                    AddHotspot(RECT{ static_cast<LONG>(scrollX + 8.0f),
                                     static_cast<LONG>(rowTop + 4.0f),
                                     static_cast<LONG>(scrollX + scrollW - 8.0f),
                                     static_cast<LONG>(rowTop + rowHeight - 4.0f) },
                               kIdOcrModelBase + i);
                    y += rowHeight;
                }
    
                divider(scrollX, scrollW);
                {
                    const bool installed =
                        std::any_of(ocrModels_.begin(), ocrModels_.end(),
                                    [](const OcrModelEntry& entry) {
                                        return entry.id ==
                                               Utf8ToWide(core::kOcrModelSmallFolder);
                                    });
                    const core::OcrModelDownload& download = core::OcrModelSmallDownload();
                    DrawText(g, L"下载官方模型", &fonts->setting,
                             RectF(scrollX + 16.0f, y + 12.0f, 220, 18), textPrimary);
                    DrawText(g, installed
                                    ? L"PP-OCRv6 高精度已下载"
                                    : L"PP-OCRv6 高精度 · 约 " +
                                          std::to_wstring(download.TotalBytes() / (1024 * 1024)) +
                                          L" MB",
                             &fonts->desc, RectF(scrollX + 16.0f, y + 32.0f, 360, 16),
                             textSecondary);
                    const float buttonH = 30.0f;
                    const float by = y + (62.0f - buttonH) * 0.5f;
                    const float downloadW = 72.0f;
                    const float dx = scrollX + scrollW - cardPad - downloadW;
                    FillRoundRect(g, RectF(dx, by, downloadW, buttonH), 7.0f, accent);
                    DrawText(g, installed ? L"重新下载" : L"下载", &fonts->buttonBold,
                             RectF(dx, by, downloadW, buttonH), Color(255, 255, 255, 255),
                             StringAlignmentCenter);
                    AddHotspot(RECT{ static_cast<LONG>(dx), static_cast<LONG>(by),
                                     static_cast<LONG>(dx + downloadW),
                                     static_cast<LONG>(by + buttonH) },
                               kIdOcrModelDownload);
    
                    const float openW = 78.0f;
                    const float ox = dx - openW - 8.0f;
                    FillRoundRect(g, RectF(ox, by, openW, buttonH), 7.0f,
                                  Color(255, 255, 255, 255));
                    StrokeRoundRect(g, RectF(ox, by, openW, buttonH), 7.0f,
                                    Color(255, 0xD9, 0xD9, 0xDE));
                    DrawText(g, L"打开目录", &fonts->setting,
                             RectF(ox, by, openW, buttonH), textPrimary, StringAlignmentCenter);
                    AddHotspot(RECT{ static_cast<LONG>(ox), static_cast<LONG>(by),
                                     static_cast<LONG>(ox + openW),
                                     static_cast<LONG>(by + buttonH) },
                               kIdOcrModelOpen);
                    y += 62.0f;
                }
                DrawText(g,
                         L"模型按目录识别：每个子目录放 det / rec 的 onnx 与字典 txt 即可选择；"
                         L"方向分类模型可选，缺省用内置的。",
                         &fonts->desc, RectF(scrollX + 16.0f, y + 6.0f, scrollW - 32.0f, 16.0f),
                         textTertiary);
                y += 28.0f;
            }
        }

        y += 20.0f;
        groupLabel(L"说明");
        cardStart(158.0f);
        DrawText(g, L"Windows 原生", &fonts->settingBold,
                 RectF(scrollX + 16.0f, y + 14.0f, 200, 18), textPrimary);
        DrawText(g,
                 L"使用 Windows 自带的离线识别引擎，速度快、占用低；中文识别需要系统中"
                 L"安装「光学字符识别」语言组件。",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 34.0f, scrollW - 32.0f, 32),
                 textSecondary);
        Pen ocrDivider(hairline, 1.0f);
        g.DrawLine(&ocrDivider, scrollX + 16.0f, y + 74.0f, scrollX + scrollW - 16.0f,
                   y + 74.0f);
        DrawText(g, L"PP-OCRv6 模型", &fonts->settingBold,
                 RectF(scrollX + 16.0f, y + 82.0f, 200, 18), textPrimary);
        DrawText(g, L"模型目录中的模型都可以直接选用，包括下载的官方模型与自行放入的 ONNX 模型。",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 102.0f, scrollW - 32.0f, 18),
                 textSecondary);
        g.DrawLine(&ocrDivider, scrollX + 16.0f, y + 126.0f, scrollX + scrollW - 16.0f,
                   y + 126.0f);
        DrawText(g, L"贴图识别始终可用：在贴图上右键选择「识别文字」，或在非标注模式下按 O。",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 134.0f, scrollW - 32.0f, 16),
                 textSecondary);
        y += 158.0f;
        break;
    }
    case Page::Recording: {
        groupLabel(L"画面");
        cardStart(186.0f);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f,
                  { L"原始", L"2160p", L"1440p", L"1080p", L"720p" }, kGroupResolution,
                  static_cast<int>(settings_.recording.resolution), 54.0f);
        DrawText(g, L"输出分辨率", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 190, 18), textPrimary);
        DrawText(g, L"只缩小不放大；高于选区时按原始尺寸输出", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 180, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"30fps", L"60fps" },
                  kGroupFrameRate,
                  settings_.recording.frameRate == core::RecordingFrameRate::Sixty ? 1 : 0, 64.0f);
        DrawText(g, L"帧率", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 240, 18),
                 textPrimary);
        DrawText(g, L"30fps 足够流畅，60fps 需要更强的机器", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 360, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"高", L"均衡", L"紧凑" },
                  kGroupQuality, static_cast<int>(settings_.recording.quality), 56.0f);
        DrawText(g, L"画质", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 240, 18),
                 textPrimary);
        DrawText(g, L"均衡：码率随分辨率与帧率自动计算", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 270, 16), textSecondary);
        y += 62.0f;

        y += 20.0f;
        groupLabel(L"控制");
        cardStart(62.0f * 7 + 34.0f);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"区域", L"窗口" }, kGroupRange,
                  static_cast<int>(settings_.recording.range), 64.0f);
        DrawText(g, L"录制范围", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 240, 18), textPrimary);
        DrawText(g, L"区域：在覆盖层拖选；窗口：光标点选窗口", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 360, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleRecordAutoStart,
                 settings_.recording.autoStart);
        DrawText(g, L"选好区域后自动开始", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 300, 18), textPrimary);
        DrawText(g, L"关闭后，选好区域需按回车或点「开始录制」才开始", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 400, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        segmentAt(scrollX + scrollW - cardPad, y, 62.0f, { L"关", L"3 秒", L"5 秒" },
                  kGroupCountdown, CountdownOption(settings_.recording.countdown), 54.0f);
        DrawText(g, L"倒计时", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 240, 18),
                 textPrimary);
        DrawText(g, L"开始录制前的等待时间，Esc 可取消", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 270, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleSystemAudio,
                 settings_.recording.systemAudio);
        DrawText(g, L"系统声音", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, 18),
                 textPrimary);
        DrawText(g, L"录制系统正在播放的声音", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 320, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleMicrophone,
                 settings_.recording.microphone);
        DrawText(g, L"麦克风", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, 18),
                 textPrimary);
        DrawText(g, L"录制麦克风输入，与系统声音混音", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 340, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleRecordCursor,
                 settings_.recording.recordCursor);
        DrawText(g, L"录制光标", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, 18),
                 textPrimary);
        DrawText(g, L"在录制画面中显示鼠标指针", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 320, 16), textSecondary);
        y += 62.0f;

        divider(scrollX, scrollW);
        {
            const float rowTop = y;
            const float fieldH = 34.0f;
            const float fieldW = 220.0f;
            const float buttonW = 64.0f;
            const float gap = 8.0f;
            const float rightEdge = scrollX + scrollW - cardPad;
            const float fieldX = rightEdge - buttonW - gap - fieldW;
            const float fieldY = rowTop + (62.0f - fieldH) * 0.5f;
            // 输入框只做回显：不给它加热区，点击不触发选择；触发由右侧「浏览…」按钮负责。
            FillRoundRect(g, RectF(fieldX, fieldY, fieldW, fieldH), 7.0f,
                          Color(255, 255, 255, 255));
            StrokeRoundRect(g, RectF(fieldX, fieldY, fieldW, fieldH), 7.0f,
                            Color(255, 0xD9, 0xD9, 0xDE));
            std::wstring directory = Utf8ToWide(settings_.recordingDirectory);
            if (directory.empty()) {
                directory = L"跟随系统视频目录";
            }
            DrawText(g, Ellipsize(g, directory, &fonts->field, fieldW - 20.0f), &fonts->field,
                     RectF(fieldX + 10.0f, fieldY, fieldW - 20.0f, fieldH), textPrimary);

            const float buttonX = rightEdge - buttonW;
            FillRoundRect(g, RectF(buttonX, fieldY, buttonW, fieldH), 7.0f,
                          Color(255, 255, 255, 255));
            StrokeRoundRect(g, RectF(buttonX, fieldY, buttonW, fieldH), 7.0f,
                            Color(255, 0xD9, 0xD9, 0xDE));
            DrawText(g, L"浏览…", &fonts->button, RectF(buttonX, fieldY, buttonW, fieldH),
                     textPrimary, StringAlignmentCenter);
            AddHotspot(RECT{ static_cast<LONG>(buttonX), static_cast<LONG>(fieldY),
                             static_cast<LONG>(buttonX + buttonW),
                             static_cast<LONG>(fieldY + fieldH) },
                       kIdDirectory);
        }
        DrawText(g, L"保存目录", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 240, 18), textPrimary);
        DrawText(g, L"录制文件的保存位置", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 320, 16), textSecondary);
        y += 62.0f;
        break;
    }
    case Page::About: {
        groupLabel(L"版本");
        cardStart(96.0f);
        DrawText(g, L"HaloCap " + Utf8ToWide(halocap::app::kHaloCapVersion), &fonts->settingBold,
                 RectF(scrollX + 16.0f, y + 14.0f, 300, 22), textPrimary);
        DrawText(g, L"Windows 原生 2.0，配置与 1.x 同格式", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 38.0f, 340, 16), textSecondary);
        {
            const float buttonW = 96.0f;
            const float buttonH = 30.0f;
            const float bx = scrollX + scrollW - cardPad - buttonW * 2 - 8.0f;
            const float by = y + (96.0f - buttonH) * 0.5f;
            FillRoundRect(g, RectF(bx, by, buttonW, buttonH), 7.0f,
                          Color(255, 0xFF, 0xFF, 0xFF));
            StrokeRoundRect(g, RectF(bx, by, buttonW, buttonH), 7.0f,
                            Color(255, 0xD9, 0xD9, 0xDE));
            DrawText(g, L"复制摘要", &fonts->button, RectF(bx, by, buttonW, buttonH),
                     textPrimary, StringAlignmentCenter);
            AddHotspot(RECT{ static_cast<LONG>(bx), static_cast<LONG>(by),
                             static_cast<LONG>(bx + buttonW), static_cast<LONG>(by + buttonH) },
                       kIdCopySummary);

            FillRoundRect(g, RectF(bx + buttonW + 8.0f, by, buttonW, buttonH), 7.0f, accent);
            DrawText(g, L"检查更新", &fonts->buttonBold,
                     RectF(bx + buttonW + 8.0f, by, buttonW, buttonH), Color(255, 255, 255, 255),
                     StringAlignmentCenter);
            AddHotspot(RECT{ static_cast<LONG>(bx + buttonW + 8.0f), static_cast<LONG>(by),
                             static_cast<LONG>(bx + buttonW + 8.0f + buttonW),
                             static_cast<LONG>(by + buttonH) },
                       kIdUpdate);
        }
        y += 96.0f;

        y += 20.0f;
        groupLabel(L"诊断");
        cardStart(172.0f);
        DrawText(g, L"环境摘要", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 14.0f, 240, 18), textPrimary);
        DrawText(g, L"OS build " + std::to_wstring(facts_.osBuild) + L" · WGC " +
                     (facts_.graphicsCaptureSupported ? L"可用" : L"不可用"),
                 &fonts->desc, RectF(scrollX + 16.0f, y + 36.0f, scrollW - 32.0f, 16),
                 textSecondary);
        DrawText(g, L"运行权限：" + std::wstring(facts_.processElevated ? L"管理员" : L"标准用户") +
                     L" · 前台窗口：" +
                     (halocap::platform::win32::IsForegroundElevated() ? L"提权" : L"普通"),
                 &fonts->desc, RectF(scrollX + 16.0f, y + 58.0f, scrollW - 32.0f, 16),
                 textSecondary);
        DrawText(g, L"模型目录：" + Utf8ToWide(modelsDirectory_), &fonts->desc,
                 RectF(scrollX + 16.0f, y + 80.0f, scrollW - 32.0f, 16), textSecondary);
        DrawText(g, L"更新来源：GitHub Releases（可在 %LOCALAPPDATA%\\HaloCap2\\update.url 覆盖镜像）",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 102.0f, scrollW - 32.0f, 16),
                 textSecondary);
        DrawText(g, L"识别完全在本机完成，图片不会上传网络。", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 124.0f, scrollW - 32.0f, 16), textSecondary);
        y += 172.0f;
        break;
    }
    }

    contentHeight_ = static_cast<int>(y + scroll_ - scrollTop + 20.0f);
    viewHeightDip_ = static_cast<int>(scrollBottom - scrollTop);
    g.ResetClip();

    // 右侧滚动条（内容超出时）。
    const float viewHeight = scrollBottom - scrollTop;
    if (contentHeight_ > static_cast<int>(viewHeight)) {
        const float trackX = frame.X + frame.Width - 12.0f;
        const float thumbHeight =
            std::max(28.0f, viewHeight * viewHeight / static_cast<float>(contentHeight_));
        const float maxScroll = static_cast<float>(contentHeight_) - viewHeight;
        const float thumbY = scrollTop + (viewHeight - thumbHeight) *
                                              (static_cast<float>(scroll_) / maxScroll);
        FillRoundRect(g, RectF(trackX, thumbY, 6.0f, thumbHeight), 3.0f,
                      Color(51, 0, 0, 0));
    }

    // 底部：提示/错误横幅 + 取消/保存。
    const float footerTop = frame.Y + frame.Height - kFooter;
    Pen footerHairline(hairline, 1.0f);
    g.DrawLine(&footerHairline, frame.X + kSidebar, footerTop, frame.X + frame.Width, footerTop);

    const float saveW = 84.0f;
    const float cancelW = 72.0f;
    const float buttonH = 32.0f;
    const float buttonY = footerTop + (kFooter - buttonH) * 0.5f;
    const float saveX = frame.X + frame.Width - 30.0f - saveW;
    const float cancelX = saveX - 8.0f - cancelW;

    if (!error_.empty()) {
        // 1.x 的 ErrorBanner：#FFEDEB 底、#F4C7C2 边、#B3261E 字。两行放得下最长那条提示。
        const RectF banner(headerX, footerTop + 8.0f, cancelX - 16.0f - headerX, 44.0f);
        FillRoundRect(g, banner, 7.0f, Color(255, 0xFF, 0xED, 0xEB));
        StrokeRoundRect(g, banner, 7.0f, Color(255, 0xF4, 0xC7, 0xC2));
        DrawText(g, error_, &fonts->desc,
                 RectF(banner.X + 10.0f, banner.Y, banner.Width - 20.0f, banner.Height),
                 Color(255, 0xB3, 0x26, 0x1E));
    } else if (!status_.empty()) {
        // 检查更新 / 下载进度：中性文字，比「更改在保存后生效」醒目但不报错。
        DrawText(g, status_, &fonts->desc,
                 RectF(headerX, footerTop, cancelX - 16.0f - headerX, kFooter), textPrimary);
    } else {
        DrawText(g, L"更改在保存后生效", &fonts->desc,
                 RectF(headerX, footerTop, cancelX - 16.0f - headerX, kFooter), textTertiary);
    }

    {
        // 取消：白底 + 描边，和保存并排时是一个明确的按钮（之前透明底+浅灰字看不出来）。
        FillRoundRect(g, RectF(cancelX, buttonY, cancelW, buttonH), 7.0f,
                      Color(255, 255, 255, 255));
        StrokeRoundRect(g, RectF(cancelX, buttonY, cancelW, buttonH), 7.0f,
                        Color(255, 0xD9, 0xD9, 0xDE));
        DrawText(g, L"取消", &fonts->button, RectF(cancelX, buttonY, cancelW, buttonH),
                 textPrimary, StringAlignmentCenter);
        AddHotspot(RECT{ static_cast<LONG>(cancelX), static_cast<LONG>(buttonY),
                         static_cast<LONG>(cancelX + cancelW),
                         static_cast<LONG>(buttonY + buttonH) },
                   kIdCancel);

        // 捕获进行中保存是灰的，但**仍然可点**：点下去要给出「先按 Esc 取消」的横幅，
        // 否则点了没任何反应，用户会以为「保存过了、只是快捷键没注册上」。
        const bool saveEnabled = captureIndex_ < 0;
        FillRoundRect(g, RectF(saveX, buttonY, saveW, buttonH), 7.0f,
                      saveEnabled ? accent : Color(255, 0xB8, 0xD4, 0xF0));
        DrawText(g, L"保存", &fonts->buttonBold, RectF(saveX, buttonY, saveW, buttonH),
                 Color(255, 255, 255, 255), StringAlignmentCenter);
        AddHotspot(RECT{ static_cast<LONG>(saveX), static_cast<LONG>(buttonY),
                         static_cast<LONG>(saveX + saveW),
                         static_cast<LONG>(buttonY + buttonH) },
                   kIdSave);
    }

    // 拷贝到 DIB 并 UpdateLayeredWindow。
    const Rect lockRect(0, 0, widthPx_, heightPx_);
    BitmapData data{};
    if (canvas.LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &data) == Ok) {
        for (int row = 0; row < heightPx_; ++row) {
            std::memcpy(static_cast<std::uint8_t*>(bits_) +
                            static_cast<std::size_t>(row) * widthPx_ * 4,
                        static_cast<const std::uint8_t*>(data.Scan0) +
                            static_cast<std::size_t>(row) * data.Stride,
                        static_cast<std::size_t>(widthPx_) * 4);
        }
        canvas.UnlockBits(&data);
    }

    HDC screen = GetDC(nullptr);
    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    POINT destination{ windowRect.left, windowRect.top };
    SIZE windowSize{ widthPx_, heightPx_ };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(window_, screen, &destination, &windowSize, memory_, &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        if (self != nullptr) {
            self->window_ = window;
        }
    }
    if (self != nullptr) {
        return self->Handle(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT SettingsWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            const float x = point.x / scale_;
            const float y = point.y / scale_;
            const bool inClose = x >= kWindowW - kMargin - 14.0f - 28.0f &&
                                 x < kWindowW - kMargin - 14.0f && y >= kMargin &&
                                 y < kMargin + kTitleBar;
            if (y < kMargin + kTitleBar && !inClose) {
                return HTCAPTION;
            }
        }
        return hit;
    }
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int id = HitTest(point);
        if (id != hoverId_) {
            hoverId_ = id;
            SetCursor(LoadCursorW(nullptr, id != kIdNone ? IDC_HAND : IDC_ARROW));
            // 只有导航/关闭有 hover 视觉；悬停其它控件不重绘整窗，避免鼠标划过时卡顿。
            if (id == kIdClose || (id >= kIdNavBase && id < kIdNavBase + 6) ||
                hoverId_ == kIdClose || (hoverId_ >= kIdNavBase && hoverId_ < kIdNavBase + 6)) {
                Render();
            }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        pressedId_ = HitTest(point);
        if (pressedId_ != kIdNone) {
            SetCapture(window);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int id = HitTest(point);
        const int pressed = pressedId_;
        pressedId_ = kIdNone;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        if (id != kIdNone && id == pressed) {
            Click(id);
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        pressedId_ = kIdNone;
        return 0;
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int maxScroll = std::max(0, contentHeight_ - viewHeightDip_);
        scroll_ = std::clamp(scroll_ - delta / WHEEL_DELTA * 40, 0, maxScroll);
        Render();
        return 0;
    }
    case kMsgDirectoryPicked: {
        pickerOpen_ = false;
        auto* result = reinterpret_cast<std::wstring*>(lParam);
        if (result == nullptr) {
            return 0;
        }
        std::wstring chosen = *result;
        delete result;
        if (!chosen.empty()) {
            // 根目录保留反斜杠，其余去掉末尾分隔符，录制路径再自己拼 "\\"。
            while (chosen.size() > 3 && (chosen.back() == L'\\' || chosen.back() == L'/')) {
                chosen.pop_back();
            }
            settings_.recordingDirectory = Utf8FromWide(chosen);
            Render();
        }
        return 0;
    }
    case kMsgUpdateChecked: {
        updateBusy_ = false;
        auto* outcome = reinterpret_cast<UpdateCheckOutcome*>(lParam);
        if (outcome == nullptr) {
            return 0;
        }
        const std::wstring tag = outcome->tag;
        const std::wstring failure = outcome->error;
        delete outcome;
        status_.clear();
        Render();
        if (!failure.empty()) {
            MessageBoxW(window_, failure.c_str(), L"HaloCap 更新", MB_OK | MB_ICONWARNING);
            return 0;
        }
        if (core::IsNewerVersion(Utf8FromWide(tag), halocap::app::kHaloCapVersion)) {
            const std::wstring message =
                L"发现新版本 v" + tag + L"（当前 v" +
                Utf8ToWide(halocap::app::kHaloCapVersion) +
                L"）。\n\n是否现在下载更新？下载完成后会放在本地更新目录，关闭本程序后解压覆盖即可。";
            if (MessageBoxW(window_, message.c_str(), L"HaloCap 更新",
                            MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                BeginUpdateDownload(tag);
            }
        } else {
            const std::wstring message =
                L"已经是最新版本 v" + Utf8ToWide(halocap::app::kHaloCapVersion) + L"。";
            MessageBoxW(window_, message.c_str(), L"HaloCap 更新", MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    case kMsgUpdateProgress: {
        const int percent = static_cast<int>(wParam);
        status_ = percent >= 100 ? std::wstring(L"正在写入更新包…")
                                 : L"正在下载更新 " + std::to_wstring(percent) + L"%…";
        Render();
        return 0;
    }
    case kMsgUpdateDownloaded: {
        updateBusy_ = false;
        auto* outcome = reinterpret_cast<UpdateDownloadOutcome*>(lParam);
        if (outcome == nullptr) {
            return 0;
        }
        const std::wstring path = outcome->path;
        const std::wstring failure = outcome->error;
        delete outcome;
        if (!failure.empty()) {
            status_.clear();
            Render();
            MessageBoxW(window_, failure.c_str(), L"HaloCap 更新", MB_OK | MB_ICONWARNING);
            return 0;
        }
        status_ = L"更新已下载";
        Render();
        std::filesystem::path directory(path);
        directory = directory.parent_path();
        const std::wstring message =
            L"更新包已下载到：\n" + path +
            L"\n\n关闭 HaloCap 后把压缩包解压覆盖到程序目录即可完成更新。\n是否打开所在文件夹？";
        if (MessageBoxW(window_, message.c_str(), L"HaloCap 更新",
                        MB_YESNO | MB_ICONINFORMATION) == IDYES) {
            OpenFolder(directory.wstring());
        }
        return 0;
    }
    case kMsgOcrModelProgress: {
        ++modelProgressUpdates_;
        const int percent = static_cast<int>(wParam);
        status_ = percent >= 100 ? std::wstring(L"正在写入模型…")
                                 : L"正在下载模型 " + std::to_wstring(percent) + L"%…";
        Render();
        return 0;
    }
    case kMsgOcrModelDownloaded: {
        updateBusy_ = false;
        auto* outcome = reinterpret_cast<OcrModelOutcome*>(lParam);
        if (outcome == nullptr) {
            return 0;
        }
        const std::wstring failure = outcome->error;
        delete outcome;
        if (!failure.empty()) {
            status_.clear();
            Render();
            MessageBoxW(window_, failure.c_str(), L"HaloCap OCR", MB_OK | MB_ICONWARNING);
            return 0;
        }
        status_ = L"模型已下载，可在上方选择使用";
        RefreshOcrModels();
        Render();
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (captureIndex_ >= 0) {
                CancelHotkeyCapture();
            } else {
                Close();
            }
            return 0;
        }
        break;
    case WM_DESTROY:
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
