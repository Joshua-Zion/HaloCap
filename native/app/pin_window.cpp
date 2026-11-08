#include "pin_window.h"

#include "image_export.h"

#include <windows.h>

#include <windowsx.h>
#include <objbase.h>
#include <gdiplus.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapPinWindow";
constexpr int kBorderPx = 1;
constexpr int kGlowPx = 12;
constexpr int kRadius = 12;

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

std::wstring TimestampName() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t buffer[64] = {};
    std::swprintf(buffer, 64, L"HaloCap_%04d-%02d-%02d_%02d-%02d-%02d",
                  static_cast<int>(now.wYear), static_cast<int>(now.wMonth),
                  static_cast<int>(now.wDay), static_cast<int>(now.wHour),
                  static_cast<int>(now.wMinute), static_cast<int>(now.wSecond));
    return buffer;
}

std::wstring SaveDirectory() {
    wchar_t pictures[MAX_PATH] = {};
    if (SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures) != S_OK) {
        return {};
    }
    return std::wstring(pictures) + L"\\HaloCap";
}

}  // namespace

PinWindow::PinWindow() = default;

PinWindow::~PinWindow() { Close(); }

bool PinWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = CS_DBLCLKS;
        description.lpfnWndProc = &PinWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool PinWindow::Create(const std::uint8_t* bgra, int width, int height, int screenX, int screenY) {
    if (bgra == nullptr || width <= 0 || height <= 0 || !EnsureClass()) {
        return false;
    }
    EnsureGdiplus();

    content_.assign(bgra, bgra + static_cast<std::size_t>(width) * height * 4);
    contentWidth_ = width;
    contentHeight_ = height;

    const int outerWidth = width + 2 * (kBorderPx + kGlowPx);
    const int outerHeight = height + 2 * (kBorderPx + kGlowPx);

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    window_ = CreateWindowExW(exStyle, kClassName, L"HaloCap pin", WS_POPUP,
                              screenX - (kGlowPx + kBorderPx),
                              screenY - (kGlowPx + kBorderPx), outerWidth, outerHeight, nullptr,
                              nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    ShowWindow(static_cast<HWND>(window_), SW_SHOWNOACTIVATE);
    Render();
    return true;
}

void PinWindow::Render() {
    if (window_ == nullptr || content_.empty()) {
        return;
    }

    const int width = std::max(1, static_cast<int>(std::lround(contentWidth_ * zoom_)));
    const int height = std::max(1, static_cast<int>(std::lround(contentHeight_ * zoom_)));
    const int outerWidth = width + 2 * (kBorderPx + kGlowPx);
    const int outerHeight = height + 2 * (kBorderPx + kGlowPx);
    const int contentLeft = kGlowPx + kBorderPx;
    const int contentTop = kGlowPx + kBorderPx;

    std::vector<std::uint8_t> layered(static_cast<std::size_t>(outerWidth) * outerHeight * 4, 0);
    const auto roundedDistance = [](float px, float py, float halfWidth, float halfHeight,
                                    float radius) {
        const float qx = std::fabs(px) - halfWidth + radius;
        const float qy = std::fabs(py) - halfHeight + radius;
        const float outsideX = std::max(qx, 0.0f);
        const float outsideY = std::max(qy, 0.0f);
        return std::sqrt(outsideX * outsideX + outsideY * outsideY) +
               std::min(std::max(qx, qy), 0.0f) - radius;
    };

    const float centerX = static_cast<float>(width) * 0.5f;
    const float centerY = static_cast<float>(height) * 0.5f;

    for (int y = 0; y < outerHeight; ++y) {
        for (int x = 0; x < outerWidth; ++x) {
            const int contentX = x - contentLeft;
            const int contentY = y - contentTop;
            const float px = static_cast<float>(contentX);
            const float py = static_cast<float>(contentY);
            const float contentDistance =
                roundedDistance(px - centerX, py - centerY, centerX, centerY, kRadius);
            const float outerDistance = roundedDistance(
                px - centerX, py - centerY, centerX + kBorderPx, centerY + kBorderPx,
                kRadius + kBorderPx);

            // 圆角判定的等号会把右/下边界也算进来，而像素索引只到 width-1/height-1；
            // 之前这里正好越界一行，触发 vector subscript out of range。
            const bool insideContent = contentDistance < 0.0f && contentX >= 0 &&
                                       contentX < width && contentY >= 0 && contentY < height;

            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            std::uint8_t a = 0;

            if (insideContent) {
                const int sampleX = std::clamp(
                    static_cast<int>(static_cast<float>(contentX) / zoom_), 0, contentWidth_ - 1);
                const int sampleY = std::clamp(
                    static_cast<int>(static_cast<float>(contentY) / zoom_), 0, contentHeight_ - 1);
                const std::size_t source =
                    (static_cast<std::size_t>(sampleY) * contentWidth_ + sampleX) * 4;
                b = content_[source + 0];
                g = content_[source + 1];
                r = content_[source + 2];
                a = 255;
            } else if (outerDistance <= 0.0f) {
                b = 255;
                g = 255;
                r = 255;
                a = 255;
            } else if (outerDistance < static_cast<float>(kGlowPx)) {
                const float t = 1.0f - outerDistance / static_cast<float>(kGlowPx);
                a = static_cast<std::uint8_t>(72.0f * t * t);
            }

            const std::size_t destination = (static_cast<std::size_t>(y) * outerWidth + x) * 4;
            layered[destination + 0] = static_cast<std::uint8_t>(b * a / 255);
            layered[destination + 1] = static_cast<std::uint8_t>(g * a / 255);
            layered[destination + 2] = static_cast<std::uint8_t>(r * a / 255);
            layered[destination + 3] = a;
        }
    }

    // 悬停工具条：右上角三个圆形按钮。
    copyRect_ = RECT{ width - 136, 8, width - 108, 36 };
    saveRect_ = RECT{ width - 104, 8, width - 76, 36 };
    ocrRect_ = RECT{ width - 72, 8, width - 44, 36 };
    closeRect_ = RECT{ width - 40, 8, width - 12, 36 };
    if (hover_) {
        Gdiplus::Bitmap canvas(outerWidth, outerHeight, outerWidth * 4, PixelFormat32bppPARGB,
                               layered.data());
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        const float barLeft = static_cast<float>(contentLeft + width - 140);
        const float barTop = static_cast<float>(contentTop + 4);
        Gdiplus::GraphicsPath bar;
        bar.AddArc(barLeft, barTop, 32.0f, 32.0f, 90.0f, 180.0f);
        bar.AddArc(barLeft + 104.0f, barTop, 32.0f, 32.0f, 270.0f, 180.0f);
        bar.CloseFigure();
        Gdiplus::SolidBrush barBrush(Gdiplus::Color(235, 0x16, 0x1A, 0x22));
        Gdiplus::Pen barPen(Gdiplus::Color(40, 255, 255, 255), 1.0f);
        graphics.FillPath(&barBrush, &bar);
        graphics.DrawPath(&barPen, &bar);

        const auto buttonCenter = [&](const RECT& rect) {
            return Gdiplus::PointF(static_cast<float>(contentLeft + rect.left + 14),
                                   static_cast<float>(contentTop + rect.top + 14));
        };
        const auto drawCopy = [&](Gdiplus::PointF center) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0xE9, 0xED, 0xF4), 1.6f);
            graphics.DrawRectangle(&pen, center.X - 7.0f, center.Y - 8.0f, 10.0f, 12.0f);
            graphics.DrawRectangle(&pen, center.X - 2.0f, center.Y - 4.0f, 10.0f, 12.0f);
        };
        const auto drawSave = [&](Gdiplus::PointF center) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0xE9, 0xED, 0xF4), 1.6f);
            graphics.DrawLine(&pen, center.X, center.Y - 9.0f, center.X, center.Y + 3.0f);
            graphics.DrawLine(&pen, center.X - 4.0f, center.Y - 1.0f, center.X, center.Y + 3.0f);
            graphics.DrawLine(&pen, center.X + 4.0f, center.Y - 1.0f, center.X, center.Y + 3.0f);
            graphics.DrawLine(&pen, center.X - 8.0f, center.Y + 8.0f, center.X + 8.0f,
                              center.Y + 8.0f);
        };
        const auto drawClose = [&](Gdiplus::PointF center) {
            Gdiplus::Pen pen(Gdiplus::Color(255, 0xE9, 0xED, 0xF4), 1.6f);
            graphics.DrawLine(&pen, center.X - 6.0f, center.Y - 6.0f, center.X + 6.0f,
                              center.Y + 6.0f);
            graphics.DrawLine(&pen, center.X + 6.0f, center.Y - 6.0f, center.X - 6.0f,
                              center.Y + 6.0f);
        };
        const auto drawOcr = [&](Gdiplus::PointF center) {
            Gdiplus::Font font(L"Microsoft YaHei UI", 9.0f, Gdiplus::FontStyleBold,
                               Gdiplus::UnitPixel);
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            Gdiplus::SolidBrush brush(Gdiplus::Color(255, 0xE9, 0xED, 0xF4));
            graphics.DrawString(L"OCR", 3, &font,
                                Gdiplus::RectF(center.X - 14.0f, center.Y - 8.0f, 28.0f, 16.0f),
                                &format, &brush);
        };
        drawCopy(buttonCenter(copyRect_));
        drawSave(buttonCenter(saveRect_));
        drawOcr(buttonCenter(ocrRect_));
        drawClose(buttonCenter(closeRect_));
    }

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = outerWidth;
    info.bmiHeader.biHeight = -outerHeight;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr) {
        std::memcpy(bits, layered.data(), layered.size());
        HGDIOBJ old = SelectObject(memory, dib);
        RECT windowRect{};
        GetWindowRect(static_cast<HWND>(window_), &windowRect);
        POINT destination{ windowRect.left, windowRect.top };
        SIZE size{ outerWidth, outerHeight };
        POINT source{ 0, 0 };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = static_cast<BYTE>(std::clamp(opacity_, 0.15f, 1.0f) * 255.0f);
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(static_cast<HWND>(window_), screen, &destination, &size, memory,
                            &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

int PinWindow::ButtonAt(POINT client) const {
    const int x = client.x - (kGlowPx + kBorderPx);
    const int y = client.y - (kGlowPx + kBorderPx);
    const auto inside = [&](const RECT& rect) {
        return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
    };
    if (inside(copyRect_)) {
        return 0;
    }
    if (inside(saveRect_)) {
        return 1;
    }
    if (inside(ocrRect_)) {
        return 2;
    }
    if (inside(closeRect_)) {
        return 3;
    }
    return -1;
}

void PinWindow::Close() {
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
}

LRESULT CALLBACK PinWindow::WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<PinWindow*>(create->lpCreateParams);
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

LRESULT PinWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        if (!trackingLeave_) {
            TRACKMOUSEEVENT track{};
            track.cbSize = sizeof(track);
            track.dwFlags = TME_LEAVE;
            track.hwndTrack = window;
            TrackMouseEvent(&track);
            trackingLeave_ = true;
        }
        const int displayWidth = static_cast<int>(std::lround(contentWidth_ * zoom_));
        const int displayHeight = static_cast<int>(std::lround(contentHeight_ * zoom_));
        const bool hover = ButtonAt(point) >= 0 ||
                           (point.x >= kGlowPx + kBorderPx &&
                            point.x < kGlowPx + kBorderPx + displayWidth &&
                            point.y >= kGlowPx + kBorderPx &&
                            point.y < kGlowPx + kBorderPx + displayHeight);
        if (hover != hover_) {
            hover_ = hover;
            Render();
        }
        if (dragging_) {
            POINT cursor{};
            GetCursorPos(&cursor);
            const int x = dragWindow_.x + (cursor.x - dragOrigin_.x);
            const int y = dragWindow_.y + (cursor.y - dragOrigin_.y);
            SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        if (hover_) {
            hover_ = false;
            Render();
        }
        return 0;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            opacity_ = std::clamp(opacity_ + (delta > 0 ? 0.08f : -0.08f), 0.15f, 1.0f);
        } else {
            zoom_ = std::clamp(zoom_ * (delta > 0 ? 1.1f : 0.9f), 0.12f, 8.0f);
            const int newWidth = std::max(1, static_cast<int>(std::lround(contentWidth_ * zoom_)));
            const int newHeight = std::max(1, static_cast<int>(std::lround(contentHeight_ * zoom_)));
            SetWindowPos(window, nullptr, 0, 0,
                         newWidth + 2 * (kBorderPx + kGlowPx),
                         newHeight + 2 * (kBorderPx + kGlowPx),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        Render();
        return 0;
    }

    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int button = ButtonAt(point);
        if (button == 3) {
            Close();
            return 0;
        }
        if (button == 2) {
            if (onOcr_) {
                onOcr_(content_.data(), contentWidth_, contentHeight_);
            }
            return 0;
        }
        if (button == 0) {
            halocap::platform::win32::CopyBgraToClipboard(window, content_.data(), contentWidth_,
                                                          contentHeight_, contentWidth_ * 4);
            return 0;
        }
        if (button == 1) {
            const std::wstring directory = SaveDirectory();
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::path(directory), error);
            const std::wstring path = directory + L"\\" + TimestampName() + L".png";
            halocap::platform::win32::SaveBgraPng(path, content_.data(), contentWidth_,
                                                  contentHeight_, contentWidth_ * 4);
            return 0;
        }

        dragging_ = true;
        GetCursorPos(&dragOrigin_);
        RECT rect{};
        GetWindowRect(window, &rect);
        dragWindow_ = POINT{ rect.left, rect.top };
        SetCapture(window);
        return 0;
    }

    case WM_LBUTTONUP:
        if (dragging_) {
            dragging_ = false;
            ReleaseCapture();
        }
        return 0;

    case WM_LBUTTONDBLCLK:
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
