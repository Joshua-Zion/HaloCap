#include "tray_menu_window.h"

#include "soft_shadow.h"

#include <windowsx.h>

#include <objbase.h>
#include <gdiplus.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace halocap::app {

using namespace Gdiplus;

namespace {

constexpr wchar_t kMenuClass[] = L"HaloCapTrayMenu";

// 面板与阴影留白（DIP）：面板 252 宽、圆角 12，与 1.x TrayMenu.xaml 同值。
constexpr float kPanelWidth = 252.0f;
constexpr float kMenuPad = 16.0f;
constexpr float kHeaderHeight = 44.0f;   // 12 + 22 + 10
constexpr float kItemHeight = 29.0f;     // 内容 17 + 上下 padding 6
constexpr float kSeparatorHeight = 9.0f; // 1 + 上下 margin 4
constexpr float kFooterHeight = 28.0f;   // 1 hairline + 7 + 13 + 8

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

// FontFamily 不可拷贝，这里只挑一个字体名交给 Font(const WCHAR*, ...) 重载。
const wchar_t* UiFamilyName() {
    static const wchar_t* name = [] {
        FontFamily variable(L"Segoe UI Variable Text");
        if (variable.IsAvailable()) {
            return L"Segoe UI Variable Text";
        }
        return L"Microsoft YaHei UI";
    }();
    return name;
}

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

Color WithAlpha(const Color& color, float alpha) {
    return Color(static_cast<BYTE>(color.GetAlpha() * std::clamp(alpha, 0.0f, 1.0f)),
                 color.GetRed(), color.GetGreen(), color.GetBlue());
}

// Pen 不可拷贝：就地配置到调用方构造好的笔上。
void ApplyRoundCaps(Pen& pen) {
    pen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
    pen.SetLineJoin(LineJoinRound);
}

// 图标与 1.x CreateIcon 同源：24×24 设计空间里的 PathGeometry，Viewbox 16×16 等比居中。
// WPF 的 Viewbox 量的是「几何 + 描边」的包围盒，所以这里每边补 0.75（描边 1.5 的一半）。
void DrawTrayIcon(Graphics& g, core::TrayCommand command, bool recording, bool paused, float x,
                  float y, float size, float alpha) {
    float bx = 0.0f;
    float by = 0.0f;
    float bw = 0.0f;
    float bh = 0.0f;
    Color color(255, 0x6E, 0x6E, 0x73);
    bool filled = false;

    switch (command) {
    case core::TrayCommand::Screenshot:
        bx = 3; by = 3; bw = 18; bh = 18;
        color = Color(255, 0x6C, 0xA8, 0xFF);
        break;
    case core::TrayCommand::Ocr:
        bx = 4; by = 4; bw = 16; bh = 16;
        color = Color(255, 0xB0, 0x8C, 0xFF);
        break;
    case core::TrayCommand::Record:
        if (recording) {
            bx = 5.5f; by = 5.5f; bw = 13; bh = 13;
            color = Color(255, 0xE5, 0x48, 0x4D);
            filled = true;
        } else {
            bx = 2; by = 6; bw = 19; bh = 12;
            color = Color(255, 0xFF, 0x73, 0x73);
        }
        break;
    case core::TrayCommand::PauseRecording:
        if (paused) {
            bx = 8.5f; by = 5.5f; bw = 9.5f; bh = 13;
        } else {
            bx = 8; by = 5.5f; bw = 8; bh = 13;
        }
        color = Color(255, 0xE5, 0x48, 0x4D);
        break;
    case core::TrayCommand::Pin:
        bx = 6; by = 4; bw = 12; bh = 16;
        color = Color(255, 0xF5, 0xB9, 0x63);
        break;
    case core::TrayCommand::OpenRadial:
        bx = 4.5f; by = 4.5f; bw = 15; bh = 15;
        break;
    case core::TrayCommand::Settings:
        bx = 3; by = 5; bw = 18; bh = 14;
        break;
    case core::TrayCommand::ToggleAutoStart:
        bx = 4.5f; by = 3.5f; bw = 15; bh = 16.363f;
        break;
    case core::TrayCommand::Exit:
        bx = 3; by = 4; bw = 16; bh = 16;
        break;
    default:
        return;
    }

    const float effW = bw + 1.5f;
    const float effH = bh + 1.5f;
    const float fit = size / std::max(effW, effH);
    const float tx = x + (size - effW * fit) * 0.5f - (bx - 0.75f) * fit;
    const float ty = y + (size - effH * fit) * 0.5f - (by - 0.75f) * fit;

    const GraphicsState state = g.Save();
    g.TranslateTransform(tx, ty);
    g.ScaleTransform(fit, fit);

    color = WithAlpha(color, alpha);
    Pen pen(color, 1.5f);
    ApplyRoundCaps(pen);
    SolidBrush brush(color);

    switch (command) {
    case core::TrayCommand::Screenshot: {
        GraphicsPath path;
        path.AddLine(3.0f, 7.0f, 3.0f, 5.0f);
        path.AddArc(3.0f, 3.0f, 4.0f, 4.0f, 180.0f, 90.0f);
        path.AddLine(5.0f, 3.0f, 7.0f, 3.0f);
        path.StartFigure();
        path.AddLine(17.0f, 3.0f, 19.0f, 3.0f);
        path.AddArc(17.0f, 3.0f, 4.0f, 4.0f, 270.0f, 90.0f);
        path.AddLine(21.0f, 5.0f, 21.0f, 7.0f);
        path.StartFigure();
        path.AddLine(21.0f, 17.0f, 21.0f, 19.0f);
        path.AddArc(17.0f, 17.0f, 4.0f, 4.0f, 0.0f, 90.0f);
        path.AddLine(19.0f, 21.0f, 17.0f, 21.0f);
        path.StartFigure();
        path.AddLine(7.0f, 21.0f, 5.0f, 21.0f);
        path.AddArc(3.0f, 17.0f, 4.0f, 4.0f, 90.0f, 90.0f);
        path.AddLine(3.0f, 19.0f, 3.0f, 17.0f);
        g.DrawPath(&pen, &path);
        g.FillEllipse(&brush, 11.25f, 11.25f, 1.5f, 1.5f);
        break;
    }
    case core::TrayCommand::Ocr: {
        GraphicsPath path;
        path.AddLine(4.0f, 7.0f, 4.0f, 4.0f);
        path.AddLine(4.0f, 4.0f, 20.0f, 4.0f);
        path.AddLine(20.0f, 4.0f, 20.0f, 7.0f);
        path.StartFigure();
        path.AddLine(9.0f, 20.0f, 15.0f, 20.0f);
        path.StartFigure();
        path.AddLine(12.0f, 4.0f, 12.0f, 20.0f);
        g.DrawPath(&pen, &path);
        break;
    }
    case core::TrayCommand::Record: {
        if (filled) {
            g.FillEllipse(&brush, 5.5f, 5.5f, 13.0f, 13.0f);
            g.DrawEllipse(&pen, 5.5f, 5.5f, 13.0f, 13.0f);
        } else {
            GraphicsPath body;
            RoundRectPath(body, RectF(2.0f, 6.0f, 15.0f, 12.0f), 2.0f);
            g.DrawPath(&pen, &body);
            const PointF lens[4] = { PointF(17.0f, 10.5f), PointF(21.0f, 8.5f),
                                     PointF(21.0f, 15.5f), PointF(17.0f, 13.5f) };
            g.DrawPolygon(&pen, lens, 4);
        }
        break;
    }
    case core::TrayCommand::PauseRecording: {
        if (paused) {
            const PointF triangle[3] = { PointF(8.5f, 5.5f), PointF(18.0f, 12.0f),
                                         PointF(8.5f, 18.5f) };
            g.DrawPolygon(&pen, triangle, 3);
        } else {
            GraphicsPath left;
            left.AddRectangle(RectF(8.0f, 5.5f, 2.5f, 13.0f));
            GraphicsPath right;
            right.AddRectangle(RectF(13.5f, 5.5f, 2.5f, 13.0f));
            g.DrawPath(&pen, &left);
            g.DrawPath(&pen, &right);
        }
        break;
    }
    case core::TrayCommand::Pin: {
        const PointF head[8] = { PointF(9.0f, 4.0f),  PointF(15.0f, 4.0f),
                                 PointF(15.0f, 9.0f), PointF(18.0f, 12.0f),
                                 PointF(18.0f, 14.0f), PointF(6.0f, 14.0f),
                                 PointF(6.0f, 12.0f), PointF(9.0f, 9.0f) };
        g.DrawPolygon(&pen, head, 8);
        g.DrawLine(&pen, 12.0f, 14.0f, 12.0f, 20.0f);
        break;
    }
    case core::TrayCommand::OpenRadial:
        g.DrawEllipse(&pen, 4.5f, 4.5f, 15.0f, 15.0f);
        g.DrawEllipse(&pen, 9.5f, 9.5f, 5.0f, 5.0f);
        break;
    case core::TrayCommand::Settings: {
        GraphicsPath path;
        path.AddLine(3.0f, 8.0f, 10.0f, 8.0f);
        path.StartFigure();
        path.AddLine(14.0f, 8.0f, 21.0f, 8.0f);
        path.StartFigure();
        path.AddLine(12.0f, 5.0f, 12.0f, 11.0f);
        path.StartFigure();
        path.AddLine(3.0f, 16.0f, 7.0f, 16.0f);
        path.StartFigure();
        path.AddLine(11.0f, 16.0f, 21.0f, 16.0f);
        path.StartFigure();
        path.AddLine(9.0f, 13.0f, 9.0f, 19.0f);
        g.DrawPath(&pen, &path);
        break;
    }
    case core::TrayCommand::ToggleAutoStart: {
        g.DrawLine(&pen, 12.0f, 3.5f, 12.0f, 11.0f);
        GraphicsPath arc;
        arc.AddArc(4.5f, 4.863f, 15.0f, 15.0f, 230.2f, -280.4f);
        g.DrawPath(&pen, &arc);
        break;
    }
    case core::TrayCommand::Exit: {
        GraphicsPath path;
        path.AddLine(14.0f, 4.0f, 19.0f, 4.0f);
        path.AddLine(19.0f, 4.0f, 19.0f, 20.0f);
        path.AddLine(19.0f, 20.0f, 14.0f, 20.0f);
        path.StartFigure();
        path.AddLine(11.0f, 8.0f, 15.0f, 12.0f);
        path.AddLine(15.0f, 12.0f, 11.0f, 16.0f);
        path.StartFigure();
        path.AddLine(15.0f, 12.0f, 3.0f, 12.0f);
        g.DrawPath(&pen, &path);
        break;
    }
    default:
        break;
    }

    g.Restore(state);
}

}  // namespace

