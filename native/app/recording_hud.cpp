#include "recording_hud.h"

#include "soft_shadow.h"

#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <string>

// Win10 2004 起才有；老 SDK 头文件里没有这个常量。
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

namespace halocap::app {

using namespace Gdiplus;

namespace {

constexpr wchar_t kFrameClass[] = L"HaloCapRecordingFrame";
constexpr wchar_t kCountdownClass[] = L"HaloCapRecordingCountdown";
constexpr wchar_t kHudClass[] = L"HaloCapRecordingHud";

// 浮层卡片之外的阴影留白（DIP）。阴影模糊 8.5 + 偏移 3 恰好放进 20。
constexpr float kHudCardWidth = 396.0f;
constexpr float kHudCardHeight = 46.0f;
constexpr float kHudPad = 20.0f;

bool EnsureGdiplus() {
    static ULONG_PTR token = 0;
    if (token == 0) {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
            return false;
        }
    }
    return true;
}

bool CreateLayeredBitmap(HDC* memory, HBITMAP* bitmap, void** bits, int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HBITMAP dib = CreateDIBSection(dc, &info, DIB_RGB_COLORS, bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (dib == nullptr || *bits == nullptr) {
        DeleteDC(dc);
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        return false;
    }
    SelectObject(dc, dib);
    *memory = dc;
    *bitmap = dib;
    return true;
}

void ReleaseLayeredBitmap(HDC* memory, HBITMAP* bitmap, void** bits) {
    if (*bitmap != nullptr) {
        DeleteObject(*bitmap);
        *bitmap = nullptr;
    }
    if (*memory != nullptr) {
        DeleteDC(*memory);
        *memory = nullptr;
    }
    *bits = nullptr;
}

void PresentLayered(HWND window, HDC memory, int width, int height, int x, int y, BYTE alpha) {
    HDC screen = GetDC(nullptr);
    POINT destination{ x, y };
    SIZE size{ width, height };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = alpha;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(window, screen, &destination, &size, memory, &source, 0, &blend,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

float MonitorScale(POINT screen) {
    const HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        return 1.0f;
    }
    return dpiX / 96.0f;
}

core::IntRect PrimaryMonitorRect() {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(MonitorFromPoint(POINT{ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &info)) {
        return core::IntRect{ info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right,
                              info.rcMonitor.bottom };
    }
    return core::IntRect{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
}

core::IntRect AnchorOrPrimary(const core::IntRect& anchor) {
    return anchor.IsEmpty() ? PrimaryMonitorRect() : anchor;
}

POINT CenterOf(const core::IntRect& rect) {
    return POINT{ (rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2 };
}

bool SameRect(const core::IntRect& a, const core::IntRect& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void ExcludeFromCapture(HWND window) {
    SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE);
}

void RoundRectPath(GraphicsPath& path, const RectF& rect, float radius) {
    const float r = std::min(radius, std::min(rect.Width, rect.Height) * 0.5f);
    const float d = r * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

void PumpOnce() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

float EaseOutCubic(float t) {
    const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - u * u * u;
}

float EaseOutQuad(float t) {
    const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
    return 1.0f - u * u;
}

std::wstring FormatHudTime(std::int64_t elapsedMs) {
    const std::int64_t total = std::max<std::int64_t>(0, elapsedMs / 1000);
    const std::int64_t hours = total / 3600;
    const std::int64_t minutes = (total % 3600) / 60;
    const std::int64_t seconds = total % 60;
    wchar_t buffer[24] = {};
    if (hours > 0) {
        std::swprintf(buffer, 24, L"%lld:%02lld:%02lld", hours, minutes, seconds);
    } else {
        std::swprintf(buffer, 24, L"%02lld:%02lld", minutes, seconds);
    }
    return buffer;
}

// 1.x 的 MeterWidth：宽度按 sqrt(电平) 展开，小音量也看得见。
float MeterWidthDip(float level) {
    const float value = std::clamp(level, 0.0f, 1.0f);
    return std::round(46.0f * std::sqrt(value) * 10.0f) / 10.0f;
}

}  // namespace

// ---- 录制边框 ----

bool RecordingFrame::Show(const core::IntRect& screenRect) {
    if (window_ != nullptr || screenRect.IsEmpty() || !EnsureGdiplus()) {
        return window_ != nullptr;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RecordingFrame::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kFrameClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return false;
    }

    rect_ = screenRect;
    scale_ = MonitorScale(CenterOf(rect_));
    const int width = rect_.right - rect_.left;
    const int height = rect_.bottom - rect_.top;
    if (!EnsureSurface(width, height)) {
        return false;
    }

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
                                  WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                              kFrameClass, L"HaloCap recording frame", WS_POPUP, rect_.left,
                              rect_.top, width, height, nullptr, nullptr,
                              GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        ReleaseSurface();
        return false;
    }
    ExcludeFromCapture(window_);
    Render();
    ShowWindow(window_, SW_SHOWNOACTIVATE);

    // 160ms QuadraticEase 淡入（与 1.x 的 ShowFrame 动画同参数）。
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const float t = static_cast<float>(GetTickCount64() - start) / 160.0f;
        if (t >= 1.0f) {
            break;
        }
        Present(static_cast<BYTE>(255 * EaseOutQuad(t)));
        PumpOnce();
        Sleep(10);
    }
    Present(255);
    return true;
}

void RecordingFrame::UpdateRect(const core::IntRect& screenRect) {
    if (window_ == nullptr || screenRect.IsEmpty() || SameRect(screenRect, rect_)) {
        return;
    }
    rect_ = screenRect;
    scale_ = MonitorScale(CenterOf(rect_));
    const int width = rect_.right - rect_.left;
    const int height = rect_.bottom - rect_.top;
    if (!EnsureSurface(width, height)) {
        return;
    }
    SetWindowPos(window_, HWND_TOPMOST, rect_.left, rect_.top, width, height, SWP_NOACTIVATE);
    Render();
    Present(255);
}

void RecordingFrame::Close() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    ReleaseSurface();
    rect_ = {};
}

bool RecordingFrame::EnsureSurface(int width, int height) {
    if (memory_ != nullptr && width == width_ && height == height_) {
        return true;
    }
    ReleaseSurface();
    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width, height)) {
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

void RecordingFrame::ReleaseSurface() {
    ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
    width_ = 0;
    height_ = 0;
}

void RecordingFrame::Render() {
    if (bits_ == nullptr) {
        return;
    }
    Bitmap canvas(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                  static_cast<BYTE*>(bits_));
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    const float outer = std::max(2.0f, std::round(2.0f * scale_));
    const float inner = std::max(1.0f, std::round(1.0f * scale_));
    Pen red(Color(255, 0xFF, 0x7A, 0x7A), outer);
    g.DrawRectangle(&red, outer * 0.5f, outer * 0.5f, width_ - outer, height_ - outer);
    const float inset = outer + inner * 0.5f;
    if (width_ - inset * 2.0f > 0.0f && height_ - inset * 2.0f > 0.0f) {
        Pen dark(Color(0x59, 0, 0, 0), inner);
        g.DrawRectangle(&dark, inset, inset, width_ - inset * 2.0f, height_ - inset * 2.0f);
    }
}

void RecordingFrame::Present(BYTE alpha) {
    if (window_ != nullptr) {
        PresentLayered(window_, memory_, width_, height_, rect_.left, rect_.top, alpha);
    }
}

LRESULT CALLBACK RecordingFrame::WndProc(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingFrame*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingFrame*>(create->lpCreateParams);
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

LRESULT RecordingFrame::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---- 录制倒计时 ----

bool RecordingCountdown::Show(int seconds, const core::IntRect& anchorScreen,
                              const std::function<void(HWND)>& probe) {
    if (seconds <= 0 || window_ != nullptr) {
        return true;
    }
    if (!EnsureGdiplus()) {
        return true;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RecordingCountdown::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kCountdownClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return true;
    }

    const core::IntRect anchor = AnchorOrPrimary(anchorScreen);
    scale_ = MonitorScale(CenterOf(anchor));
    width_ = static_cast<int>(std::lround(148.0f * scale_));
    height_ = width_;
    screenX_ = anchor.left + (anchor.right - anchor.left - width_) / 2;
    screenY_ = anchor.top + (anchor.bottom - anchor.top - height_) / 2;

    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width_, height_)) {
        return true;
    }
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kCountdownClass, L"HaloCap countdown", WS_POPUP, screenX_, screenY_,
                              width_, height_, nullptr, nullptr, GetModuleHandleW(nullptr),
                              this);
    if (window_ == nullptr) {
        ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
        return true;
    }
    ExcludeFromCapture(window_);

    bool cancelled = false;
    bool probed = probe == nullptr;
    for (int left = seconds; left > 0 && !cancelled; --left) {
        const ULONGLONG tickStart = GetTickCount64();
        Render(left, 1.28f, 0.55f);
        Present();
        ShowWindow(window_, SW_SHOWNOACTIVATE);

        while (GetTickCount64() - tickStart < 1000) {
            PumpOnce();
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                cancelled = true;
                break;
            }
            const ULONGLONG elapsed = GetTickCount64() - tickStart;
            if (elapsed < 240) {
                // tick 动画：数字 1.28→1 缩放 220ms CubicEaseOut，透明度 0.55→1 160ms。
                const float numberScale =
                    1.28f - 0.28f * EaseOutCubic(static_cast<float>(elapsed) / 220.0f);
                const float numberAlpha =
                    0.55f + 0.45f * std::min(1.0f, static_cast<float>(elapsed) / 160.0f);
                Render(left, numberScale, numberAlpha);
                Present();
            } else if (!probed) {
                Render(left, 1.0f, 1.0f);
                Present();
                probed = true;
                probe(window_);
            }
            Sleep(16);
        }
    }

