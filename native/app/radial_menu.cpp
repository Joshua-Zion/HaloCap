#include "radial_menu.h"

#include <windowsx.h>
#include <shellscalingapi.h>

#include <objbase.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#include "soft_shadow.h"

namespace halocap::app {

namespace {

using namespace Gdiplus;

constexpr wchar_t kClassName[] = L"HaloCapRadialMenu";
constexpr UINT kMsgTick = WM_APP + 11;

// 1.x 的设计画布：380 DIP，盘半径 170，死区 52，节点半径 110，
// 扇区 56–168，扇区间隙 3.6°，四周 20 DIP 透明边距留给投影。
constexpr float kDesign = 380.0f;
constexpr float kDiskRadius = 170.0f;
constexpr float kDeadZone = 52.0f;
constexpr float kNodeRadius = 110.0f;
constexpr float kSectorInner = 56.0f;
constexpr float kSectorOuter = 168.0f;
constexpr float kSectorGapDegrees = 3.6f;
constexpr float kPadding = 20.0f;

struct ActionSpec {
    core::AppAction action;
    const wchar_t* title;
    const wchar_t* shortcut;
    Color accent;
};

const ActionSpec kActions[4] = {
    { core::AppAction::Screenshot, L"截图", L"1", Color(255, 0x6C, 0xA8, 0xFF) },
    { core::AppAction::Ocr, L"文字识别", L"2", Color(255, 0xB0, 0x8C, 0xFF) },
    { core::AppAction::Record, L"录屏", L"3", Color(255, 0xFF, 0x73, 0x73) },
    { core::AppAction::Pin, L"贴图", L"4", Color(255, 0xF5, 0xB9, 0x63) },
};

RadialMenu* g_active = nullptr;

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

Font* LabelFont() {
    static Font font(L"Microsoft YaHei UI", 12.5f, FontStyleRegular, UnitPixel);
    return &font;
}

Font* CenterFont() {
    static Font font(L"Microsoft YaHei UI", 13.5f, FontStyleBold, UnitPixel);
    return &font;
}

Font* BadgeFont() {
    static Font font(L"Microsoft YaHei UI", 9.5f, FontStyleBold, UnitPixel);
    return &font;
}

PointF DesignPoint(float angleDegrees, float radius) {
    const float radians = (angleDegrees - 90.0f) * 3.14159265f / 180.0f;
    return PointF(190.0f + radius * std::cos(radians), 190.0f + radius * std::sin(radians));
}

GraphicsPath* SectorPath(float startAngle, float endAngle, float inner, float outer) {
    auto* path = new GraphicsPath();
    const PointF outerStart = DesignPoint(startAngle, outer);
    const PointF outerEnd = DesignPoint(endAngle, outer);
    const PointF innerEnd = DesignPoint(endAngle, inner);
    const PointF innerStart = DesignPoint(startAngle, inner);
    path->AddArc(190.0f - outer, 190.0f - outer, outer * 2.0f, outer * 2.0f,
                 startAngle - 90.0f, endAngle - startAngle);
    path->AddLine(outerEnd, innerEnd);
    path->AddArc(190.0f - inner, 190.0f - inner, inner * 2.0f, inner * 2.0f,
                 endAngle - 90.0f, -(endAngle - startAngle));
    path->AddLine(innerStart, outerStart);
    path->CloseFigure();
    return path;
}

GraphicsPath* RoundRectPath(float left, float top, float width, float height, float radius) {
    auto* path = new GraphicsPath();
    const float d = radius * 2.0f;
    path->AddArc(left, top, d, d, 180.0f, 90.0f);
    path->AddArc(left + width - d, top, d, d, 270.0f, 90.0f);
    path->AddArc(left + width - d, top + height - d, d, d, 0.0f, 90.0f);
    path->AddArc(left, top + height - d, d, d, 90.0f, 90.0f);
    path->CloseFigure();
    return path;
}

void DrawCenteredText(Graphics& graphics, const wchar_t* text, Font* font,
                      const RectF& rect, const Color& color) {
    StringFormat format;
    format.SetAlignment(StringAlignmentCenter);
    format.SetLineAlignment(StringAlignmentCenter);
    SolidBrush brush(color);
    graphics.DrawString(text, -1, font, rect, &format, &brush);
}

Color Blend(const Color& from, const Color& to, float t) {
    const auto mix = [t](BYTE a, BYTE b) {
        return static_cast<BYTE>(std::lround(a + (b - a) * t));
    };
    return Color(255, mix(from.GetR(), to.GetR()), mix(from.GetG(), to.GetG()),
                 mix(from.GetB(), to.GetB()));
}

Color WithAlpha(const Color& color, int alpha) {
    return Color(static_cast<BYTE>(std::clamp(alpha, 0, 255)), color.GetR(), color.GetG(),
                 color.GetB());
}

void DrawIcon(Graphics& graphics, int index, const PointF& center, const Color& color) {
    Pen pen(color, 1.55f);
    pen.SetStartCap(LineCapRound);
    pen.SetEndCap(LineCapRound);
    pen.SetLineJoin(LineJoinRound);
    const float x = center.X;
    const float y = center.Y;

    switch (index) {
    case 0: {   // 截图：四角 + 中心点
        graphics.DrawLine(&pen, x - 7.5f, y - 3.5f, x - 7.5f, y - 6.0f);
        graphics.DrawLine(&pen, x - 7.5f, y - 6.0f, x - 5.0f, y - 8.0f);
        graphics.DrawLine(&pen, x + 7.5f, y - 3.5f, x + 7.5f, y - 6.0f);
        graphics.DrawLine(&pen, x + 7.5f, y - 6.0f, x + 5.0f, y - 8.0f);
        graphics.DrawLine(&pen, x - 7.5f, y + 3.5f, x - 7.5f, y + 6.0f);
        graphics.DrawLine(&pen, x - 7.5f, y + 6.0f, x - 5.0f, y + 8.0f);
        graphics.DrawLine(&pen, x + 7.5f, y + 3.5f, x + 7.5f, y + 6.0f);
        graphics.DrawLine(&pen, x + 7.5f, y + 6.0f, x + 5.0f, y + 8.0f);
        SolidBrush dot(color);
        graphics.FillEllipse(&dot, x - 1.2f, y - 1.2f, 2.4f, 2.4f);
        break;
    }
    case 1: {   // 文字识别：T
        graphics.DrawLine(&pen, x - 7.5f, y - 7.5f, x + 7.5f, y - 7.5f);
        graphics.DrawLine(&pen, x, y - 7.5f, x, y + 7.5f);
        graphics.DrawLine(&pen, x - 3.5f, y + 7.5f, x + 3.5f, y + 7.5f);
        break;
    }
    case 2: {   // 录屏：机身 + 镜头
        GraphicsPath* body = RoundRectPath(x - 9.0f, y - 5.0f, 13.0f, 10.0f, 3.0f);
        graphics.DrawPath(&pen, body);
        delete body;
        graphics.DrawLine(&pen, x + 4.5f, y, x + 9.0f, y - 4.0f);
        graphics.DrawLine(&pen, x + 9.0f, y - 4.0f, x + 9.0f, y + 4.0f);
        graphics.DrawLine(&pen, x + 9.0f, y + 4.0f, x + 4.5f, y);
        break;
    }
    default: {  // 贴图：图钉
        graphics.DrawLine(&pen, x - 4.0f, y - 8.0f, x + 4.0f, y - 8.0f);
        graphics.DrawLine(&pen, x + 4.0f, y - 8.0f, x + 5.0f, y - 3.0f);
        graphics.DrawLine(&pen, x + 5.0f, y - 3.0f, x + 8.0f, y - 0.5f);
        graphics.DrawLine(&pen, x + 8.0f, y - 0.5f, x + 8.0f, y + 1.5f);
        graphics.DrawLine(&pen, x + 8.0f, y + 1.5f, x - 8.0f, y + 1.5f);
        graphics.DrawLine(&pen, x - 8.0f, y + 1.5f, x - 8.0f, y - 0.5f);
        graphics.DrawLine(&pen, x - 8.0f, y - 0.5f, x - 5.0f, y - 3.0f);
        graphics.DrawLine(&pen, x - 5.0f, y - 3.0f, x - 4.0f, y - 8.0f);
        graphics.DrawLine(&pen, x, y + 1.5f, x, y + 9.0f);
        break;
    }
    }
}

int WindowDip(core::MenuSize size) {
    switch (size) {
    case core::MenuSize::Small: return 360;
    case core::MenuSize::Large: return 480;
    default: return 420;
    }
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
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool RadialMenu::Open(SelectHandler onSelect, core::MenuSize size) {
    Close();
    if (!EnsureClass() || !EnsureGdiplus()) {
        return false;
    }
    onSelect_ = std::move(onSelect);
    windowDip_ = WindowDip(size);

    POINT cursor{};
    GetCursorPos(&cursor);
    centerScreen_ = cursor;

    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        workArea_ = info.rcWork;
    } else {
        workArea_ = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY)) || dpiX == 0) {
        dpiX = 96;
    }
    pixelScale_ = dpiX / 96.0f;
    contentScale_ = (windowDip_ - 2.0f * kPadding) / kDesign;

