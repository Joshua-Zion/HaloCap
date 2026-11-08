#include "radial_menu.h"

#include <windows.h>
#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapRadialMenu";
constexpr int kRingRadius = 96;      // 四个按钮圆心到菜单中心
constexpr int kButtonRadius = 34;
constexpr int kMenuRadius = 150;
constexpr UINT kMsgEscape = WM_APP + 1;

Gdiplus::Font* g_labelFont = nullptr;
Gdiplus::Font* g_centerFont = nullptr;
Gdiplus::Font* g_hintFont = nullptr;

RadialMenu* g_active = nullptr;

void EnsureFonts() {
    static ULONG_PTR gdiplusToken = 0;
    if (gdiplusToken == 0) {
        Gdiplus::GdiplusStartupInput input;
        Gdiplus::GdiplusStartup(&gdiplusToken, &input, nullptr);
    }
    if (g_labelFont == nullptr) {
        g_labelFont = new Gdiplus::Font(L"Microsoft YaHei UI", 12.0f, Gdiplus::FontStyleRegular,
                                        Gdiplus::UnitPixel);
        g_centerFont = new Gdiplus::Font(L"Microsoft YaHei UI", 13.0f, Gdiplus::FontStyleBold,
                                         Gdiplus::UnitPixel);
        g_hintFont = new Gdiplus::Font(L"Microsoft YaHei UI", 10.0f, Gdiplus::FontStyleRegular,
                                       Gdiplus::UnitPixel);
    }
}

const wchar_t* ActionLabel(core::AppAction action) {
    switch (action) {
    case core::AppAction::Screenshot: return L"截图";
    case core::AppAction::Ocr: return L"OCR";
    case core::AppAction::Record: return L"录制";
    case core::AppAction::Pin: return L"贴图";
    default: return L"";
    }
}

void DrawScreenshotIcon(Gdiplus::Graphics& g, float cx, float cy, const Gdiplus::Color& color) {
    Gdiplus::Pen pen(color, 1.8f);
    g.DrawRectangle(&pen, cx - 10.0f, cy - 6.0f, 20.0f, 13.0f);
    g.DrawEllipse(&pen, cx - 4.0f, cy - 2.5f, 8.0f, 8.0f);
    Gdiplus::SolidBrush bumpBrush(color);
    g.FillRectangle(&bumpBrush, cx - 5.0f, cy - 9.0f, 7.0f, 3.0f);
}

void DrawOcrIcon(Gdiplus::Graphics& g, float cx, float cy, const Gdiplus::Color& color) {
    Gdiplus::Pen pen(color, 1.8f);
    g.DrawRectangle(&pen, cx - 9.0f, cy - 8.0f, 18.0f, 16.0f);
    g.DrawLine(&pen, cx - 5.0f, cy - 3.0f, cx + 5.0f, cy - 3.0f);
    g.DrawLine(&pen, cx - 5.0f, cy + 1.0f, cx + 5.0f, cy + 1.0f);
    g.DrawLine(&pen, cx - 5.0f, cy + 5.0f, cx + 1.0f, cy + 5.0f);
}

void DrawRecordIcon(Gdiplus::Graphics& g, float cx, float cy, const Gdiplus::Color& color) {
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 107, 107));
    g.FillEllipse(&brush, cx - 8.0f, cy - 8.0f, 16.0f, 16.0f);
    (void)color;
}

void DrawPinIcon(Gdiplus::Graphics& g, float cx, float cy, const Gdiplus::Color& color) {
    Gdiplus::SolidBrush brush(color);
    g.FillEllipse(&brush, cx - 7.0f, cy - 9.0f, 14.0f, 9.0f);
    Gdiplus::Pen pen(color, 1.8f);
    g.DrawLine(&pen, cx, cy - 1.0f, cx, cy + 7.0f);
    g.DrawLine(&pen, cx, cy + 7.0f, cx + 5.0f, cy + 10.0f);
}

void DrawIcon(Gdiplus::Graphics& g, core::AppAction action, float cx, float cy,
              const Gdiplus::Color& color) {
    switch (action) {
    case core::AppAction::Screenshot: DrawScreenshotIcon(g, cx, cy, color); break;
    case core::AppAction::Ocr: DrawOcrIcon(g, cx, cy, color); break;
    case core::AppAction::Record: DrawRecordIcon(g, cx, cy, color); break;
    case core::AppAction::Pin: DrawPinIcon(g, cx, cy, color); break;
    default: break;
    }
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

}  // namespace