    DestroyWindow(window_);
    window_ = nullptr;
    ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
    return !cancelled;
}

void RecordingCountdown::Render(int secondsLeft, float numberScale, float numberAlpha) {
    if (bits_ == nullptr) {
        return;
    }
    Bitmap canvas(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                  static_cast<BYTE*>(bits_));
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    const float s = scale_;
    const RectF card(15.0f * s, 15.0f * s, 118.0f * s, 118.0f * s);
    DrawSoftShadow(g, card, 30.0f * s, 0.35f, 5.5f * s, 4.0f * s, false);

    GraphicsPath cardPath;
    RoundRectPath(cardPath, card, 30.0f * s);
    SolidBrush background(Color(0xF2, 255, 255, 255));
    g.FillPath(&background, &cardPath);
    Pen border(Color(0x1F, 0, 0, 0), std::max(1.0f, s));
    g.DrawPath(&border, &cardPath);

    StringFormat center;
    center.SetAlignment(StringAlignmentCenter);
    center.SetLineAlignment(StringAlignmentCenter);

    const std::wstring number = std::to_wstring(secondsLeft);
    Font numberFont(L"Segoe UI", 54.0f * s, FontStyleBold, UnitPixel);
    SolidBrush numberBrush(
        Color(static_cast<BYTE>(255 * std::clamp(numberAlpha, 0.0f, 1.0f)), 0x1C, 0x1E, 0x24));
    const float cx = card.X + card.Width * 0.5f;
    const float cy = card.Y + card.Height * 0.5f - 8.0f * s;
    g.TranslateTransform(cx, cy);
    g.ScaleTransform(numberScale, numberScale);
    g.DrawString(number.c_str(), static_cast<INT>(number.size()), &numberFont,
                 RectF(-59.0f * s, -37.0f * s, 118.0f * s, 74.0f * s), &center, &numberBrush);
    g.ResetTransform();

    Font captionFont(L"Microsoft YaHei UI", 10.5f * s, FontStyleRegular, UnitPixel);
    SolidBrush captionBrush(Color(255, 0x75, 0x7A, 0x88));
    g.DrawString(L"开始录屏", 4, &captionFont,
                 RectF(card.X, card.GetBottom() - 31.0f * s, card.Width, 14.0f * s), &center,
                 &captionBrush);
}