bool TrayMenuWindow::Open(const core::TrayMenuState& state, const std::wstring& versionLabel,
                          POINT screen) {
    if (window_ != nullptr || !EnsureGdiplus()) {
        return window_ != nullptr;
    }

    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &TrayMenuWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kMenuClass;
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    if (!registered) {
        return false;
    }

    items_ = core::BuildTrayMenu(state);
    version_ = versionLabel;
    recording_ = state.recording;
    paused_ = state.recordingPaused;
    highlight_ = -1;
    pressed_ = -1;

    float itemsDip = 0.0f;
    for (const core::TrayMenuItem& item : items_) {
        itemsDip += item.separator ? kSeparatorHeight : kItemHeight;
    }
    panelHeightDip_ = kHeaderHeight + 1.0f + 5.0f + itemsDip + 5.0f + 1.0f + kFooterHeight;

    scale_ = MonitorScale(screen);
    padPx_ = static_cast<int>(std::lround(kMenuPad * scale_));
    width_ = static_cast<int>(std::lround((kPanelWidth + kMenuPad * 2.0f) * scale_));
    height_ = static_cast<int>(std::lround((panelHeightDip_ + kMenuPad * 2.0f) * scale_));

    if (!CreateLayeredBitmap(&memory_, &bitmap_, &bits_, width_, height_)) {
        return false;
    }

    // MousePoint：左上角跟着光标；右边/下边放不下就翻到光标另一侧，再夹进工作区。
    const HMONITOR monitor = MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    RECT area{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    if (GetMonitorInfoW(monitor, &info)) {
        area = info.rcWork;
    }
    int left = screen.x;
    int top = screen.y;
    if (left + width_ > area.right) {
        left = screen.x - width_;
    }
    if (top + height_ > area.bottom) {
        top = screen.y - height_;
    }
    left = std::clamp(left, static_cast<int>(area.left),
                      std::max(static_cast<int>(area.left),
                               static_cast<int>(area.right) - width_));
    top = std::clamp(top, static_cast<int>(area.top),
                     std::max(static_cast<int>(area.top),
                              static_cast<int>(area.bottom) - height_));

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kMenuClass,
                              L"HaloCap tray menu", WS_POPUP, left, top, width_, height_,
                              nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
        return false;
    }

    Layout();
    Render();
    ShowWindow(window_, SW_SHOW);

    previousForeground_ = GetForegroundWindow();
    SetForegroundWindow(window_);
    SetFocus(window_);
    SetCapture(window_);

    const ULONGLONG start = GetTickCount64();
    for (;;) {
        const float t = static_cast<float>(GetTickCount64() - start) / 120.0f;
        if (t >= 1.0f) {
            break;
        }
        const float u = 1.0f - std::clamp(t, 0.0f, 1.0f);
        Present(static_cast<BYTE>(255 * (1.0f - u * u * u)));
        PumpOnce();
        Sleep(10);
    }
    Present(255);
    // 开场淡入期间线程在忙，系统可能把光标显示成「启动中」；结束时马上改回箭头。
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    return true;
}

