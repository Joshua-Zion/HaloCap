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
#include <vector>

#include "update.h"
#include "version.h"
#include "soft_shadow.h"
#include "win32_update_fetch.h"
#include "win32_autostart.h"

namespace halocap::app {

namespace {

using namespace Gdiplus;

constexpr wchar_t kClassName[] = L"HaloCapSettings";
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
    kIdTogglePinOcr = 34,
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

    ShowWindow(window_, SW_SHOW);
    // 普通窗口：打开时到前台一次（和正常应用一致），但不再置顶，随后可被其他窗口盖住。
    SetForegroundWindow(window_);
    Render();
    return true;
}

void SettingsWindow::ShowPageForTest(int index) {
    page_ = static_cast<Page>(std::clamp(index, 0, 5));
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

    stream << L"Models: " << Utf8ToWide(modelsDirectory_) << L"\n";
    return stream.str();
}

void SettingsWindow::CopySummary() {
    CopyTextToClipboard(SummaryText());
    MessageBoxW(window_, L"环境摘要已复制到剪贴板。", L"HaloCap", MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::CheckUpdate() {
    std::string json;
    std::wstring source = L"本地清单";
    const std::string url = ReadUpdateUrl();
    if (!url.empty()) {
        std::string networkError;
        if (halocap::platform::win32::HttpGetUtf8(Utf8ToWide(url), &json, &networkError) &&
            !json.empty()) {
            source = L"网络清单";
        } else {
            json.clear();
        }
    }
    if (json.empty()) {
        json = ReadLocalUpdateManifest();
    }
    if (json.empty()) {
        MessageBoxW(window_,
                    L"未找到更新清单：可在 %LOCALAPPDATA%\\HaloCap2\\update.url 写清单地址，"
                    L"或放 update.json。",
                    L"HaloCap 更新", MB_OK | MB_ICONINFORMATION);
        return;
    }
    core::UpdateManifest manifest;
    std::string error;
    if (!core::ParseUpdateManifest(json, &manifest, &error)) {
        MessageBoxW(window_, L"更新清单解析失败，详见日志。", L"HaloCap 更新",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    const bool newer = core::IsNewerVersion(manifest.version, halocap::app::kHaloCapVersion);
    std::wstring message = L"当前版本：" + Utf8ToWide(halocap::app::kHaloCapVersion) +
                           L"\n最新版本：" + Utf8ToWide(manifest.version) + L"\n\n";
    message += newer ? L"发现新版本，可从清单中的地址下载。" : L"已经是最新版本。";
    if (newer) {
        message += L"\n" + Utf8ToWide(manifest.url);
    }
    message += L"\n来源：" + source;
    MessageBoxW(window_, message.c_str(), L"HaloCap 更新", MB_OK | MB_ICONINFORMATION);
}

void SettingsWindow::ChooseDirectory() {
    std::wstring chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog)))) {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        dialog->SetTitle(L"选择录制保存目录");
        if (SUCCEEDED(dialog->Show(window_))) {
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

    if (chosen.empty()) {
        BROWSEINFOW browse{};
        browse.hwndOwner = window_;
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
    }

    if (!chosen.empty()) {
        // 根目录保留反斜杠，其余去掉末尾分隔符，录制路径再自己拼 "\\"。
        while (chosen.size() > 3 && (chosen.back() == L'\\' || chosen.back() == L'/')) {
            chosen.pop_back();
        }
        settings_.recordingDirectory = Utf8FromWide(chosen);
        Render();
    }
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
    case kIdToggleConvertFiles:
        settings_.convertFilePathsToImages = !settings_.convertFilePathsToImages;
        break;
    case kIdTogglePinGlow: settings_.pinGlowEnabled = !settings_.pinGlowEnabled; break;
    case kIdToggleAutoCopy: settings_.ocr.autoCopy = !settings_.ocr.autoCopy; break;
    case kIdTogglePinOcr: settings_.ocr.pinOcrEnabled = !settings_.ocr.pinOcrEnabled; break;
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
        L"选择识别引擎与结果偏好",
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
        cardStart(62.0f);
        switchAt(scrollX, scrollW, y, 62.0f, kIdToggleAutoStart, settings_.autoStart);
        DrawText(g, L"开机自启", &fonts->setting, RectF(scrollX + 16.0f, y + 12.0f, 300, rowTitle),
                 textPrimary);
        DrawText(g, L"进入桌面后静默运行，仅驻留托盘", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 320, 16), textSecondary);
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
        groupLabel(L"识别");
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

        y += 20.0f;
        groupLabel(L"引擎说明");
        cardStart(128.0f);
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
        DrawText(g, L"PP-OCRv6 tiny", &fonts->settingBold,
                 RectF(scrollX + 16.0f, y + 82.0f, 200, 18), textPrimary);
        DrawText(g, L"内置约 6 MB 的检测与识别模型，首次使用会解压到本地；对中英混排和小字更稳。",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 102.0f, scrollW - 32.0f, 18),
                 textSecondary);
        y += 128.0f;

        y += 20.0f;
        groupLabel(L"入口");
        cardStart(62.0f);
        switchAt(scrollX, scrollW, y, 62.0f, kIdTogglePinOcr, settings_.ocr.pinOcrEnabled);
        DrawText(g, L"贴图右键识别文字", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 12.0f, 320, 18), textPrimary);
        DrawText(g, L"在贴图上右键或按 O 识别原图，识别结果不包含标注", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 32.0f, 380, 16), textSecondary);
        y += 62.0f;
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
            const float fieldW = 300.0f;
            const float fieldH = 34.0f;
            const float fieldX = scrollX + scrollW - cardPad - fieldW;
            const float fieldY = rowTop + (62.0f - fieldH) * 0.5f;
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
            AddHotspot(RECT{ static_cast<LONG>(fieldX), static_cast<LONG>(fieldY),
                             static_cast<LONG>(fieldX + fieldW),
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
        cardStart(150.0f);
        DrawText(g, L"环境摘要", &fonts->setting,
                 RectF(scrollX + 16.0f, y + 14.0f, 240, 18), textPrimary);
        DrawText(g, L"OS build " + std::to_wstring(facts_.osBuild) + L" · WGC " +
                     (facts_.graphicsCaptureSupported ? L"可用" : L"不可用"),
                 &fonts->desc, RectF(scrollX + 16.0f, y + 36.0f, scrollW - 32.0f, 16),
                 textSecondary);
        DrawText(g, L"模型目录：" + Utf8ToWide(modelsDirectory_), &fonts->desc,
                 RectF(scrollX + 16.0f, y + 58.0f, scrollW - 32.0f, 16), textSecondary);
        DrawText(g, L"更新清单：%LOCALAPPDATA%\\HaloCap2\\update.url（网络）或 update.json（本地）",
                 &fonts->desc, RectF(scrollX + 16.0f, y + 80.0f, scrollW - 32.0f, 16),
                 textSecondary);
        DrawText(g, L"识别完全在本机完成，图片不会上传网络。", &fonts->desc,
                 RectF(scrollX + 16.0f, y + 102.0f, scrollW - 32.0f, 16), textSecondary);
        y += 150.0f;
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
    } else {
        DrawText(g, L"更改在保存后生效", &fonts->desc,
                 RectF(headerX, footerTop, cancelX - 16.0f - headerX, kFooter), textTertiary);
    }

    {
        FillRoundRect(g, RectF(cancelX, buttonY, cancelW, buttonH), 7.0f, Color(0, 0, 0, 0));
        DrawText(g, L"取消", &fonts->button, RectF(cancelX, buttonY, cancelW, buttonH),
                 textSecondary, StringAlignmentCenter);
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
        const int id = HitTest(point);
        if (id != kIdNone) {
            Click(id);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        const int maxScroll = std::max(0, contentHeight_ - viewHeightDip_);
        scroll_ = std::clamp(scroll_ - delta / WHEEL_DELTA * 40, 0, maxScroll);
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