void RecordingCountdown::Present() {
    if (window_ != nullptr) {
        PresentLayered(window_, memory_, width_, height_, screenX_, screenY_, 255);
    }
}

LRESULT CALLBACK RecordingCountdown::WndProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingCountdown*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingCountdown*>(create->lpCreateParams);
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

LRESULT RecordingCountdown::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_DESTROY) {
        window_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---- 录制浮层 ----

bool RecordingHud::Create(const core::IntRect& anchorScreen) {
    if (!EnsureGdiplus() || window_ != nullptr) {
        return window_ != nullptr;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RecordingHud::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kHudClass;
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return false;
    }

    anchor_ = AnchorOrPrimary(anchorScreen);
    scale_ = MonitorScale(CenterOf(anchor_));
    width_ = static_cast<int>(std::lround((kHudCardWidth + kHudPad * 2.0f) * scale_));
    height_ = static_cast<int>(std::lround((kHudCardHeight + kHudPad * 2.0f) * scale_));
    padPx_ = static_cast<int>(std::lround(kHudPad * scale_));

    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width_, height_)) {
        return false;
    }
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kHudClass, L"HaloCap recording", WS_POPUP, 0, 0, width_, height_,
                              nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
        return false;
    }
    ExcludeFromCapture(window_);
    PlaceWindow();

    pulseStart_ = GetTickCount64();
    Render();
    ShowWindow(window_, SW_SHOWNOACTIVATE);

    // 140ms CubicEase 淡入（1.x ShowHud 的同款开场）。
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const float t = static_cast<float>(GetTickCount64() - start) / 140.0f;
        if (t >= 1.0f) {
            break;
        }
        Present(static_cast<BYTE>(255 * EaseOutCubic(t)));
        PumpOnce();
        Sleep(10);
    }
    Present(255);
    return true;
}