void TrayMenuWindow::Close() {
    // 先摘句柄再收尾：DestroyWindow 会同步发 WM_KILLFOCUS/WM_DESTROY，重入的 Close 必须空转。
    HWND handle = window_;
    window_ = nullptr;
    HWND restore = previousForeground_;
    previousForeground_ = nullptr;
    if (handle != nullptr) {
        if (GetCapture() == handle) {
            ReleaseCapture();
        }
        DestroyWindow(handle);
        if (restore != nullptr && IsWindow(restore)) {
            SetForegroundWindow(restore);
        }
    }
    ReleaseLayeredBitmap(&memory_, &bitmap_, &bits_);
    width_ = 0;
    height_ = 0;
    items_.clear();
    itemRects_.clear();
    version_.clear();
    highlight_ = -1;
    pressed_ = -1;
    trackingMouse_ = false;
}

core::TrayCommand TrayMenuWindow::PollCommand() {
    const core::TrayCommand command = pending_;
    pending_ = core::TrayCommand::None;
    return command;
}

RECT TrayMenuWindow::ItemRectForTest(int index) const {
    if (index < 0 || index >= static_cast<int>(itemRects_.size())) {
        return RECT{};
    }
    return itemRects_[static_cast<std::size_t>(index)];
}

void TrayMenuWindow::Layout() {
    const float s = scale_;
    itemRects_.assign(items_.size(), RECT{});
    float y = kHeaderHeight + 1.0f + 5.0f;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].separator) {
            y += kSeparatorHeight;
            continue;
        }
        itemRects_[i] = RECT{
            padPx_ + static_cast<int>(std::lround(6.0f * s)),
            padPx_ + static_cast<int>(std::lround(y * s)),
            padPx_ + static_cast<int>(std::lround(246.0f * s)),
            padPx_ + static_cast<int>(std::lround((y + kItemHeight) * s)),
        };
        y += kItemHeight;
    }
}