    widthPx_ = static_cast<int>(std::lround(windowDip_ * pixelScale_));
    heightPx_ = widthPx_;

    const int margin = static_cast<int>(std::lround(10.0f * pixelScale_));
    int left = cursor.x - widthPx_ / 2;
    int top = cursor.y - heightPx_ / 2;
    const int minLeft = workArea_.left + margin;
    const int maxLeft = workArea_.right - widthPx_ - margin;
    const int minTop = workArea_.top + margin;
    const int maxTop = workArea_.bottom - heightPx_ - margin;
    left = (maxLeft >= minLeft) ? std::clamp(left, minLeft, maxLeft) : workArea_.left;
    top = (maxTop >= minTop) ? std::clamp(top, minTop, maxTop) : workArea_.top;

    HDC screen = GetDC(nullptr);
    memory_ = CreateCompatibleDC(screen);
    BITMAPINFO info2{};
    info2.bmiHeader.biSize = sizeof(info2.bmiHeader);
    info2.bmiHeader.biWidth = widthPx_;
    info2.bmiHeader.biHeight = -heightPx_;
    info2.bmiHeader.biPlanes = 1;
    info2.bmiHeader.biBitCount = 32;
    info2.bmiHeader.biCompression = BI_RGB;
    bitmap_ = CreateDIBSection(memory_, &info2, DIB_RGB_COLORS, &bits_, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (bitmap_ == nullptr || bits_ == nullptr) {
        Close();
        return false;
    }
    SelectObject(memory_, bitmap_);

    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                              kClassName, L"HaloCap radial", WS_POPUP, left, top, widthPx_,
                              heightPx_, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        Close();
        return false;
    }