void RecordingHud::Close() {
    if (window_ != nullptr) {
        if (GetCapture() == window_) {
            ReleaseCapture();
        }
        DestroyWindow(window_);
        window_ = nullptr;
    }
    ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
    width_ = 0;
    height_ = 0;
    state_ = {};
    hasRenderedState_ = false;
    displaySystemLevel_ = 0.0f;
    displayMicLevel_ = 0.0f;
    hovered_ = 0;
    pressed_ = 0;
    buttonsDisabled_ = false;
    trackingMouse_ = false;
    pending_ = Action::None;
}

void RecordingHud::PlaceWindow() {
    const int cardWidth = width_ - padPx_ * 2;
    const int cardHeight = height_ - padPx_ * 2;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(CenterOf(anchor_), MONITOR_DEFAULTTONEAREST), &info);
    const RECT area = info.rcWork;
    const int margin = 10;

    // 与 1.x PlaceWindow 逐行同源：上方优先，放不下翻下方，再夹进工作区（±4）。
    int left = anchor_.left + (anchor_.right - anchor_.left - cardWidth) / 2;
    int top = anchor_.top - cardHeight - margin;
    if (top < area.top + 4) {
        top = anchor_.bottom + margin;
    }
    if (top + cardHeight > area.bottom - 4) {
        top = anchor_.top + margin;
    }
    left = std::clamp(left, static_cast<int>(area.left) + 4,
                      std::max(static_cast<int>(area.left) + 4,
                               static_cast<int>(area.right) - cardWidth - 4));
    top = std::clamp(top, static_cast<int>(area.top) + 4,
                     std::max(static_cast<int>(area.top) + 4,
                              static_cast<int>(area.bottom) - cardHeight - 4));

    screenX_ = left - padPx_;
    screenY_ = top - padPx_;
    if (window_ != nullptr) {
        SetWindowPos(window_, HWND_TOPMOST, screenX_, screenY_, 0, 0,
                     SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
}

void RecordingHud::UpdateAnchor(const core::IntRect& anchorScreen) {
    if (anchorScreen.IsEmpty() || SameRect(anchorScreen, anchor_)) {
        return;
    }
    anchor_ = anchorScreen;
    if (window_ != nullptr) {
        PlaceWindow();
    }
}

void RecordingHud::Update(const RecordingHudState& state) {
    if (window_ == nullptr) {
        return;
    }
    // 电平表回落：新峰值立即上去，旧值每帧衰减 20%，避免条子乱抖。
    displaySystemLevel_ =
        state.showSystemLevel ? std::max(state.systemLevel, displaySystemLevel_ * 0.8f) : 0.0f;
    displayMicLevel_ =
        state.showMicLevel ? std::max(state.micLevel, displayMicLevel_ * 0.8f) : 0.0f;

    const bool changed = !hasRenderedState_ || state.elapsedMs != state_.elapsedMs ||
                         state.paused != state_.paused || state.info != state_.info ||
                         state.showSystemLevel != state_.showSystemLevel ||
                         state.showMicLevel != state_.showMicLevel ||
                         state.systemLevel != state_.systemLevel ||
                         state.micLevel != state_.micLevel;
    state_ = state;
    state_.systemLevel = displaySystemLevel_;
    state_.micLevel = displayMicLevel_;
    hasRenderedState_ = true;

    // 录制中红点要呼吸，每帧都画；暂停后画面静止，只在状态变化时重画。
    if (changed || !state_.paused) {
        Render();
        Present(255);
    }
}

RecordingHud::Action RecordingHud::PollAction() {
    const Action action = pending_;
    pending_ = Action::None;
    return action;
}

void RecordingHud::Render() {
    if (bits_ == nullptr) {
        return;
    }
    const float s = scale_;
    pauseRect_ = RECT{ padPx_ + static_cast<int>(std::lround(273.0f * s)),
                       padPx_ + static_cast<int>(std::lround(10.0f * s)),
                       padPx_ + static_cast<int>(std::lround(325.0f * s)),
                       padPx_ + static_cast<int>(std::lround(36.0f * s)) };
    stopRect_ = RECT{ padPx_ + static_cast<int>(std::lround(331.0f * s)),
                      padPx_ + static_cast<int>(std::lround(10.0f * s)),
                      padPx_ + static_cast<int>(std::lround(383.0f * s)),
                      padPx_ + static_cast<int>(std::lround(36.0f * s)) };

    Bitmap canvas(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                  static_cast<BYTE*>(bits_));
    Graphics g(&canvas);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAlias);
    g.Clear(Color(0, 0, 0, 0));

    const RectF card(static_cast<float>(padPx_), static_cast<float>(padPx_),
                     kHudCardWidth * s, kHudCardHeight * s);
    DrawSoftShadow(g, card, 13.0f * s, 0.35f, 8.5f * s, 3.0f * s, false);
    GraphicsPath cardPath;
    RoundRectPath(cardPath, card, 13.0f * s);
    SolidBrush background(Color(0xF2, 255, 255, 255));
    g.FillPath(&background, &cardPath);
    Pen border(Color(0x1F, 0, 0, 0), std::max(1.0f, s));
    g.DrawPath(&border, &cardPath);

    g.TranslateTransform(static_cast<float>(padPx_), static_cast<float>(padPx_));
    const float cardHeight = kHudCardHeight * s;

    StringFormat leftCenter;
    leftCenter.SetLineAlignment(StringAlignmentCenter);
    leftCenter.SetFormatFlags(StringFormatFlagsNoWrap);

    // 呼吸红点：1→0.35 720ms SineEaseInOut 往返 = 一条余弦；暂停时静止在 0.55。
    float dotOpacity = 0.55f;
    if (!state_.paused) {
        const ULONGLONG phase = (GetTickCount64() - pulseStart_) % 1440;
        dotOpacity =
            0.675f + 0.325f * std::cos(3.14159265358979f * static_cast<float>(phase) / 720.0f);
    }
    SolidBrush halo(Color(0x33, 0xE0, 0x31, 0x2B));
    g.FillEllipse(&halo, 13.0f * s, 15.5f * s, 15.0f * s, 15.0f * s);
    SolidBrush dot(Color(static_cast<BYTE>(255 * dotOpacity), 0xE0, 0x31, 0x2B));
    g.FillEllipse(&dot, 16.5f * s, 19.0f * s, 8.0f * s, 8.0f * s);

    const std::wstring stateText = state_.paused ? L"PAUSE" : L"REC";
    Font stateFont(L"Consolas", 9.5f * s, FontStyleBold, UnitPixel);
    SolidBrush stateBrush(state_.paused ? Color(255, 0xE0, 0x8A, 0x00)
                                        : Color(255, 0xE0, 0x31, 0x2B));
    g.DrawString(stateText.c_str(), static_cast<INT>(stateText.size()), &stateFont,
                 RectF(36.0f * s, 0.0f, 60.0f * s, cardHeight), &leftCenter, &stateBrush);
    SizeF stateSize;
    g.MeasureString(stateText.c_str(), static_cast<INT>(stateText.size()), &stateFont,
                    SizeF(200.0f * s, cardHeight), &leftCenter, &stateSize);

    const std::wstring time = FormatHudTime(state_.elapsedMs);
    Font timerFont(L"Consolas", 12.5f * s, FontStyleRegular, UnitPixel);
    SolidBrush timerBrush(Color(255, 0x1C, 0x1E, 0x24));
    const float timerX = 36.0f * s + stateSize.Width + 8.0f * s;
    g.DrawString(time.c_str(), static_cast<INT>(time.size()), &timerFont,
                 RectF(timerX, 0.0f, 90.0f * s, cardHeight), &leftCenter, &timerBrush);
    SizeF timerSize;
    g.MeasureString(time.c_str(), static_cast<INT>(time.size()), &timerFont,
                    SizeF(200.0f * s, cardHeight), &leftCenter, &timerSize);

    // 信息行 + 电平表：整块在卡片里垂直居中，宽度一直顶到暂停按钮左侧 10px。
    const float infoLeft = timerX + timerSize.Width + 10.0f * s;
    const float infoWidth = std::max(0.0f, 263.0f * s - infoLeft);
    Font infoFont(L"Microsoft YaHei UI", 10.0f * s, FontStyleRegular, UnitPixel);
    SolidBrush infoBrush(Color(255, 0x75, 0x7A, 0x88));
    const bool meters = state_.showSystemLevel || state_.showMicLevel;
    const float infoHeight = infoFont.GetHeight(&g);
    const float blockHeight = infoHeight + (meters ? 7.0f * s : 0.0f);
    const float infoY = (cardHeight - blockHeight) * 0.5f;
    StringFormat ellipsis;
    ellipsis.SetLineAlignment(StringAlignmentCenter);
    ellipsis.SetFormatFlags(StringFormatFlagsNoWrap);
    ellipsis.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(state_.info.c_str(), static_cast<INT>(state_.info.size()), &infoFont,
                 RectF(infoLeft, infoY, infoWidth, infoHeight), &ellipsis, &infoBrush);

    if (meters) {
        const float meterY = infoY + infoHeight + 4.0f * s;
        float x = infoLeft;
        const auto drawMeter = [&](float level, const Color& fill) {
            GraphicsPath track;
            RoundRectPath(track, RectF(x, meterY, 46.0f * s, 3.0f * s), 1.5f * s);
            SolidBrush trackBrush(Color(0x14, 0, 0, 0));
            g.FillPath(&trackBrush, &track);
            const float fillWidth = MeterWidthDip(level) * s;
            if (fillWidth >= 0.5f) {
                GraphicsPath bar;
                RoundRectPath(bar, RectF(x, meterY, fillWidth, 3.0f * s), 1.5f * s);
                SolidBrush barBrush(fill);
                g.FillPath(&barBrush, &bar);
            }
            x += 50.0f * s;
        };
        if (state_.showSystemLevel) {
            drawMeter(state_.systemLevel, Color(255, 0x18, 0xA0, 0x5B));
        }
        if (state_.showMicLevel) {
            drawMeter(state_.micLevel, Color(255, 0xE0, 0x8A, 0x00));
        }
    }

    // 暂停 / 停止按钮：hover、pressed、disabled 三态与 1.x 的样式触发器一一对应。
    Font labelFont(L"Microsoft YaHei UI", 11.0f * s, FontStyleRegular, UnitPixel);
    const float buttonAlpha = buttonsDisabled_ ? 0.4f : 1.0f;
    const auto drawButton = [&](float x, bool isStop, bool hovered, bool pressed) {
        const float y = 10.0f * s;
        const float w = 52.0f * s;
        const float h = 26.0f * s;
        Color base(0x0A, 0, 0, 0);
        if (hovered) {
            base = isStop ? Color(0x1F, 0xE0, 0x31, 0x2B) : Color(0x14, 0, 0, 0);
        }
        if (pressed) {
            base = isStop ? Color(0x14, 0xE0, 0x31, 0x2B) : Color(0x0A, 0, 0, 0);
        }
        base = Color(static_cast<BYTE>(base.GetAlpha() * buttonAlpha), base.GetRed(),
                     base.GetGreen(), base.GetBlue());
        GraphicsPath host;
        RoundRectPath(host, RectF(x, y, w, h), 7.0f * s);
        SolidBrush hostBrush(base);
        g.FillPath(&hostBrush, &host);

        const std::wstring label =
            isStop ? L"停止" : (state_.paused ? L"继续" : L"暂停");
        SizeF labelSize;
        g.MeasureString(label.c_str(), static_cast<INT>(label.size()), &labelFont,
                        SizeF(200.0f * s, h), &leftCenter, &labelSize);
        const float contentWidth = 14.0f * s + labelSize.Width;
        const float x0 = x + (w - contentWidth) * 0.5f;
        SolidBrush ink(Color(static_cast<BYTE>(255 * buttonAlpha), 0x2A, 0x2E, 0x37));
        if (isStop) {
            GraphicsPath square;
            RoundRectPath(square, RectF(x0, y + 9.0f * s, 8.0f * s, 8.0f * s), 1.5f * s);
            g.FillPath(&ink, &square);
        } else {
            g.FillRectangle(&ink, x0, y + 9.0f * s, 3.0f * s, 8.0f * s);
            g.FillRectangle(&ink, x0 + 5.0f * s, y + 9.0f * s, 3.0f * s, 8.0f * s);
        }
        g.DrawString(label.c_str(), static_cast<INT>(label.size()), &labelFont,
                     RectF(x0 + 14.0f * s, y, labelSize.Width + 2.0f * s, h), &leftCenter,
                     &ink);
    };
    drawButton(273.0f * s, false, hovered_ == 1, pressed_ == 1);
    drawButton(331.0f * s, true, hovered_ == 2, pressed_ == 2);
}