void TrayMenuWindow::Render() {
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
    const auto D = [s](float dip) { return dip * s; };

    const RectF panel(static_cast<float>(padPx_), static_cast<float>(padPx_), D(kPanelWidth),
                      D(panelHeightDip_));
    DrawSoftShadow(g, panel, D(12.0f), 0.28f, D(9.0f), D(3.0f), false);
    GraphicsPath panelPath;
    RoundRectPath(panelPath, panel, D(12.0f));
    SolidBrush background(Color(0xFC, 255, 255, 255));
    g.FillPath(&background, &panelPath);
    Pen panelBorder(Color(0x25, 0, 0, 0), std::max(1.0f, s));
    g.DrawPath(&panelBorder, &panelPath);

    g.TranslateTransform(static_cast<float>(padPx_), static_cast<float>(padPx_));

    const Color primary(255, 0x1D, 0x1D, 0x1F);
    const Color tertiary(255, 0x8E, 0x8E, 0x93);
    const Color hairline(0x14, 0, 0, 0);
    const Color accent(255, 0x00, 0x7A, 0xFF);

    StringFormat center;
    center.SetLineAlignment(StringAlignmentCenter);
    center.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat right;
    right.SetAlignment(StringAlignmentFar);
    right.SetLineAlignment(StringAlignmentCenter);
    right.SetFormatFlags(StringFormatFlagsNoWrap);
    StringFormat ellipsis;
    ellipsis.SetLineAlignment(StringAlignmentCenter);
    ellipsis.SetFormatFlags(StringFormatFlagsNoWrap);
    ellipsis.SetTrimming(StringTrimmingEllipsisCharacter);

    // 品牌头：光圈标记 + HaloCap/光圈 + 右侧版本号。
    Pen glyphOuter(Color(0x22, 0, 0, 0), std::max(1.0f, D(1.0f)));
    g.DrawEllipse(&glyphOuter, D(14.0f), D(12.0f), D(22.0f), D(22.0f));
    Pen glyphInner(accent, D(1.6f));
    g.DrawEllipse(&glyphInner, D(17.5f), D(15.5f), D(15.0f), D(15.0f));
    SolidBrush glyphDot(Color(255, 0xF5, 0xB9, 0x63));
    g.FillEllipse(&glyphDot, D(23.25f), D(21.25f), D(3.5f), D(3.5f));

    Font brandFont(UiFamilyName(), D(12.5f), FontStyleBold, UnitPixel);
    SolidBrush brandBrush(primary);
    g.DrawString(L"HaloCap", 7, &brandFont, RectF(D(45.0f), D(12.0f), D(90.0f), D(22.0f)),
                 &center, &brandBrush);
    SizeF brandSize;
    g.MeasureString(L"HaloCap", 7, &brandFont, SizeF(200.0f, 40.0f), &center, &brandSize);
    Font brandCnFont(UiFamilyName(), D(10.0f), FontStyleRegular, UnitPixel);
    SolidBrush tertiaryBrush(tertiary);
    g.DrawString(L"光圈", 2, &brandCnFont,
                 RectF(D(45.0f) + brandSize.Width + D(6.0f), D(13.0f), D(40.0f), D(22.0f)),
                 &center, &tertiaryBrush);
    Font versionFont(UiFamilyName(), D(10.0f), FontStyleRegular, UnitPixel);
    g.DrawString(version_.c_str(), static_cast<INT>(version_.size()), &versionFont,
                 RectF(D(150.0f), D(12.0f), D(88.0f), D(22.0f)), &right, &tertiaryBrush);

    SolidBrush hairlineBrush(hairline);
    g.FillRectangle(&hairlineBrush, D(10.0f), D(kHeaderHeight), D(232.0f),
                    std::max(1.0f, s));

    // 条目：图标 16×16、标题、快捷键列、勾选；hover/pressed/disabled 与 XAML 触发器同值。
    Font itemFont(UiFamilyName(), D(12.5f), FontStyleRegular, UnitPixel);
    Font shortcutFont(UiFamilyName(), D(10.5f), FontStyleRegular, UnitPixel);
    float y = kHeaderHeight + 1.0f + 5.0f;
    for (std::size_t i = 0; i < items_.size(); ++i) {
        const core::TrayMenuItem& item = items_[i];
        if (item.separator) {
            g.FillRectangle(&hairlineBrush, D(14.0f), D(y + 4.0f), D(224.0f),
                            std::max(1.0f, s));
            y += kSeparatorHeight;
            continue;
        }

        const int index = static_cast<int>(i);
        if (highlight_ == index) {
            GraphicsPath host;
            RoundRectPath(host, RectF(D(6.0f), D(y), D(240.0f), D(kItemHeight)), D(7.0f));
            SolidBrush hostBrush(pressed_ == index ? Color(255, 0xE6, 0xEF, 0xFB)
                                                   : Color(255, 0xF0, 0xF5, 0xFC));
            g.FillPath(&hostBrush, &host);
        }

        const float alpha = item.enabled ? 1.0f : 0.36f;
        DrawTrayIcon(g, item.command, recording_, paused_, D(14.0f), D(y + 6.5f), D(16.0f),
                     alpha);

        float shortcutWidth = 0.0f;
        const std::wstring shortcut = Utf8ToWide(item.shortcut);
        if (!shortcut.empty()) {
            SizeF size;
            g.MeasureString(shortcut.c_str(), static_cast<INT>(shortcut.size()), &shortcutFont,
                            SizeF(300.0f * s, 40.0f * s), &center, &size);
            shortcutWidth = size.Width;
            const float rightEdge = item.checked ? D(218.0f) : D(238.0f);
            SolidBrush shortcutBrush(WithAlpha(tertiary, alpha));
            g.DrawString(shortcut.c_str(), static_cast<INT>(shortcut.size()), &shortcutFont,
                         RectF(rightEdge - shortcutWidth - D(2.0f), D(y),
                               shortcutWidth + D(2.0f), D(kItemHeight)),
                         &center, &shortcutBrush);
        }

        if (item.checked) {
            const GraphicsState state = g.Save();
            g.TranslateTransform(D(224.0f), D(y + 7.5f));
            g.ScaleTransform(s, s);
            Pen check(WithAlpha(accent, alpha), 1.7f);
            ApplyRoundCaps(check);
            const PointF points[3] = { PointF(3.0f, 8.5f), PointF(6.2f, 11.7f),
                                       PointF(12.8f, 4.6f) };
            g.DrawLines(&check, points, 3);
            g.Restore(state);
        }

        const bool stopping = item.command == core::TrayCommand::Record && recording_;
        const Color labelColor = stopping ? Color(255, 0xD7, 0x00, 0x15) : primary;
        SolidBrush labelBrush(WithAlpha(labelColor, alpha));
        const std::wstring label = Utf8ToWide(item.label);
        const float rightEdge = item.checked ? D(218.0f) : D(238.0f);
        const float textRight = rightEdge - (shortcut.empty() ? 0.0f : shortcutWidth + D(6.0f));
        g.DrawString(label.c_str(), static_cast<INT>(label.size()), &itemFont,
                     RectF(D(41.0f), D(y), std::max(0.0f, textRight - D(49.0f)),
                           D(kItemHeight)),
                     &ellipsis, &labelBrush);
        y += kItemHeight;
    }

    // 页脚：分隔线 + 右对齐的「Esc 关闭」。
    g.FillRectangle(&hairlineBrush, D(10.0f), D(y + 5.0f), D(232.0f), std::max(1.0f, s));
    Font footerFont(UiFamilyName(), D(9.5f), FontStyleRegular, UnitPixel);
    SolidBrush footerBrush(Color(255, 0xB0, 0xB0, 0xB5));
    g.DrawString(L"Esc 关闭", 6, &footerFont,
                 RectF(D(100.0f), D(y + 13.0f), D(138.0f), D(13.0f)), &right, &footerBrush);
}