    centerScreen_ = POINT{ left + widthPx_ / 2, top + heightPx_ / 2 };
    showStartMs_ = GetTickCount64();
    animating_ = true;
    hovered_ = -1;
    for (float& alpha : hoverAlpha_) {
        alpha = 0.0f;
    }

    Render();
    ShowWindow(static_cast<HWND>(window_), SW_SHOWNOACTIVATE);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    SetTimer(static_cast<HWND>(window_), 1, 16, nullptr);

    g_active = this;
    mouseHook_ =
        SetWindowsHookExW(WH_MOUSE_LL, &RadialMenu::MouseHook, GetModuleHandleW(nullptr), 0);
    keyHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, &RadialMenu::KeyHook, GetModuleHandleW(nullptr), 0);
    return true;
}

void RadialMenu::Close() {
    if (mouseHook_ != nullptr) {
        UnhookWindowsHookEx(mouseHook_);
        mouseHook_ = nullptr;
    }
    if (keyHook_ != nullptr) {
        UnhookWindowsHookEx(keyHook_);
        keyHook_ = nullptr;
    }
    if (g_active == this) {
        g_active = nullptr;
    }
    if (window_ != nullptr) {
        KillTimer(static_cast<HWND>(window_), 1);
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
    dragging_ = false;
    hovered_ = -1;
}

struct RadialMenuFrameCache {
    int size = 0;
    std::unique_ptr<Gdiplus::Bitmap> frames[5];
};

void RadialMenu::BuildCache() {
    cache_ = std::make_unique<RadialMenuFrameCache>();
    cache_->size = std::max(1, static_cast<int>(std::lround(kDesign * contentScale_ * pixelScale_)));
    BuildFrame(0);
}

void RadialMenu::BuildFrame(int index) {
    if (cache_ == nullptr || index < 0 || index >= 5 || cache_->frames[index]) {
        return;
    }
    auto frame = std::make_unique<Gdiplus::Bitmap>(cache_->size, cache_->size,
                                                   PixelFormat32bppPARGB);
    Gdiplus::Graphics graphics(frame.get());
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
    graphics.SetCompositingQuality(CompositingQualityHighQuality);
    graphics.Clear(Color(0, 0, 0, 0));
    const float k = static_cast<float>(cache_->size) / kDesign;
    Matrix transform(k, 0.0f, 0.0f, k, 0.0f, 0.0f);
    graphics.SetTransform(&transform);
    DrawDesign(graphics, index - 1);
    cache_->frames[index] = std::move(frame);
}
void RadialMenu::DrawDesign(Gdiplus::Graphics& graphics, int hoverIndex) {    // 外盘投影：真软阴影（降采样 + 盒式模糊），避免一圈圈同心环。
    DrawSoftShadow(graphics, RectF(20.0f, 22.0f, 340.0f, 340.0f), 170.0f, 0.85f, 7.0f, 4.0f,
                   true);
    // 白色径向盘：#FFFFFF → #F8F9FB(0.62) → #EFF1F5。
    GraphicsPath disk;
    disk.AddEllipse(190.0f - kDiskRadius, 190.0f - kDiskRadius, kDiskRadius * 2.0f,
                    kDiskRadius * 2.0f);
    PathGradientBrush diskBrush(&disk);
    diskBrush.SetCenterPoint(PointF(190.0f, 190.0f - 0.14f * kDiskRadius));
    diskBrush.SetCenterColor(Color(255, 255, 255, 255));
    std::vector<Color> surround(static_cast<std::size_t>(disk.GetPointCount()),
                                Color(255, 0xEF, 0xF1, 0xF5));
    INT surroundCount = static_cast<INT>(surround.size());
    diskBrush.SetSurroundColors(surround.data(), &surroundCount);
    graphics.FillPath(&diskBrush, &disk);
    Pen diskBorder(Color(26, 0, 0, 0), 1.0f);
    graphics.DrawEllipse(&diskBorder, 190.0f - kDiskRadius, 190.0f - kDiskRadius,
                         kDiskRadius * 2.0f, kDiskRadius * 2.0f);

    // 四个扇区：基础 #07000000，悬停填充 accent 0x26、外弧 accent 2.2。
    const float halfSweep = 45.0f - kSectorGapDegrees * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const float centerAngle = i * 90.0f;
        GraphicsPath* sector =
            SectorPath(centerAngle - halfSweep, centerAngle + halfSweep, kSectorInner,
                       kSectorOuter);
        SolidBrush base(Color(7, 0, 0, 0));
        graphics.FillPath(&base, sector);

        const float hover = (i == hoverIndex ? 1.0f : 0.0f);
        if (hover > 0.01f) {
            SolidBrush lit(WithAlpha(kActions[i].accent, static_cast<int>(0x26 * hover)));
            graphics.FillPath(&lit, sector);
            Pen arc(WithAlpha(kActions[i].accent, static_cast<int>(255 * hover)), 2.2f);
            arc.SetStartCap(LineCapRound);
            arc.SetEndCap(LineCapRound);
            GraphicsPath arcPath;
            const float radius = kSectorOuter - 0.8f;
            arcPath.AddArc(190.0f - radius, 190.0f - radius, radius * 2.0f, radius * 2.0f,
                           centerAngle - halfSweep - 90.0f, halfSweep * 2.0f);
            graphics.DrawPath(&arc, &arcPath);
        }
        delete sector;
    }

    // 四个动作：节点半径 110，悬停外移 4、图标 1.06。
    for (int i = 0; i < 4; ++i) {
        const float angle = i * 90.0f;
        const PointF node = DesignPoint(angle, kNodeRadius);
        const float radians = (angle - 90.0f) * 3.14159265f / 180.0f;
        const float hover = (i == hoverIndex ? 1.0f : 0.0f);
        const float ox = std::cos(radians) * 4.0f * hover;
        const float oy = std::sin(radians) * 4.0f * hover;
        const PointF iconCenter(node.X + ox, node.Y - 8.0f + oy);

        const float iconScale = 1.0f + 0.06f * hover;
        const float half = 20.0f * iconScale;
        GraphicsPath* iconRect = RoundRectPath(iconCenter.X - half, iconCenter.Y - half,
                                                half * 2.0f, half * 2.0f, 13.0f);
        SolidBrush iconBackground(Color(255, 255, 255, 255));
        graphics.FillPath(&iconBackground, iconRect);
        if (hover > 0.01f) {
            SolidBrush tint(WithAlpha(kActions[i].accent, static_cast<int>(0x26 * hover)));
            graphics.FillPath(&tint, iconRect);
        }
        const Color borderColor =
            Blend(Color(26, 0, 0, 0), kActions[i].accent, hover);
        Pen iconBorder(borderColor, 1.0f);
        graphics.DrawPath(&iconBorder, iconRect);
        delete iconRect;

        const Color iconColor = Blend(Color(255, 0x3C, 0x3C, 0x43), kActions[i].accent, hover);
        DrawIcon(graphics, i, iconCenter, iconColor);

        // 快捷键徽标：17×17，右上角 (-4,-3)。
        const float badgeRadius = 8.5f;
        const PointF badgeCenter(iconCenter.X + 20.0f * iconScale - 5.0f,
                                 iconCenter.Y - 20.0f * iconScale + 4.0f);
        SolidBrush badgeBackground(
            hover > 0.5f ? kActions[i].accent : Color(255, 0xED, 0xEE, 0xF2));
        graphics.FillEllipse(&badgeBackground, badgeCenter.X - badgeRadius,
                             badgeCenter.Y - badgeRadius, badgeRadius * 2.0f,
                             badgeRadius * 2.0f);
        const Color badgeText =
            hover > 0.5f ? Color(255, 0x1D, 0x1D, 0x1F) : Color(255, 0x85, 0x85, 0x8B);
        DrawCenteredText(graphics, kActions[i].shortcut, BadgeFont(),
                         RectF(badgeCenter.X - badgeRadius, badgeCenter.Y - badgeRadius,
                               badgeRadius * 2.0f, badgeRadius * 2.0f),
                         badgeText);

        const Color labelColor =
            Blend(Color(255, 0x51, 0x51, 0x56), Color(255, 0x1D, 0x1D, 0x1F), hover);
        DrawCenteredText(graphics, kActions[i].title, LabelFont(),
                         RectF(node.X + ox - 40.0f, node.Y + 14.0f + oy, 80.0f, 20.0f),
                         labelColor);
    }

    // 中心盘：104 白色圆 + 柔和投影。
    DrawSoftShadow(graphics, RectF(138.0f, 140.0f, 104.0f, 104.0f), 52.0f, 0.25f, 5.0f, 2.0f,
                   true);    SolidBrush centerWhite(Color(255, 255, 255, 255));
    graphics.FillEllipse(&centerWhite, 190.0f - 52.0f, 190.0f - 52.0f, 104.0f, 104.0f);
    Pen centerBorder(Color(20, 0, 0, 0), 1.0f);
    graphics.DrawEllipse(&centerBorder, 190.0f - 52.0f, 190.0f - 52.0f, 104.0f, 104.0f);

    int active = hoverIndex;

    if (active >= 0) {
        SolidBrush dot(kActions[active].accent);
        graphics.FillEllipse(&dot, 190.0f - 3.0f, 190.0f - 20.0f, 6.0f, 6.0f);
        DrawCenteredText(graphics, kActions[active].title, CenterFont(),
                         RectF(190.0f - 48.0f, 190.0f - 2.0f, 96.0f, 22.0f),
                         Color(255, 0x1D, 0x1D, 0x1F));
    } else {
        DrawCenteredText(graphics, L"HaloCap", CenterFont(),
                         RectF(190.0f - 48.0f, 190.0f - 11.0f, 96.0f, 24.0f),
                         Color(255, 0x1D, 0x1D, 0x1F));
    }

}