RadialMenu::RadialMenu() = default;

RadialMenu::~RadialMenu() { Close(); }

bool RadialMenu::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &RadialMenu::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_HAND);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool RadialMenu::Open(SelectHandler onSelect) {
    Close();
    EnsureFonts();
    if (!EnsureClass()) {
        return false;
    }
    onSelect_ = std::move(onSelect);

    width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    const int originX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int originY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    if (width_ < 200 || height_ < 200) {
        width_ = GetSystemMetrics(SM_CXSCREEN);
        height_ = GetSystemMetrics(SM_CYSCREEN);
    }

    POINT cursor{};
    GetCursorPos(&cursor);
    center_ = POINT{ std::clamp(static_cast<int>(cursor.x) - originX, kMenuRadius + 8,
                                width_ - kMenuRadius - 8),
                     std::clamp(static_cast<int>(cursor.y) - originY, kMenuRadius + 8,
                                height_ - kMenuRadius - 8) };

    HDC screen = GetDC(nullptr);
    memory_ = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_;
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
    std::memset(bits_, 0, static_cast<std::size_t>(width_) * height_ * 4);

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClassName, L"HaloCap radial", WS_POPUP, originX, originY, width_,
                              height_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        Close();
        return false;
    }

    Render();
    ShowWindow(static_cast<HWND>(window_), SW_SHOWNOACTIVATE);

    g_active = this;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &RadialMenu::KeyHook, GetModuleHandleW(nullptr), 0);
    return true;
}

void RadialMenu::Close() {
    if (hook_ != nullptr) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (g_active == this) {
        g_active = nullptr;
    }
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
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
    hovered_ = -1;
}