void TrayMenuWindow::Present(BYTE alpha) {
    if (window_ == nullptr) {
        return;
    }
    RECT rect{};
    GetWindowRect(window_, &rect);
    PresentLayered(window_, memory_, width_, height_, rect.left, rect.top, alpha);
}

void TrayMenuWindow::Activate(int index) {
    if (index < 0 || index >= static_cast<int>(items_.size())) {
        return;
    }
    const core::TrayMenuItem& item = items_[static_cast<std::size_t>(index)];
    if (item.separator || !item.enabled) {
        return;
    }
    pending_ = item.command;
    Close();
}

void TrayMenuWindow::StepSelection(int direction) {
    const int count = static_cast<int>(items_.size());
    if (count == 0) {
        return;
    }
    for (int step = 1; step <= count; ++step) {
        const int index = highlight_ < 0
                              ? (direction > 0 ? step - 1 : count - step)
                              : ((highlight_ + direction * step) % count + count) % count;
        const core::TrayMenuItem& item = items_[static_cast<std::size_t>(index)];
        if (!item.separator && item.enabled) {
            highlight_ = index;
            Render();
            Present();
            return;
        }
    }
}

void TrayMenuWindow::SelectEdge(bool first) {
    const int count = static_cast<int>(items_.size());
    for (int step = 0; step < count; ++step) {
        const int index = first ? step : count - 1 - step;
        const core::TrayMenuItem& item = items_[static_cast<std::size_t>(index)];
        if (!item.separator && item.enabled) {
            highlight_ = index;
            Render();
            Present();
            return;
        }
    }
}