void RadialMenu::Render() {
    if (bits_ == nullptr || window_ == nullptr) {
        return;
    }
    if (cache_ == nullptr) {
        BuildCache();
    }

    const ULONGLONG now = GetTickCount64();
    const float show = std::clamp((now - showStartMs_) / 160.0f, 0.0f, 1.0f);
    const float ease = 1.0f - std::pow(1.0f - show, 3.0f);
    float scale = 0.93f + 0.07f * ease;
    float fade = 1.0f;
    if (closing_) {
        fade = std::clamp(1.0f - (now - closeStartMs_) / 120.0f, 0.0f, 1.0f);
        scale *= 0.96f + 0.04f * fade;
    }

    Bitmap canvas(widthPx_, heightPx_, PixelFormat32bppPARGB);
    Graphics graphics(&canvas);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    graphics.SetPixelOffsetMode(PixelOffsetModeHalf);
    graphics.Clear(Color(0, 0, 0, 0));

    const int frameIndex = hovered_ >= 0 ? hovered_ + 1 : 0;
    BuildFrame(0);
    Bitmap* neutral = cache_->frames[0].get();
    const float drawSize = cache_->size * scale;
    const float left = (widthPx_ - drawSize) * 0.5f;
    const float top = (heightPx_ - drawSize) * 0.5f;
    const RectF frameDestination(left, top, drawSize, drawSize);
    graphics.DrawImage(neutral, frameDestination);

    if (hovered_ >= 0) {
        BuildFrame(hovered_ + 1);
        Bitmap* hover = cache_->frames[hovered_ + 1].get();
        const float alpha = std::clamp(hoverAlpha_[hovered_], 0.0f, 1.0f);
        if (alpha > 0.01f) {
            const float k = drawSize / kDesign;
            const float centerX = left + drawSize * 0.5f;
            const float centerY = top + drawSize * 0.5f;
            const float centerRadius = 52.0f * k;
            GraphicsPath centerPath;
            centerPath.AddEllipse(centerX - centerRadius, centerY - centerRadius,
                                  centerRadius * 2.0f, centerRadius * 2.0f);
            Gdiplus::Region centerRegion(&centerPath);
            Gdiplus::Region outerRegion(Rect(static_cast<INT>(left), static_cast<INT>(top),
                                             static_cast<INT>(drawSize),
                                             static_cast<INT>(drawSize)));
            outerRegion.Exclude(&centerRegion);

            // 扇区/图标：130 ms 淡入，中心标题不参与淡入（与 1.x 一致，避免文字重影）。
            graphics.SetClip(&outerRegion);
            if (alpha < 0.99f) {
                ColorMatrix matrix = {};
                matrix.m[0][0] = 1.0f;
                matrix.m[1][1] = 1.0f;
                matrix.m[2][2] = 1.0f;
                matrix.m[3][3] = alpha;
                ImageAttributes attributes;
                attributes.SetColorMatrix(&matrix);
                graphics.DrawImage(hover, frameDestination, 0.0f, 0.0f,
                                   static_cast<float>(cache_->size),
                                   static_cast<float>(cache_->size), UnitPixel, &attributes);
            } else {
                graphics.DrawImage(hover, frameDestination);
            }
            graphics.SetClip(&centerRegion);
            graphics.DrawImage(hover, frameDestination);
            graphics.ResetClip();
        }
    }

    const Rect lockRect(0, 0, widthPx_, heightPx_);
    BitmapData data{};
    if (canvas.LockBits(&lockRect, ImageLockModeRead, PixelFormat32bppPARGB, &data) == Ok) {
        for (int y = 0; y < heightPx_; ++y) {
            std::memcpy(static_cast<std::uint8_t*>(bits_) + static_cast<std::size_t>(y) * widthPx_ * 4,
                        static_cast<const std::uint8_t*>(data.Scan0) +
                            static_cast<std::size_t>(y) * data.Stride,
                        static_cast<std::size_t>(widthPx_) * 4);
        }
        canvas.UnlockBits(&data);
    }

    HDC screen = GetDC(nullptr);
    RECT windowRect{};
    GetWindowRect(static_cast<HWND>(window_), &windowRect);
    POINT destination{ windowRect.left, windowRect.top };
    SIZE windowSize{ widthPx_, heightPx_ };
    POINT source{ 0, 0 };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = static_cast<BYTE>(std::lround(255.0f * std::min(1.0f, show * 1.15f) * fade));
    blend.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(static_cast<HWND>(window_), screen, &destination, &windowSize, memory_,
                        &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}
void RadialMenu::Tick() {
    const ULONGLONG now = GetTickCount64();
    if (closing_) {
        Render();
        if (now - closeStartMs_ >= 120) {
            const SelectHandler handler = onSelect_;
            const int index = pendingAction_;
            pendingAction_ = -1;
            Close();
            if (index >= 0 && handler) {
                handler(kActions[index].action);
            }
        }
        return;
    }

    bool settled = true;
    for (int i = 0; i < 4; ++i) {
        const float target = (i == hovered_) ? 1.0f : 0.0f;
        const float step = 16.0f / 130.0f;
        if (hoverAlpha_[i] < target) {
            hoverAlpha_[i] = std::min(target, hoverAlpha_[i] + step);
            settled = false;
        } else if (hoverAlpha_[i] > target) {
            hoverAlpha_[i] = std::max(target, hoverAlpha_[i] - step);
            settled = false;
        }
    }

    const float show = std::clamp((now - showStartMs_) / 160.0f, 0.0f, 1.0f);
    Render();
    if (show >= 1.0f && settled) {
        KillTimer(static_cast<HWND>(window_), 1);
    }
}
void RadialMenu::UpdateHover(POINT screenPoint) {
    if (dragging_) {
        return;
    }
    const float dx = static_cast<float>(screenPoint.x - centerScreen_.x);
    const float dy = static_cast<float>(screenPoint.y - centerScreen_.y);
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float designToPx = contentScale_ * pixelScale_;
    const float deadPx = kDeadZone * designToPx;
    const float diskPx = kDiskRadius * designToPx;

    if (distance < deadPx || distance > diskPx) {
        if (hovered_ != -1) {
            hovered_ = -1;
            SetTimer(static_cast<HWND>(window_), 1, 16, nullptr);
        }
        return;
    }

    float angle = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
    if (angle < 0.0f) {
        angle += 360.0f;
    }
    const int index = static_cast<int>(std::floor((angle + 45.0f) / 90.0f)) % 4;
    if (index != hovered_) {
        hovered_ = index;
        SetTimer(static_cast<HWND>(window_), 1, 16, nullptr);
    }
}

void RadialMenu::Select(int index) {
    if (index < 0 || index >= 4 || closing_) {
        return;
    }
    if (kActions[index].action == core::AppAction::Pin) {
        // 1.x：贴图没有选区要立即让出屏幕，保留 120 ms 淡出。
        StartHide(index);
        return;
    }
    const SelectHandler handler = onSelect_;
    const core::AppAction action = kActions[index].action;
    Close();
    if (handler) {
        handler(action);
    }
}

void RadialMenu::StartHide(int pendingAction) {
    if (closing_) {
        return;
    }
    closing_ = true;
    pendingAction_ = pendingAction;
    closeStartMs_ = GetTickCount64();
    SetTimer(static_cast<HWND>(window_), 1, 16, nullptr);
}

void RadialMenu::ClickAt(POINT screenPoint) {
    if (closing_) {
        return;
    }
    const float dx = static_cast<float>(screenPoint.x - centerScreen_.x);
    const float dy = static_cast<float>(screenPoint.y - centerScreen_.y);
    const float distance = std::sqrt(dx * dx + dy * dy);
    const float designToPx = contentScale_ * pixelScale_;
    if (distance < kDeadZone * designToPx) {
        StartDrag(screenPoint);
        return;
    }
    if (distance <= kDiskRadius * designToPx) {
        float angle = std::atan2(dx, -dy) * 180.0f / 3.14159265f;
        if (angle < 0.0f) {
            angle += 360.0f;
        }
        const int index = static_cast<int>(std::floor((angle + 45.0f) / 90.0f)) % 4;
        Select(index);
        return;
    }
    StartHide(-1);
}

void RadialMenu::StartDrag(POINT screenPoint) {
    dragging_ = true;
    dragOffset_ = POINT{ centerScreen_.x - screenPoint.x, centerScreen_.y - screenPoint.y };
    hovered_ = -1;
    SetTimer(static_cast<HWND>(window_), 1, 16, nullptr);
}

void RadialMenu::DragTo(POINT screenPoint) {
    if (!dragging_ || window_ == nullptr) {
        return;
    }
    const int left = screenPoint.x + dragOffset_.x - widthPx_ / 2;
    const int top = screenPoint.y + dragOffset_.y - heightPx_ / 2;
    SetWindowPos(static_cast<HWND>(window_), HWND_TOPMOST, left, top, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    centerScreen_ = POINT{ left + widthPx_ / 2, top + heightPx_ / 2 };
}

LRESULT CALLBACK RadialMenu::MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_active == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const auto* mouse = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    switch (wParam) {
    case WM_MOUSEMOVE:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (g_active->dragging_) {
            g_active->DragTo(mouse->pt);
        } else {
            g_active->UpdateHover(mouse->pt);
        }
        // 移动必须放行，否则系统光标会被钩子冻住。
        return CallNextHookEx(nullptr, code, wParam, lParam);
    case WM_LBUTTONDOWN:
        g_active->ClickAt(mouse->pt);
        return 1;
    case WM_LBUTTONUP:
        g_active->dragging_ = false;
        return 1;
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        g_active->StartHide(-1);
        return 1;
    default:
        return 1;
    }
}

LRESULT CALLBACK RadialMenu::KeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_active == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    const DWORD key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode;
    if (key == VK_ESCAPE) {
        g_active->StartHide(-1);
        return 1;
    }
    if (key >= '1' && key <= '4') {
        PostMessageW(static_cast<HWND>(g_active->window_), WM_APP + 12, key - '1', 0);
        return 1;
    }
    if (key >= VK_NUMPAD1 && key <= VK_NUMPAD4) {
        PostMessageW(static_cast<HWND>(g_active->window_), WM_APP + 12, key - VK_NUMPAD1, 0);
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

LRESULT RadialMenu::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        POINT cursor{};
        GetCursorPos(&cursor);
        UpdateHover(cursor);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_LBUTTONDOWN: {
        POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(window, &cursor);
        ClickAt(cursor);
        return 0;
    }
    case WM_LBUTTONUP:
        dragging_ = false;
        return 0;
    case WM_TIMER:
        if (wParam == 1) {
            Tick();
            return 0;
        }
        break;
    case WM_APP + 12:
        Select(static_cast<int>(wParam));
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