void RecordingHud::Present(BYTE alpha) {
    if (window_ != nullptr) {
        PresentLayered(window_, memory_, width_, height_, screenX_, screenY_, alpha);
    }
}

int RecordingHud::HitButton(POINT client) const {
    const auto inside = [](const RECT& rect, POINT p) {
        return p.x >= rect.left && p.x < rect.right && p.y >= rect.top && p.y < rect.bottom;
    };
    if (inside(pauseRect_, client)) {
        return 1;
    }
    if (inside(stopRect_, client)) {
        return 2;
    }
    return 0;
}

LRESULT CALLBACK RecordingHud::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<RecordingHud*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RecordingHud*>(create->lpCreateParams);
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

LRESULT RecordingHud::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = buttonsDisabled_ ? 0 : HitButton(point);
        if (hit != hovered_) {
            hovered_ = hit;
            Render();
            Present(255);
        }
        if (!trackingMouse_) {
            TRACKMOUSEEVENT leave{};
            leave.cbSize = sizeof(leave);
            leave.dwFlags = TME_LEAVE;
            leave.hwndTrack = window;
            TrackMouseEvent(&leave);
            trackingMouse_ = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        if (hovered_ != 0) {
            hovered_ = 0;
            Render();
            Present(255);
        }
        return 0;
    case WM_SETCURSOR: {
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        if (!buttonsDisabled_ && HitButton(point) != 0) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return 1;
        }
        break;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = buttonsDisabled_ ? 0 : HitButton(point);
        if (hit != 0) {
            pressed_ = hit;
            SetCapture(window);
            Render();
            Present(255);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        // 先取走按下态再释放捕获：ReleaseCapture 会同步发 WM_CAPTURECHANGED，
        // 那里的「取消按下」会把 pressed_ 清掉，顺序反了点击就永远读不到。
        const int pressed = pressed_;
        pressed_ = 0;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = HitButton(point);
        if (pressed != 0 && !buttonsDisabled_ && hit == pressed) {
            pending_ = pressed == 1 ? Action::Pause : Action::Stop;
            if (pressed == 2) {
                // 1.x：点停止之后两个按钮一起禁用，避免收尾期间再点出第二次动作。
                buttonsDisabled_ = true;
                hovered_ = 0;
            }
        }
        Render();
        Present(255);
        return 0;
    }
    case WM_CAPTURECHANGED:
        if (pressed_ != 0) {
            pressed_ = 0;
            Render();
            Present(255);
        }
        return 0;
    case WM_DESTROY:
        window_ = nullptr;
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}