int TrayMenuWindow::HitItem(POINT client) const {
    for (std::size_t i = 0; i < itemRects_.size(); ++i) {
        const RECT& rect = itemRects_[i];
        if (rect.right > rect.left && client.x >= rect.left && client.x < rect.right &&
            client.y >= rect.top && client.y < rect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

LRESULT CALLBACK TrayMenuWindow::WndProc(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
    auto* self = reinterpret_cast<TrayMenuWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TrayMenuWindow*>(create->lpCreateParams);
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

LRESULT TrayMenuWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_SETCURSOR:
        // 菜单里永远是箭头：类光标在某些时序下会被系统换成「启动中」的转圈光标。
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = HitItem(point);
        const int next =
            (hit >= 0 && items_[static_cast<std::size_t>(hit)].enabled) ? hit : -1;
        if (next != highlight_) {
            highlight_ = next;
            Render();
            Present();
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
        if (highlight_ != -1) {
            highlight_ = -1;
            Render();
            Present();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int hit = HitItem(point);
        if (hit >= 0 && items_[static_cast<std::size_t>(hit)].enabled) {
            pressed_ = hit;
            highlight_ = hit;
            Render();
            Present();
            return 0;
        }
        // 面板外（含阴影留白）：与 1.x StaysOpen=false 一样，点别处就收起。
        if (point.x < padPx_ || point.x >= width_ - padPx_ || point.y < padPx_ ||
            point.y >= height_ - padPx_) {
            Close();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const int pressed = pressed_;
        pressed_ = -1;
        if (pressed >= 0 && HitItem(point) == pressed) {
            Activate(pressed);
        } else if (window_ != nullptr) {
            Render();
            Present();
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        Close();
        return 0;
    case WM_KEYDOWN:
        switch (wParam) {
        case VK_ESCAPE:
            Close();
            return 0;
        case VK_DOWN:
            StepSelection(1);
            return 0;
        case VK_UP:
            StepSelection(-1);
            return 0;
        case VK_HOME:
            SelectEdge(true);
            return 0;
        case VK_END:
            SelectEdge(false);
            return 0;
        case VK_RETURN:
        case VK_SPACE:
            Activate(highlight_);
            return 0;
        default:
            break;
        }
        break;
    case WM_KILLFOCUS:
        Close();
        return 0;
    case WM_CAPTURECHANGED:
        if (pressed_ != -1) {
            pressed_ = -1;
            Render();
            Present();
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