LRESULT CALLBACK RadialMenu::KeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_active == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
        reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode == VK_ESCAPE) {
        PostMessageW(static_cast<HWND>(g_active->window_), kMsgEscape, 0, 0);
        return 1;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

LRESULT CALLBACK RadialMenu::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<RadialMenu*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<RadialMenu*>(create->lpCreateParams);
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

int RadialMenu::HitTest(POINT client) const {
    const float dx = static_cast<float>(client.x - center_.x);
    const float dy = static_cast<float>(client.y - center_.y);
    const float distance = std::sqrt(dx * dx + dy * dy);
    if (distance < kRingRadius - kButtonRadius - 4.0f ||
        distance > kRingRadius + kButtonRadius + 4.0f) {
        return -1;
    }

    // 12 点开始顺时针：截图 / OCR / 录制 / 贴图。
    const float angle = std::atan2(dy, dx);
    const float top = -1.5707963f;
    int best = -1;
    float bestDelta = 0.7854f;   // 45°
    for (int i = 0; i < 4; ++i) {
        float delta = std::fabs(angle - (top + i * 1.5707963f));
        if (delta > 3.1415926f) {
            delta = 6.2831853f - delta;
        }
        if (delta < bestDelta) {
            bestDelta = delta;
            best = i;
        }
    }
    return best;
}

void RadialMenu::Render() {
    if (bits_ == nullptr) {
        return;
    }

    Gdiplus::Bitmap canvas(width_, height_, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics g(&canvas);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));

        // 全屏压暗一点，让焦点落在圆盘上。
        Gdiplus::SolidBrush dimBrush(Gdiplus::Color(46, 0, 0, 0));
        g.FillRectangle(&dimBrush, 0, 0, width_, height_);

        const float cx = static_cast<float>(center_.x);
        const float cy = static_cast<float>(center_.y);
        Gdiplus::SolidBrush background(Gdiplus::Color(230, 0x16, 0x1A, 0x22));
        g.FillEllipse(&background, cx - kMenuRadius, cy - kMenuRadius, kMenuRadius * 2.0f,
                      kMenuRadius * 2.0f);
        Gdiplus::Pen ringPen(Gdiplus::Color(40, 255, 255, 255), 1.0f);
        g.DrawEllipse(&ringPen, cx - kMenuRadius, cy - kMenuRadius, kMenuRadius * 2.0f,
                      kMenuRadius * 2.0f);

        const core::AppAction actions[] = { core::AppAction::Screenshot, core::AppAction::Ocr,
                                            core::AppAction::Record, core::AppAction::Pin };
        const float top = -1.5707963f;
        for (int i = 0; i < 4; ++i) {
            const float angle = top + i * 1.5707963f;
            const float bx = cx + std::cos(angle) * kRingRadius;
            const float by = cy + std::sin(angle) * kRingRadius;
            const bool hovered = hovered_ == i;
            Gdiplus::SolidBrush buttonBrush(
                hovered ? Gdiplus::Color(255, 0x2F, 0x7C, 0xF6)
                        : Gdiplus::Color(255, 0x22, 0x28, 0x33));
            g.FillEllipse(&buttonBrush, bx - kButtonRadius, by - kButtonRadius,
                          kButtonRadius * 2.0f, kButtonRadius * 2.0f);
            Gdiplus::Pen buttonPen(Gdiplus::Color(hovered ? 120 : 30, 255, 255, 255), 1.0f);
            g.DrawEllipse(&buttonPen, bx - kButtonRadius, by - kButtonRadius,
                          kButtonRadius * 2.0f, kButtonRadius * 2.0f);
            DrawIcon(g, actions[i], bx, by - 6.0f,
                     Gdiplus::Color(255, hovered ? 255 : 0xE9, hovered ? 255 : 0xED,
                                    hovered ? 255 : 0xF4));

            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const std::wstring label = ActionLabel(actions[i]);
            Gdiplus::RectF labelRect(bx - 40.0f, by + 16.0f, 80.0f, 22.0f);
            Gdiplus::SolidBrush labelBrush(Gdiplus::Color(
                255, hovered ? 255 : 0xE9, hovered ? 255 : 0xED, hovered ? 255 : 0xF4));
            g.DrawString(label.c_str(), static_cast<int>(label.size()), g_labelFont, labelRect,
                         &format, &labelBrush);
        }

        Gdiplus::StringFormat centerFormat;
        centerFormat.SetAlignment(Gdiplus::StringAlignmentCenter);
        centerFormat.SetLineAlignment(Gdiplus::StringAlignmentCenter);
        Gdiplus::SolidBrush centerBrush(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
        g.DrawString(L"HaloCap", 7, g_centerFont,
                     Gdiplus::RectF(cx - 60.0f, cy - 26.0f, 120.0f, 22.0f), &centerFormat,
                     &centerBrush);
        Gdiplus::SolidBrush hintBrush(Gdiplus::Color(255, 0x9A, 0xA5, 0xB5));
        g.DrawString(L"Esc 关闭", 6, g_hintFont,
                     Gdiplus::RectF(cx - 60.0f, cy + 2.0f, 120.0f, 18.0f), &centerFormat,
                     &hintBrush);
    }

    Gdiplus::Rect lockRect(0, 0, width_, height_);
    Gdiplus::BitmapData data{};
    if (canvas.LockBits(&lockRect, Gdiplus::ImageLockModeRead, PixelFormat32bppPARGB, &data) ==
        Gdiplus::Ok) {
        for (int y = 0; y < height_; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(bits_) + static_cast<std::size_t>(y) * width_ * 4,
                        static_cast<const std::uint8_t*>(data.Scan0) +
                            static_cast<std::size_t>(y) * data.Stride,
                        static_cast<std::size_t>(width_) * 4);
        }
        canvas.UnlockBits(&data);
    }

    HDC screen = GetDC(nullptr);
    POINT destination{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
    SIZE size{ width_, height_ };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(static_cast<HWND>(window_), screen, &destination, &size, memory_, &source, 0,
                        &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT RadialMenu::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hovered = HitTest(point);
        if (hovered != hovered_) {
            hovered_ = hovered;
            Render();
        }
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = HitTest(point);
        if (hit >= 0) {
            const core::AppAction actions[] = { core::AppAction::Screenshot, core::AppAction::Ocr,
                                                core::AppAction::Record, core::AppAction::Pin };
            const SelectHandler handler = onSelect_;
            const core::AppAction action = actions[hit];
            Close();
            if (handler) {
                handler(action);
            }
        } else {
            Close();
        }
        return 0;
    }

    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        Close();
        return 0;

    case kMsgEscape:
        Close();
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
