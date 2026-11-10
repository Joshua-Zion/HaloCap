#include "pin_window.h"

#include "display_list.h"
#include "image_export.h"
#include "mosaic.h"
#include "path.h"
#include "toolbar_icons.gen.h"

#include <windows.h>

#include <windowsx.h>

#include <commdlg.h>

#include <objbase.h>

#include <gdiplus.h>
#include <shlobj.h>

#pragma comment(lib, "comdlg32.lib")

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

namespace halocap::app {

namespace {

constexpr wchar_t kPinClass[] = L"HaloCapPinWindow";
constexpr wchar_t kMenuClass[] = L"HaloCapPinMenu";
constexpr wchar_t kEditorClass[] = L"HaloCapPinTextEditor";

constexpr UINT kMsgCommitEditor = WM_APP + 1;
constexpr UINT kMsgCancelEditor = WM_APP + 2;

constexpr UINT_PTR kHudTimer = 1;

enum MenuId : int {
    kMenuAnnotate = 1,
    kMenuOcr,
    kMenuCopy,
    kMenuSave,
    kMenuReset,
    kMenuOutline,
    kMenuClose,
    kMenuCloseAll,
};

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

Gdiplus::Color ToGp(const core::Color& color) {
    const auto channel = [](float value) {
        return static_cast<BYTE>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return Gdiplus::Color(channel(color.a), channel(color.r), channel(color.g),
                          channel(color.b));
}

Gdiplus::RectF ToGp(const core::Rect& rect) {
    return Gdiplus::RectF(rect.left, rect.top, rect.Width(), rect.Height());
}

// GDI+ 的 GraphicsPath 拷贝构造是 protected，不能按值返回，只能填进调用方的对象。
void BuildRoundRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    const float diameter = std::min(radius * 2.0f, std::min(rect.Width, rect.Height));
    if (diameter <= 0.5f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - diameter, rect.GetBottom() - diameter, diameter, diameter,
                0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

std::wstring Widen(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(0, needed)), L'\0');
    if (needed > 0) {
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            result.data(), needed);
    }
    return result;
}

std::string Narrow(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0, nullptr,
                                           nullptr);
    std::string result(static_cast<std::size_t>(std::max(0, needed)), '\0');
    if (needed > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                            result.data(), needed, nullptr, nullptr);
    }
    return result;
}

struct ParsedIcon {
    std::vector<core::PathCommand> first;
    std::vector<core::PathCommand> second;
    bool firstEvenOdd = false;
    bool secondEvenOdd = false;
    bool valid = false;
};

const ParsedIcon* IconFor(const char* name) {
    static std::map<std::string, ParsedIcon> cache;
    const auto cached = cache.find(name);
    if (cached != cache.end()) {
        return &cached->second;
    }
    const IconPathData* source = nullptr;
    for (const IconPathData& candidate : kIconPaths) {
        if (std::strcmp(candidate.name, name) == 0) {
            source = &candidate;
            break;
        }
    }
    ParsedIcon parsed;
    if (source != nullptr) {
        if (source->first != nullptr) {
            parsed.valid = core::ParsePathData(source->first, &parsed.first,
                                               &parsed.firstEvenOdd) ||
                           !parsed.first.empty();
        }
        if (source->second != nullptr) {
            core::ParsePathData(source->second, &parsed.second, &parsed.secondEvenOdd);
        }
    }
    cache[name] = std::move(parsed);
    return &cache[name];
}

void AddPathCommands(Gdiplus::GraphicsPath& path, const std::vector<core::PathCommand>& commands,
                     float scale, float originX, float originY) {
    const auto toPoint = [&](const core::Point& point) {
        return Gdiplus::PointF(originX + point.x * scale, originY + point.y * scale);
    };
    Gdiplus::PointF current(0.0f, 0.0f);
    bool figure = false;
    for (const core::PathCommand& command : commands) {
        switch (command.kind) {
        case core::PathCommand::Kind::Move:
            path.StartFigure();
            current = toPoint(command.points[0]);
            figure = true;
            break;
        case core::PathCommand::Kind::Line:
            if (figure) {
                const Gdiplus::PointF point = toPoint(command.points[0]);
                path.AddLine(current, point);
                current = point;
            }
            break;
        case core::PathCommand::Kind::Cubic:
            if (figure) {
                const Gdiplus::PointF control1 = toPoint(command.points[0]);
                const Gdiplus::PointF control2 = toPoint(command.points[1]);
                const Gdiplus::PointF end = toPoint(command.points[2]);
                path.AddBezier(current, control1, control2, end);
                current = end;
            }
            break;
        case core::PathCommand::Kind::Quad:
            if (figure) {
                const Gdiplus::PointF control = toPoint(command.points[0]);
                const Gdiplus::PointF end = toPoint(command.points[2]);
                const Gdiplus::PointF control1(current.X + (control.X - current.X) * 2.0f / 3.0f,
                                               current.Y + (control.Y - current.Y) * 2.0f / 3.0f);
                const Gdiplus::PointF control2(end.X + (control.X - end.X) * 2.0f / 3.0f,
                                               end.Y + (control.Y - end.Y) * 2.0f / 3.0f);
                path.AddBezier(current, control1, control2, end);
                current = end;
            }
            break;
        case core::PathCommand::Kind::Close:
            if (figure) {
                path.CloseFigure();
                figure = false;
            }
            break;
        }
    }
}

// 1.x 的图标是 160 单位的路径装进 200×200 的盒子，再缩到 20 DIP（App.xaml 的 Viewbox）。
// 图标尺寸和按钮尺寸分开传：覆盖层的二级栏是 26 DIP 按钮，图标仍是 20 DIP。
void DrawIcon(Gdiplus::Graphics& graphics, const RECT& rect, const char* name,
              const Gdiplus::Color& color, float iconSize) {
    if (name == nullptr) {
        return;
    }
    const ParsedIcon* parsed = IconFor(name);
    if (parsed == nullptr || !parsed->valid) {
        return;
    }
    const float scale = iconSize / 200.0f;
    const float originX = (rect.left + rect.right) * 0.5f - iconSize * 0.5f;
    const float originY = (rect.top + rect.bottom) * 0.5f - iconSize * 0.5f;

    Gdiplus::SolidBrush brush(color);
    const auto draw = [&](const std::vector<core::PathCommand>& commands, bool evenOdd) {
        if (commands.empty()) {
            return;
        }
        Gdiplus::GraphicsPath path;
        path.SetFillMode(evenOdd ? Gdiplus::FillModeAlternate : Gdiplus::FillModeWinding);
        AddPathCommands(path, commands, scale, originX, originY);
        graphics.FillPath(&brush, &path);
    };
    draw(parsed->first, parsed->firstEvenOdd);
    draw(parsed->second, parsed->secondEvenOdd);
}

// 把 core 的显示列表画进 GDI+。Blit 不在这里画：文字与马赛克块由调用方按
// 自己的数据源绘制（贴图窗口没有字体图集，也没有后端资源表）。
void DrawPlan(Gdiplus::Graphics& graphics, const core::FramePlan& plan) {
    for (const core::Item& item : plan.Items()) {
        std::visit(
            [&](const auto& specific) {
                using T = std::decay_t<decltype(specific)>;
                if constexpr (std::is_same_v<T, core::FillRect>) {
                    Gdiplus::SolidBrush brush(ToGp(specific.color));
                    graphics.FillRectangle(&brush, ToGp(specific.rect));
                } else if constexpr (std::is_same_v<T, core::StrokeRect>) {
                    Gdiplus::Pen pen(ToGp(specific.color), specific.thickness);
                    graphics.DrawRectangle(&pen, ToGp(specific.rect));
                } else if constexpr (std::is_same_v<T, core::FillEllipse>) {
                    Gdiplus::SolidBrush brush(ToGp(specific.color));
                    graphics.FillEllipse(&brush, ToGp(specific.rect));
                } else if constexpr (std::is_same_v<T, core::StrokeEllipse>) {
                    Gdiplus::Pen pen(ToGp(specific.color), specific.thickness);
                    graphics.DrawEllipse(&pen, ToGp(specific.rect));
                } else if constexpr (std::is_same_v<T, core::FillRoundedRect>) {
                    Gdiplus::SolidBrush brush(ToGp(specific.color));
                    Gdiplus::GraphicsPath path;
                    BuildRoundRect(path, ToGp(specific.rect), specific.radius);
                    graphics.FillPath(&brush, &path);
                } else if constexpr (std::is_same_v<T, core::StrokeRoundedRect>) {
                    Gdiplus::Pen pen(ToGp(specific.color), specific.thickness);
                    Gdiplus::GraphicsPath path;
                    BuildRoundRect(path, ToGp(specific.rect), specific.radius);
                    graphics.DrawPath(&pen, &path);
                } else if constexpr (std::is_same_v<T, core::Segment>) {
                    Gdiplus::Pen pen(ToGp(specific.color), specific.thickness);
                    pen.SetStartCap(Gdiplus::LineCapRound);
                    pen.SetEndCap(Gdiplus::LineCapRound);
                    graphics.DrawLine(&pen, specific.from.x, specific.from.y, specific.to.x,
                                      specific.to.y);
                } else if constexpr (std::is_same_v<T, core::PathShape>) {
                    if (specific.commands.empty()) {
                        return;
                    }
                    Gdiplus::GraphicsPath path;
                    path.SetFillMode(specific.evenOdd ? Gdiplus::FillModeAlternate
                                                      : Gdiplus::FillModeWinding);
                    AddPathCommands(path, specific.commands, 1.0f, 0.0f, 0.0f);
                    if (specific.strokeWidth > 0.0f) {
                        Gdiplus::Pen pen(ToGp(specific.color), specific.strokeWidth);
                        pen.SetStartCap(Gdiplus::LineCapRound);
                        pen.SetEndCap(Gdiplus::LineCapRound);
                        pen.SetLineJoin(Gdiplus::LineJoinRound);
                        graphics.DrawPath(&pen, &path);
                    } else {
                        Gdiplus::SolidBrush brush(ToGp(specific.color));
                        graphics.FillPath(&brush, &path);
                    }
                }
            },
            item);
    }
}

float MeasureTextWidth(const std::wstring& text, const std::wstring& family, float sizePx,
                       int style) {
    BYTE pixel[4] = {};
    Gdiplus::Bitmap surface(1, 1, 4, PixelFormat32bppPARGB, pixel);
    Gdiplus::Graphics graphics(&surface);
    Gdiplus::Font font(family.c_str(), sizePx, style, Gdiplus::UnitPixel);
    Gdiplus::RectF bounds;
    graphics.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font,
                           Gdiplus::PointF(0.0f, 0.0f), &bounds);
    return bounds.Width;
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
    if (SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures) !=
        S_OK) {
        return {};
    }
    return std::wstring(pictures) + L"\\HaloCap";
}

const core::Color kInkNormal{ 0.235f, 0.259f, 0.314f, 1.0f };   // #3C4250
const core::Color kAccent{ 0.184f, 0.486f, 0.965f, 1.0f };       // #2F7CF6

}  // namespace

PinWindow* PinWindow::g_menuOwner = nullptr;
PinWindow* PinWindow::g_editorOwner = nullptr;
WNDPROC PinWindow::g_oldEditProc = nullptr;
HFONT PinWindow::g_editorFont = nullptr;
HBRUSH PinWindow::g_editorBrush = nullptr;

PinWindow::PinWindow() = default;

PinWindow::~PinWindow() { Close(); }

bool PinWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = CS_DBLCLKS;
        description.lpfnWndProc = &PinWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kPinClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool PinWindow::EnsureMenuClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &PinWindow::MenuProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kMenuClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool PinWindow::EnsureEditorClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &PinWindow::EditorProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kEditorClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool PinWindow::Create(const std::uint8_t* bgra, int width, int height, int screenX, int screenY,
                       const PinWindowOptions& options) {
    if (bgra == nullptr || width <= 0 || height <= 0 || !EnsureClass() || !EnsureGdiplus()) {
        return false;
    }

    options_ = options;
    fontFamilyW_ = Widen(options_.fontFamily);
    if (fontFamilyW_.empty()) {
        fontFamilyW_ = L"Microsoft YaHei UI";
    }
    content_.assign(bgra, bgra + static_cast<std::size_t>(width) * height * 4);
    contentWidth_ = width;
    contentHeight_ = height;
    contentScreenX_ = screenX;
    contentScreenY_ = screenY;
    zoom_ = 1.0f;
    opacity_ = 1.0f;
    style_ = core::AnnotationStyle{};
    style_.color = core::annotation_defaults::PaletteColor(0);
    style_.thickness = core::annotation_defaults::kThickness;
    style_.textSize = core::annotation_defaults::kTextSize;
    tool_ = core::AnnotationTool::Pen;
    shapeTool_ = core::AnnotationTool::Rectangle;
    eraserSize_ = core::annotation_defaults::kEraserSize;

    const DWORD exStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    window_ = CreateWindowExW(exStyle, kPinClass, L"HaloCap pin", WS_POPUP, screenX, screenY,
                              width, height, nullptr, nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    {
        const UINT dpi = GetDpiForWindow(static_cast<HWND>(window_));
        uiScale_ = dpi >= 48 ? static_cast<float>(dpi) / 96.0f : 1.0f;
        if (uiScale_ < 1.0f) {
            uiScale_ = 1.0f;
        }
    }

    UpdateLayout();
    Render();
    ShowWindow(static_cast<HWND>(window_), SW_SHOW);
    SetForegroundWindow(static_cast<HWND>(window_));
    SetFocus(static_cast<HWND>(window_));
    return true;
}

void PinWindow::Close() {
    HideMenu();
    DestroyTextEditor();
    dragging_ = false;
    drawing_ = false;
    mosaicPainting_ = false;
    if (window_ != nullptr) {
        DestroyWindow(static_cast<HWND>(window_));
        window_ = nullptr;
    }
}

core::IntRect PinWindow::ContentRectForTest() const {
    return core::IntRect{ contentLeft_, contentTop_, contentLeft_ + displayWidth_,
                          contentTop_ + displayHeight_ };
}

int PinWindow::OptionContentRightForTest() const {
    int right = 0;
    for (const PinButton& button : optionButtons_) {
        if (!button.separator) {
            right = std::max(right, static_cast<int>(button.rect.right));
        }
    }
    return right;
}

void PinWindow::UpdateLayout() {
    if (window_ == nullptr) {
        return;
    }
    displayWidth_ = std::max(1, static_cast<int>(std::lround(contentWidth_ * zoom_)));
    displayHeight_ = std::max(1, static_cast<int>(std::lround(contentHeight_ * zoom_)));
    // 「外描边」就是这圈阴影（1.x 的 PinGlowEnabled）：关掉之后窗口只剩图像本身，
    // 和屏幕上的原内容完全重合，看起来就是「融进去了」。
    glow_ = options_.outline ? std::max(4, ScaleInt(10.0f)) : 0;
    border_ = options_.outline ? std::max(1, ScaleInt(1.0f)) : 0;

    const int pad = ScaleInt(4.0f);
    const int gap = ScaleInt(6.0f);
    toolbarHeight_ = ScaleInt(30.0f) + 2 * ScaleInt(4.0f);
    // 和截图覆盖层的二级栏同尺寸：按钮 26 DIP、高 34 DIP（kOptionButton/kOptionHeight）。
    optionHeight_ = ScaleInt(34.0f);
    chromeHeight_ = annotating_ ? pad + toolbarHeight_ + gap + optionHeight_ + pad : 0;

    LayoutToolbar(0, 0);
    LayoutOptionBar(0, 0);

    const int imageOuterWidth = displayWidth_ + 2 * (glow_ + border_);
    const int imageOuterHeight = displayHeight_ + 2 * (glow_ + border_);
    windowWidth_ = std::max(imageOuterWidth, std::max(toolbarWidth_, optionWidth_));
    windowHeight_ = imageOuterHeight + chromeHeight_;

    const int sideInset = (windowWidth_ - imageOuterWidth) / 2;
    contentLeft_ = sideInset + glow_ + border_;

    // 工具栏放下面还是翻到上面：看贴图所在显示器工作区的剩余空间（1.x 的 PreferToolbarAbove）。
    chromeAbove_ = false;
    if (annotating_) {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        const POINT center{ contentScreenX_ + displayWidth_ / 2,
                            contentScreenY_ + displayHeight_ / 2 };
        if (GetMonitorInfoW(MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST), &info)) {
            const int spaceBelow = static_cast<int>(info.rcWork.bottom) -
                                   (contentScreenY_ + displayHeight_ + glow_ + border_);
            const int spaceAbove =
                contentScreenY_ - glow_ - border_ - static_cast<int>(info.rcWork.top);
            chromeAbove_ = spaceBelow < chromeHeight_ && spaceAbove > spaceBelow;
        }
    }
    contentTop_ = glow_ + border_ + (chromeAbove_ ? chromeHeight_ : 0);

    if (annotating_) {
        if (chromeAbove_) {
            LayoutOptionBar((windowWidth_ - optionWidth_) / 2, pad);
            LayoutToolbar((windowWidth_ - toolbarWidth_) / 2, pad + optionHeight_ + gap);
        } else {
            const int optionTop = windowHeight_ - pad - optionHeight_;
            LayoutToolbar((windowWidth_ - toolbarWidth_) / 2,
                          optionTop - gap - toolbarHeight_);
            LayoutOptionBar((windowWidth_ - optionWidth_) / 2, optionTop);
        }
    }

    const int windowX = contentScreenX_ - contentLeft_;
    const int windowY = contentScreenY_ - contentTop_;
    RECT current{};
    GetWindowRect(static_cast<HWND>(window_), &current);
    if (current.left != windowX || current.top != windowY ||
        current.right - current.left != windowWidth_ ||
        current.bottom - current.top != windowHeight_) {
        SetWindowPos(static_cast<HWND>(window_), HWND_TOPMOST, windowX, windowY, windowWidth_,
                     windowHeight_, SWP_NOACTIVATE);
    }
}

void PinWindow::SetOutline(bool enabled) {
    if (options_.outline == enabled) {
        return;
    }
    options_.outline = enabled;
    Render();
}

void PinWindow::SetOcrEnabled(bool enabled) {
    if (options_.ocrEnabled == enabled) {
        return;
    }
    options_.ocrEnabled = enabled;
    Render();
}

void PinWindow::SetAnnotationMode(bool enabled) {
    if (annotating_ == enabled) {
        return;
    }
    annotating_ = enabled;
    hoveredToolbar_ = -1;
    hoveredOption_ = -1;
    if (!enabled) {
        CommitTextEditor();
        if (drawing_ || mosaicPainting_) {
            // 1.x 退出标注会丢弃没落完的这一笔。
            document_.Cancel();
            drawing_ = false;
            mosaicPainting_ = false;
        }
    } else {
        SetForegroundWindow(static_cast<HWND>(window_));
        SetFocus(static_cast<HWND>(window_));
    }
    UpdateLayout();
    Render();
    if (enabled) {
        ShowHud(L"标注模式 · 滚轮调参数 · 空格退出");
    }
}

void PinWindow::SetTool(core::AnnotationTool tool) {
    tool_ = tool;
    if (tool == core::AnnotationTool::Highlighter) {
        style_.thickness = core::annotation_defaults::kHighlighterThickness;
    } else if (tool != core::AnnotationTool::Rectangle && tool != core::AnnotationTool::Ellipse &&
               style_.thickness > core::annotation_defaults::kMaxThickness / 2.0f) {
        style_.thickness = core::annotation_defaults::kThickness;
    }
    UpdateLayout();
    Render();
}

void PinWindow::UpdateHover(int index, bool optionBar) {
    int& target = optionBar ? hoveredOption_ : hoveredToolbar_;
    if (target == index) {
        return;
    }
    target = index;
    Render();
}

bool PinWindow::IsOverChrome(POINT client) const {
    if (!annotating_) {
        return false;
    }
    return ToolbarButtonAt(client) >= 0 || OptionButtonAt(client) >= 0;
}

int PinWindow::ToolbarButtonAt(POINT client) const {
    for (std::size_t i = 0; i < toolbar_.size(); ++i) {
        const RECT& rect = toolbar_[i].rect;
        if (!toolbar_[i].separator && client.x >= rect.left && client.x < rect.right &&
            client.y >= rect.top && client.y < rect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int PinWindow::OptionButtonAt(POINT client) const {
    for (std::size_t i = 0; i < optionButtons_.size(); ++i) {
        const RECT& rect = optionButtons_[i].rect;
        if (optionButtons_[i].separator || optionButtons_[i].action == PinAction::None) {
            continue;
        }
        if (client.x >= rect.left && client.x < rect.right && client.y >= rect.top &&
            client.y < rect.bottom) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ---- 标注工具栏与选项栏 ----

void PinWindow::LayoutToolbar(int originX, int originY) {
    toolbar_.clear();
    if (!annotating_) {
        toolbarWidth_ = 0;
        return;
    }
    const int btn = ScaleInt(30.0f);
    const int gap = ScaleInt(2.0f);
    const int pad = ScaleInt(4.0f);
    const int separator = std::max(1, ScaleInt(1.0f));

    int x = originX + pad;
    const auto addButton = [&](PinAction action, const char* icon, bool active, bool enabled) {
        PinButton button;
        button.rect = RECT{ x, originY + pad, x + btn, originY + pad + btn };
        button.action = action;
        button.icon = icon;
        button.active = active;
        button.enabled = enabled;
        toolbar_.push_back(button);
        x += btn + gap;
    };
    const auto addSeparator = [&] {
        // 与覆盖层同一套推进规则：分隔线两侧各留 4 DIP，线本身画 22 DIP 高。
        const int margin = ScaleInt(4.0f);
        x += margin - gap;
        PinButton line;
        line.separator = true;
        line.rect = RECT{ x, originY + pad + ScaleInt(4.0f), x + separator,
                          originY + pad + btn - ScaleInt(4.0f) };
        toolbar_.push_back(line);
        x += separator + margin;
    };

    const bool shapeActive = tool_ == core::AnnotationTool::Rectangle ||
                             tool_ == core::AnnotationTool::Ellipse;
    addButton(PinAction::ToolShape, "Shape", shapeActive, true);
    addButton(PinAction::ToolArrow, "Arrow", tool_ == core::AnnotationTool::Arrow, true);
    addButton(PinAction::ToolPen, "Pen", tool_ == core::AnnotationTool::Pen, true);
    addButton(PinAction::ToolHighlighter, "Highlighter",
              tool_ == core::AnnotationTool::Highlighter, true);
    addButton(PinAction::ToolEraser, "Eraser", tool_ == core::AnnotationTool::Eraser, true);
    addButton(PinAction::ToolText, "Text", tool_ == core::AnnotationTool::Text, true);
    addButton(PinAction::ToolMosaic, "Mosaic", tool_ == core::AnnotationTool::Mosaic, true);
    addButton(PinAction::ToolStep, "Step", tool_ == core::AnnotationTool::Step, true);
    addSeparator();
    addButton(PinAction::Undo, "Undo", false, document_.CanUndo());
    addButton(PinAction::Redo, "Redo", false, document_.CanRedo());
    addButton(PinAction::Clear, "Clear", false, !document_.Items().empty());
    addSeparator();
    addButton(PinAction::CloseEdit, "Close", false, true);
    addButton(PinAction::Done, "Done", false, true);

    // 宽度是「内容宽度」，必须减掉 originX：第二次按居中位置重排时 originX 不再是 0，
    // 不减就会把面板撑大 originX 那么多（二级栏右边那截空白的来源）。
    toolbarWidth_ = (x - originX) - gap + pad;
    toolbarBounds_ =
        RECT{ originX, originY, originX + toolbarWidth_, originY + toolbarHeight_ };
}

void PinWindow::LayoutOptionBar(int originX, int originY) {
    optionButtons_.clear();
    if (!annotating_) {
        optionWidth_ = 0;
        return;
    }
    // 与 ScreenshotOverlay::LayoutOptionBar 对齐：按钮 26 DIP、色板格 22 DIP、数值格 30 DIP。
    const int btn = ScaleInt(26.0f);
    const int colorWidth = ScaleInt(22.0f);
    const int gap = ScaleInt(2.0f);
    const int pad = ScaleInt(4.0f);
    const int separatorWidth = std::max(1, ScaleInt(1.0f));
    const int valueWidth = ScaleInt(30.0f);

    int x = originX + pad;
    const auto add = [&](PinAction action, const char* icon, bool active, bool enabled) {
        PinButton button;
        button.rect = RECT{ x, originY + pad, x + btn, originY + pad + btn };
        button.action = action;
        button.icon = icon;
        button.active = active;
        button.enabled = enabled;
        optionButtons_.push_back(button);
        x += btn + gap;
    };
    const auto addDot = [&](int index) {
        PinButton button;
        button.rect = RECT{ x, originY + pad, x + colorWidth, originY + pad + btn };
        button.action = PinAction::Color;
        button.payload = index;
        button.dot = true;
        button.dotColor = core::annotation_defaults::PaletteColor(index);
        button.active = style_.color == button.dotColor;
        optionButtons_.push_back(button);
        x += colorWidth + gap;
    };
    const auto addValue = [&](const std::wstring& text) {
        PinButton button;
        button.rect = RECT{ x, originY + pad, x + valueWidth, originY + pad + btn };
        button.text = text;
        optionButtons_.push_back(button);
        x += valueWidth + gap;
    };
    const auto addSeparator = [&] {
        const int margin = ScaleInt(4.0f);
        x += margin - gap;
        PinButton line;
        line.separator = true;
        line.rect = RECT{ x, originY + pad + ScaleInt(4.0f), x + separatorWidth,
                          originY + pad + btn - ScaleInt(4.0f) };
        optionButtons_.push_back(line);
        x += separatorWidth + margin;
    };

    const bool showColors = tool_ != core::AnnotationTool::Mosaic &&
                            tool_ != core::AnnotationTool::Eraser;
    if (showColors) {
        for (int index = 0; index < core::annotation_defaults::kPaletteCount; ++index) {
            addDot(index);
        }
        if (tool_ != core::AnnotationTool::Step) {
            addSeparator();
        }
    }

    switch (tool_) {
    case core::AnnotationTool::Rectangle:
    case core::AnnotationTool::Ellipse:
        add(PinAction::ShapeRectangle, "Rectangle", tool_ == core::AnnotationTool::Rectangle,
            true);
        add(PinAction::ShapeEllipse, "Ellipse", tool_ == core::AnnotationTool::Ellipse, true);
        add(PinAction::FillOn, "Outline", !style_.filled, true);
        add(PinAction::FillOff, "Fill", style_.filled, true);
        add(PinAction::ThicknessDown, "Decrease", false, true);
        addValue(std::to_wstring(static_cast<int>(style_.thickness + 0.5f)));
        add(PinAction::ThicknessUp, "Increase", false, true);
        break;
    case core::AnnotationTool::Pen:
    case core::AnnotationTool::Highlighter:
        add(PinAction::ThicknessDown, "Decrease", false, true);
        addValue(std::to_wstring(static_cast<int>(style_.thickness + 0.5f)));
        add(PinAction::ThicknessUp, "Increase", false, true);
        break;
    case core::AnnotationTool::Arrow:
        add(PinAction::ThicknessDown, "Decrease", false, true);
        addValue(std::to_wstring(static_cast<int>(style_.thickness + 0.5f)));
        add(PinAction::ThicknessUp, "Increase", false, true);
        add(PinAction::ArrowEnd, "ArrowEnd", style_.arrowHead == core::ArrowHeadMode::End,
            true);
        add(PinAction::ArrowBoth, "ArrowBoth", style_.arrowHead == core::ArrowHeadMode::Both,
            true);
        add(PinAction::ArrowNone, "ArrowLine", style_.arrowHead == core::ArrowHeadMode::None,
            true);
        break;
    case core::AnnotationTool::Text:
        add(PinAction::TextSizeDown, "Decrease", false, true);
        addValue(std::to_wstring(static_cast<int>(style_.textSize + 0.5f)));
        add(PinAction::TextSizeUp, "Increase", false, true);
        add(PinAction::Bold, "Bold", style_.bold, true);
        add(PinAction::Italic, "Italic", style_.italic, true);
        break;
    case core::AnnotationTool::Mosaic:
        add(PinAction::MosaicDown, "Decrease", false, true);
        addValue(std::to_wstring(style_.mosaicBlockPx));
        add(PinAction::MosaicUp, "Increase", false, true);
        break;
    case core::AnnotationTool::Eraser:
        add(PinAction::EraserDown, "Decrease", false, true);
        addValue(std::to_wstring(static_cast<int>(eraserSize_ + 0.5f)));
        add(PinAction::EraserUp, "Increase", false, true);
        break;
    case core::AnnotationTool::Step:
    default:
        break;
    }

    optionWidth_ = (x - originX) - gap + pad;
    optionBounds_ =
        RECT{ originX, originY, originX + optionWidth_, originY + optionHeight_ };
}

void PinWindow::DrawChrome(Gdiplus::Graphics& graphics) const {
    if (!annotating_) {
        return;
    }
    const auto drawPanel = [&](const RECT& bounds) {
        if (bounds.right <= bounds.left) {
            return;
        }
        Gdiplus::RectF rect(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                            static_cast<float>(bounds.right - bounds.left),
                            static_cast<float>(bounds.bottom - bounds.top));
        Gdiplus::GraphicsPath path;
        BuildRoundRect(path, rect, Scale(10.0f));
        Gdiplus::SolidBrush fill(Gdiplus::Color(0xFA, 0xF8, 0xF9, 0xFB));
        graphics.FillPath(&fill, &path);
        Gdiplus::Pen border(Gdiplus::Color(0x22, 0, 0, 0), Scale(1.0f));
        graphics.DrawPath(&border, &path);
    };
    drawPanel(toolbarBounds_);
    drawPanel(optionBounds_);

    const auto drawButtons = [&](const std::vector<PinButton>& buttons, int hovered) {
        for (std::size_t i = 0; i < buttons.size(); ++i) {
            const PinButton& button = buttons[i];
            if (button.separator) {
                Gdiplus::SolidBrush brush(Gdiplus::Color(0x1A, 0, 0, 0));
                graphics.FillRectangle(&brush, static_cast<INT>(button.rect.left),
                                       static_cast<INT>(button.rect.top),
                                       static_cast<INT>(button.rect.right - button.rect.left),
                                       static_cast<INT>(button.rect.bottom - button.rect.top));
                continue;
            }
            Gdiplus::RectF rect(static_cast<float>(button.rect.left),
                                static_cast<float>(button.rect.top),
                                static_cast<float>(button.rect.right - button.rect.left),
                                static_cast<float>(button.rect.bottom - button.rect.top));
            // 色板格不画整块底色：选中态是套在圆点上的正圆环（和覆盖层一致），
            // 给整格铺蓝色圆角块会让色点看起来是方的。
            if (!button.dot) {
                if (button.active) {
                    Gdiplus::GraphicsPath path;
                    BuildRoundRect(path, rect, Scale(7.0f));
                    Gdiplus::SolidBrush brush(ToGp(kAccent));
                    graphics.FillPath(&brush, &path);
                } else if (static_cast<int>(i) == hovered) {
                    Gdiplus::GraphicsPath path;
                    BuildRoundRect(path, rect, Scale(7.0f));
                    Gdiplus::SolidBrush brush(Gdiplus::Color(0x14, 0, 0, 0));
                    graphics.FillPath(&brush, &path);
                }
            }

            if (button.dot) {
                const float cx = (rect.X + rect.GetRight()) * 0.5f;
                const float cy = (rect.Y + rect.GetBottom()) * 0.5f;
                const float radius = Scale(6.0f);
                Gdiplus::SolidBrush dot(ToGp(button.dotColor));
                graphics.FillEllipse(&dot, cx - radius, cy - radius, radius * 2.0f,
                                     radius * 2.0f);
                if (button.active) {
                    // 与覆盖层一致：正圆环套在点上，别按整格画椭圆。
                    Gdiplus::Pen ring(ToGp(kAccent), Scale(1.5f));
                    graphics.DrawEllipse(&ring, cx - radius - Scale(3.0f),
                                         cy - radius - Scale(3.0f),
                                         (radius + Scale(3.0f)) * 2.0f,
                                         (radius + Scale(3.0f)) * 2.0f);
                }
                continue;
            }

            if (!button.text.empty()) {
                // 覆盖层二级栏的数值是 12 DIP 常规字重。
                Gdiplus::Font font(fontFamilyW_.c_str(), Scale(12.0f),
                                   Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
                Gdiplus::StringFormat format;
                format.SetAlignment(Gdiplus::StringAlignmentCenter);
                format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                Gdiplus::SolidBrush brush(ToGp(kInkNormal));
                graphics.DrawString(button.text.c_str(),
                                    static_cast<INT>(button.text.size()), &font, rect, &format,
                                    &brush);
                continue;
            }

            const float alpha = button.enabled ? 1.0f : 0.32f;
            const core::Color ink =
                button.active
                    ? core::Color{ 1.0f, 1.0f, 1.0f, alpha }
                    : core::Color{ kInkNormal.r, kInkNormal.g, kInkNormal.b, alpha };
            DrawIcon(graphics, button.rect, button.icon, ToGp(ink), Scale(20.0f));
        }
    };
    drawButtons(toolbar_, hoveredToolbar_);
    drawButtons(optionButtons_, hoveredOption_);
}

void PinWindow::DrawHud(Gdiplus::Graphics& graphics) const {
    if (!hudVisible_ || hud_.empty()) {
        return;
    }
    Gdiplus::Font font(fontFamilyW_.c_str(), Scale(11.0f), Gdiplus::FontStyleRegular,
                       Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::RectF bounds;
    graphics.MeasureString(hud_.c_str(), static_cast<INT>(hud_.size()), &font,
                           Gdiplus::PointF(0.0f, 0.0f), &format, &bounds);
    const float width = bounds.Width + Scale(20.0f);
    const float height = bounds.Height + Scale(10.0f);
    const float left = contentLeft_ + (displayWidth_ - width) * 0.5f;
    const float top = contentTop_ + displayHeight_ - height - Scale(10.0f);
    Gdiplus::RectF chip(left, top, width, height);
    Gdiplus::GraphicsPath path;
    BuildRoundRect(path, chip, Scale(7.0f));
    Gdiplus::SolidBrush fill(Gdiplus::Color(0xD9, 0x10, 0x14, 0x1C));
    graphics.FillPath(&fill, &path);
    Gdiplus::Pen border(Gdiplus::Color(0x2A, 255, 255, 255), 1.0f);
    graphics.DrawPath(&border, &path);
    Gdiplus::SolidBrush ink(Gdiplus::Color(255, 0xEA, 0xF0, 0xF8));
    graphics.DrawString(hud_.c_str(), static_cast<INT>(hud_.size()), &font,
                        Gdiplus::RectF(left, top, width, height), &format, &ink);
}

// ---- 渲染 ----

void PinWindow::RenderLayered(const std::vector<std::uint8_t>& pixels, int width, int height) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr &&
        pixels.size() >= static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4) {
        std::memcpy(bits, pixels.data(),
                    static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
        HGDIOBJ old = SelectObject(memory, dib);
        RECT windowRect{};
        GetWindowRect(static_cast<HWND>(window_), &windowRect);
        POINT destination{ windowRect.left, windowRect.top };
        SIZE size{ width, height };
        POINT source{ 0, 0 };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha =
            static_cast<BYTE>(std::clamp(opacity_, 0.15f, 1.0f) * 255.0f);
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(static_cast<HWND>(window_), screen, &destination, &size, memory,
                            &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void PinWindow::RenderAnnotations(Gdiplus::Graphics& graphics, float scale, float offsetX,
                                  float offsetY,
                                  const std::vector<core::Annotation>& items) const {
    core::FramePlan plan;
    core::AnnotationPlanOptions options;
    options.rasterizer = nullptr;   // 文字/序号数字由这里按 GDI+ 自己画
    options.fontFamily = options_.fontFamily;
    options.scale = scale;
    options.sizeScale = uiScale_;
    options.origin = core::Point{ offsetX, offsetY };
    core::AppendAnnotationPlan(items, options, plan);
    DrawPlan(graphics, plan);

    const auto toGp = [&](const core::Point& point) {
        return Gdiplus::PointF(offsetX + point.x * scale, offsetY + point.y * scale);
    };

    for (const core::Annotation& annotation : items) {
        if (annotation.tool == core::AnnotationTool::Text && !annotation.text.empty() &&
            !annotation.points.empty()) {
            const float sizePx = std::max(8.0f, annotation.style.textSize * uiScale_) * scale;
            const int fontStyle =
                (annotation.style.bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular) |
                (annotation.style.italic ? Gdiplus::FontStyleItalic
                                         : Gdiplus::FontStyleRegular);
            Gdiplus::Font font(fontFamilyW_.c_str(), sizePx, fontStyle, Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(ToGp(annotation.style.color));
            const std::wstring text = Widen(annotation.text);
            graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                                toGp(annotation.points[0]), nullptr, &brush);
        } else if (annotation.tool == core::AnnotationTool::Step &&
                   !annotation.points.empty() && !annotation.text.empty()) {
            const float size =
                std::max(8.0f, annotation.style.textSize * 0.72f) * uiScale_ * scale;
            const float radius =
                std::max(8.0f, annotation.style.textSize * 0.611f) * uiScale_ * scale;
            const Gdiplus::PointF center = toGp(annotation.points[0]);
            const core::Color ink = core::IsLightColor(annotation.style.color)
                                        ? core::Color{ 0.106f, 0.122f, 0.141f, 1.0f }
                                        : core::Color{ 1.0f, 1.0f, 1.0f, 1.0f };
            Gdiplus::Font font(fontFamilyW_.c_str(), size, Gdiplus::FontStyleBold,
                               Gdiplus::UnitPixel);
            Gdiplus::SolidBrush brush(ToGp(ink));
            Gdiplus::StringFormat format;
            format.SetAlignment(Gdiplus::StringAlignmentCenter);
            format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            const std::wstring text = Widen(annotation.text);
            graphics.DrawString(text.c_str(), static_cast<INT>(text.size()), &font,
                                Gdiplus::RectF(center.X - radius, center.Y - radius,
                                               radius * 2.0f, radius * 2.0f),
                                &format, &brush);
        } else if (annotation.tool == core::AnnotationTool::Mosaic &&
                   annotation.points.size() >= 2 && annotation.mosaicTileCpu != nullptr &&
                   !annotation.mosaicTileCpu->empty()) {
            const core::Rect rect =
                core::NormalizeRect(annotation.points[0], annotation.points[1]);
            const int tileWidth = static_cast<int>(annotation.mosaicTileSize.width + 0.5f);
            const int tileHeight = static_cast<int>(annotation.mosaicTileSize.height + 0.5f);
            if (tileWidth <= 0 || tileHeight <= 0) {
                continue;
            }
            Gdiplus::Bitmap tile(tileWidth, tileHeight, tileWidth * 4, PixelFormat32bppPARGB,
                                 const_cast<BYTE*>(annotation.mosaicTileCpu->data()));
            graphics.DrawImage(&tile,
                               Gdiplus::RectF(offsetX + rect.left * scale,
                                              offsetY + rect.top * scale, rect.Width() * scale,
                                              rect.Height() * scale));
        }
    }
}

void PinWindow::Render() {
    if (window_ == nullptr || content_.empty() || !EnsureGdiplus()) {
        return;
    }
    UpdateLayout();

    // 1. 内容 + 标注，先画进一张显示尺寸的位图。
    std::vector<std::uint8_t> display(
        static_cast<std::size_t>(displayWidth_) * displayHeight_ * 4, 0);
    {
        Gdiplus::Bitmap canvas(displayWidth_, displayHeight_, displayWidth_ * 4,
                               PixelFormat32bppPARGB, display.data());
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::Bitmap source(contentWidth_, contentHeight_, contentWidth_ * 4,
                               PixelFormat32bppRGB, content_.data());
        graphics.DrawImage(&source,
                           Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(displayWidth_),
                                          static_cast<float>(displayHeight_)));
        if (!document_.Items().empty() || document_.Drawing()) {
            std::vector<core::Annotation> items = document_.Items();
            if (document_.Drawing() && !document_.Active().points.empty()) {
                items.push_back(document_.Active());
            }
            RenderAnnotations(graphics, zoom_, 0.0f, 0.0f, items);
        }
    }

    // 2. 内容 + 描边 + 阴影，拼进分层窗口缓冲。贴图是矩形（1.x 也不切圆角），
    //    外描边关掉时 border/glow 都是 0，这一圈完全不画。
    std::vector<std::uint8_t> layered(
        static_cast<std::size_t>(windowWidth_) * windowHeight_ * 4, 0);
    const std::uint8_t borderB = 0xFF;
    const std::uint8_t borderG = 0xFF;
    const std::uint8_t borderR = 0xFF;
    const std::uint8_t borderA = 0xFF;

    for (int y = 0; y < windowHeight_; ++y) {
        for (int x = 0; x < windowWidth_; ++x) {
            const int contentX = x - contentLeft_;
            const int contentY = y - contentTop_;
            const bool insideContent = contentX >= 0 && contentX < displayWidth_ &&
                                       contentY >= 0 && contentY < displayHeight_;
            const int outsideX = contentX < 0
                                     ? -contentX
                                     : (contentX >= displayWidth_ ? contentX - displayWidth_ + 1
                                                                  : 0);
            const int outsideY = contentY < 0
                                     ? -contentY
                                     : (contentY >= displayHeight_ ? contentY - displayHeight_ + 1
                                                                   : 0);
            const int distance = std::max(outsideX, outsideY);

            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            std::uint8_t a = 0;
            if (insideContent) {
                const std::size_t sourceIndex =
                    (static_cast<std::size_t>(contentY) * displayWidth_ + contentX) * 4;
                b = display[sourceIndex + 0];
                g = display[sourceIndex + 1];
                r = display[sourceIndex + 2];
                a = display[sourceIndex + 3];
            } else if (border_ > 0 && distance <= border_) {
                b = borderB;
                g = borderG;
                r = borderR;
                a = borderA;
            } else if (glow_ > 0 && distance <= border_ + glow_) {
                const float t = 1.0f - static_cast<float>(distance - border_) /
                                           static_cast<float>(glow_);
                a = static_cast<std::uint8_t>(72.0f * t * t);
            }

            const std::size_t destination =
                (static_cast<std::size_t>(y) * windowWidth_ + x) * 4;
            layered[destination + 0] = static_cast<std::uint8_t>(b * a / 255);
            layered[destination + 1] = static_cast<std::uint8_t>(g * a / 255);
            layered[destination + 2] = static_cast<std::uint8_t>(r * a / 255);
            layered[destination + 3] = a;
        }
    }

    // 3. 标注工具栏、HUD。
    {
        Gdiplus::Bitmap canvas(windowWidth_, windowHeight_, windowWidth_ * 4,
                               PixelFormat32bppPARGB, layered.data());
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
        DrawChrome(graphics);
        DrawHud(graphics);
    }

    RenderLayered(layered, windowWidth_, windowHeight_);
}

void PinWindow::ShowHud(const std::wstring& text) {
    hud_ = text;
    hudVisible_ = true;
    if (window_ != nullptr) {
        SetTimer(static_cast<HWND>(window_), kHudTimer, 1250, nullptr);
    }
    Render();
}

void PinWindow::HideHud() {
    hudVisible_ = false;
    Render();
}

// ---- 绘制标注 ----

void PinWindow::PaintMosaicAt(core::Point local) {
    const float radius =
        std::max(10.0f, static_cast<float>(style_.mosaicBlockPx) * 2.0f) * uiScale_;
    int left = static_cast<int>(std::lround(local.x - radius));
    int top = static_cast<int>(std::lround(local.y - radius));
    int right = static_cast<int>(std::lround(local.x + radius));
    int bottom = static_cast<int>(std::lround(local.y + radius));
    left = std::clamp(left, 0, contentWidth_);
    top = std::clamp(top, 0, contentHeight_);
    right = std::clamp(right, 0, contentWidth_);
    bottom = std::clamp(bottom, 0, contentHeight_);
    const int regionWidth = right - left;
    const int regionHeight = bottom - top;
    if (regionWidth <= 0 || regionHeight <= 0) {
        return;
    }

    // 取样 = 底图 + 这一笔之前的所有标注（1.x 的 CreateCompositedTile 同语义）。
    std::vector<std::uint8_t> region(
        static_cast<std::size_t>(regionWidth) * regionHeight * 4, 0);
    {
        Gdiplus::Bitmap canvas(regionWidth, regionHeight, regionWidth * 4,
                               PixelFormat32bppPARGB, region.data());
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeNearestNeighbor);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        Gdiplus::Bitmap source(contentWidth_, contentHeight_, contentWidth_ * 4,
                               PixelFormat32bppRGB, content_.data());
        graphics.DrawImage(&source,
                           Gdiplus::RectF(0.0f, 0.0f, static_cast<float>(contentWidth_),
                                          static_cast<float>(contentHeight_)));
        RenderAnnotations(graphics, 1.0f, -static_cast<float>(left),
                          -static_cast<float>(top), document_.Items());
    }

    const core::MosaicTile tile = core::DownsampleRegion(
        core::ImageRef{ region.data(), regionWidth, regionHeight, regionWidth * 4 },
        core::IntRect{ 0, 0, regionWidth, regionHeight },
        core::MosaicBlockPixels(core::annotation_defaults::kMosaicBlockPx, 1.0f));
    if (tile.width <= 0 || tile.height <= 0) {
        return;
    }

    document_.Begin(core::AnnotationTool::Mosaic, style_);
    document_.AddPoint(core::Point{ static_cast<float>(left), static_cast<float>(top) });
    document_.SetEnd(core::Point{ static_cast<float>(right), static_cast<float>(bottom) });
    document_.Commit();
    if (!document_.Items().empty()) {
        core::Annotation& annotation = const_cast<core::Annotation&>(document_.Items().back());
        annotation.mosaicTileCpu =
            std::make_shared<const std::vector<std::uint8_t>>(tile.bgra);
        annotation.mosaicTileSize =
            core::Size{ static_cast<float>(tile.width), static_cast<float>(tile.height) };
    }
}

void PinWindow::CommitLiveStroke() {
    if (!document_.Drawing()) {
        drawing_ = false;
        return;
    }
    document_.Commit();
    drawing_ = false;
    UpdateLayout();
    ShowHud(L"已添加标注 · Ctrl+Z 撤销");
}

void PinWindow::FinishStroke() {
    if (mosaicPainting_) {
        mosaicPainting_ = false;
        UpdateLayout();
        Render();
        return;
    }
    if (drawing_) {
        CommitLiveStroke();
    }
}

// ---- 文字 ----

LRESULT CALLBACK PinWindow::EditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_CTLCOLOREDIT) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(0xE9, 0xED, 0xF4));
        SetBkColor(dc, RGB(0x10, 0x14, 0x1C));
        return reinterpret_cast<LRESULT>(g_editorBrush);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK PinWindow::EditorEditProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    if (message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            if (g_editorOwner != nullptr && g_editorOwner->window_ != nullptr) {
                PostMessageW(static_cast<HWND>(g_editorOwner->window_), kMsgCommitEditor, 0, 0);
            }
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            if (g_editorOwner != nullptr && g_editorOwner->window_ != nullptr) {
                PostMessageW(static_cast<HWND>(g_editorOwner->window_), kMsgCancelEditor, 0, 0);
            }
            return 0;
        }
    }
    if (message == WM_KILLFOCUS) {
        if (g_editorOwner != nullptr && g_editorOwner->window_ != nullptr) {
            PostMessageW(static_cast<HWND>(g_editorOwner->window_), kMsgCommitEditor, 0, 0);
        }
    }
    return CallWindowProcW(g_oldEditProc, window, message, wParam, lParam);
}

void PinWindow::BeginTextEditor(core::Point local) {
    if (!EnsureEditorClass() || editorWindow_ != nullptr) {
        return;
    }
    textPoint_ = local;
    const int screenX = contentScreenX_ + static_cast<int>(std::lround(local.x * zoom_));
    const int screenY = contentScreenY_ + static_cast<int>(std::lround(local.y * zoom_));
    const int editorWidth =
        std::max(ScaleInt(160.0f), static_cast<int>(style_.textSize * uiScale_) * 10);
    const int editorHeight = static_cast<int>(style_.textSize * uiScale_) + ScaleInt(12.0f);

    editorWindow_ =
        CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kEditorClass, L"", WS_POPUP, screenX,
                        screenY, editorWidth, editorHeight, nullptr, nullptr,
                        GetModuleHandleW(nullptr), nullptr);
    if (editorWindow_ == nullptr) {
        return;
    }
    editControl_ = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 2, 2,
        editorWidth - ScaleInt(4.0f), editorHeight - ScaleInt(4.0f),
        static_cast<HWND>(editorWindow_), reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr),
        nullptr);
    if (editControl_ == nullptr) {
        DestroyWindow(static_cast<HWND>(editorWindow_));
        editorWindow_ = nullptr;
        return;
    }

    g_editorOwner = this;
    if (g_editorBrush == nullptr) {
        g_editorBrush = CreateSolidBrush(RGB(0x10, 0x14, 0x1C));
    }
    if (g_editorFont != nullptr) {
        DeleteObject(g_editorFont);
    }
    g_editorFont = CreateFontW(
        -static_cast<int>(std::max(8.0f, style_.textSize * uiScale_)), 0, 0, 0,
        style_.bold ? FW_BOLD : FW_NORMAL, style_.italic ? TRUE : FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, fontFamilyW_.c_str());
    SendMessageW(static_cast<HWND>(editControl_), WM_SETFONT,
                 reinterpret_cast<WPARAM>(g_editorFont), TRUE);
    g_oldEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        static_cast<HWND>(editControl_), GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(&PinWindow::EditorEditProc)));
    ShowWindow(static_cast<HWND>(editorWindow_), SW_SHOW);
    startingEditor_ = true;
    SetFocus(static_cast<HWND>(editControl_));
    startingEditor_ = false;
}

void PinWindow::CommitTextEditor() {
    if (editorWindow_ == nullptr || editControl_ == nullptr) {
        return;
    }
    wchar_t buffer[512] = {};
    GetWindowTextW(static_cast<HWND>(editControl_), buffer,
                   static_cast<int>(std::size(buffer)));
    std::wstring text(buffer);
    while (!text.empty() && (text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    while (!text.empty() && (text.front() == L' ' || text.front() == L'\t')) {
        text.erase(text.begin());
    }
    DestroyTextEditor();
    if (window_ != nullptr) {
        SetFocus(static_cast<HWND>(window_));
    }
    if (text.empty()) {
        Render();
        return;
    }

    core::Rect bounds{ textPoint_.x, textPoint_.y, textPoint_.x, textPoint_.y };
    {
        BYTE pixel[4] = {};
        Gdiplus::Bitmap measure(1, 1, 4, PixelFormat32bppPARGB, pixel);
        Gdiplus::Graphics graphics(&measure);
        const int fontStyle =
            (style_.bold ? Gdiplus::FontStyleBold : Gdiplus::FontStyleRegular) |
            (style_.italic ? Gdiplus::FontStyleItalic : Gdiplus::FontStyleRegular);
        Gdiplus::Font font(fontFamilyW_.c_str(), std::max(8.0f, style_.textSize * uiScale_),
                           fontStyle, Gdiplus::UnitPixel);
        Gdiplus::RectF measured;
        graphics.MeasureString(text.c_str(), static_cast<INT>(text.size()), &font,
                               Gdiplus::PointF(0.0f, 0.0f), &measured);
        bounds.right = bounds.left + measured.Width;
        bounds.bottom = bounds.top + measured.Height;
    }

    core::Annotation& annotation = document_.Begin(core::AnnotationTool::Text, style_);
    annotation.points = { textPoint_ };
    annotation.text = Narrow(text);
    annotation.textBounds = bounds;
    document_.Commit();
    UpdateLayout();
    Render();
}

void PinWindow::CancelTextEditor() {
    DestroyTextEditor();
    if (window_ != nullptr) {
        SetFocus(static_cast<HWND>(window_));
    }
    Render();
}

void PinWindow::DestroyTextEditor() {
    if (editControl_ != nullptr) {
        DestroyWindow(static_cast<HWND>(editControl_));
        editControl_ = nullptr;
    }
    if (editorWindow_ != nullptr) {
        DestroyWindow(static_cast<HWND>(editorWindow_));
        editorWindow_ = nullptr;
    }
    if (g_editorOwner == this) {
        g_editorOwner = nullptr;
    }
}

// ---- 交互 ----

void PinWindow::OnMouseMove(HWND window, POINT client) {
    if (dragging_) {
        ContinueWindowDrag(window);
        return;
    }

    if (annotating_) {
        const int toolbar = ToolbarButtonAt(client);
        const int option = OptionButtonAt(client);
        UpdateHover(toolbar, false);
        UpdateHover(option, true);

        if (drawing_ || mosaicPainting_) {
            const core::Point local{ (static_cast<float>(client.x) - contentLeft_) / zoom_,
                                     (static_cast<float>(client.y) - contentTop_) / zoom_ };
            if (tool_ == core::AnnotationTool::Eraser) {
                document_.EraseAt(local, eraserSize_ * 0.5f * uiScale_);
                Render();
                return;
            }
            if (mosaicPainting_) {
                const float dx = local.x - lastMosaicPoint_.x;
                const float dy = local.y - lastMosaicPoint_.y;
                const float brush =
                    std::max(10.0f, static_cast<float>(style_.mosaicBlockPx) * 2.0f) * uiScale_;
                if (dx * dx + dy * dy >= brush * brush * 0.25f) {
                    const float distance = std::sqrt(dx * dx + dy * dy);
                    const int steps =
                        std::max(1, static_cast<int>(distance / (brush * 0.4f)));
                    for (int step = 1; step <= steps; ++step) {
                        const float t = static_cast<float>(step) / static_cast<float>(steps);
                        PaintMosaicAt(core::Point{ lastMosaicPoint_.x + dx * t,
                                                   lastMosaicPoint_.y + dy * t });
                    }
                    lastMosaicPoint_ = local;
                    Render();
                }
                return;
            }
            if (tool_ == core::AnnotationTool::Pen ||
                tool_ == core::AnnotationTool::Highlighter) {
                document_.AddPoint(local, 0.8f);
            } else if (tool_ == core::AnnotationTool::Arrow &&
                       (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                const core::Point start = document_.Active().points.empty()
                                              ? local
                                              : document_.Active().points.front();
                document_.SetEnd(core::SnapToAngle(start, local));
            } else if ((tool_ == core::AnnotationTool::Rectangle ||
                        tool_ == core::AnnotationTool::Ellipse) &&
                       (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
                const core::Point start = document_.Active().points.empty()
                                              ? local
                                              : document_.Active().points.front();
                document_.SetEnd(core::ConstrainSquare(start, local));
            } else {
                document_.SetEnd(local);
            }
            Render();
            return;
        }
        return;
    }

    if (!trackingLeave_) {
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = window;
        TrackMouseEvent(&track);
        trackingLeave_ = true;
    }
}

void PinWindow::OnLeftButtonDown(HWND window, POINT client) {
    if (menuOpen_) {
        HideMenu();
        return;
    }
    if (editorWindow_ != nullptr) {
        CommitTextEditor();
        return;
    }

    if (annotating_) {
        const int toolbar = ToolbarButtonAt(client);
        if (toolbar >= 0) {
            if (toolbar_[static_cast<std::size_t>(toolbar)].enabled) {
                HandleToolbar(toolbar_[static_cast<std::size_t>(toolbar)]);
            }
            return;
        }
        const int option = OptionButtonAt(client);
        if (option >= 0) {
            const PinButton& button = optionButtons_[static_cast<std::size_t>(option)];
            if (button.action == PinAction::Color) {
                style_.color = core::annotation_defaults::PaletteColor(button.payload);
                UpdateLayout();
                Render();
            } else if (button.enabled) {
                HandleOption(button.action);
            }
            return;
        }
        const core::Point local{ (static_cast<float>(client.x) - contentLeft_) / zoom_,
                                 (static_cast<float>(client.y) - contentTop_) / zoom_ };
        if (local.x < 0.0f || local.y < 0.0f ||
            local.x >= static_cast<float>(contentWidth_) ||
            local.y >= static_cast<float>(contentHeight_)) {
            return;
        }
        SetForegroundWindow(window);
        SetFocus(window);
        HideHud();
        if (tool_ == core::AnnotationTool::Eraser) {
            drawing_ = true;
            document_.EraseAt(local, eraserSize_ * 0.5f * uiScale_);
            SetCapture(window);
            Render();
            return;
        }
        if (tool_ == core::AnnotationTool::Text) {
            BeginTextEditor(local);
            return;
        }
        if (tool_ == core::AnnotationTool::Step) {
            document_.Begin(core::AnnotationTool::Step, style_);
            document_.AddPoint(local);
            document_.Commit();
            UpdateLayout();
            ShowHud(L"序号 " + std::to_wstring(document_.NextStepNumber() - 1));
            return;
        }
        if (tool_ == core::AnnotationTool::Mosaic) {
            mosaicPainting_ = true;
            lastMosaicPoint_ = local;
            PaintMosaicAt(local);
            SetCapture(window);
            Render();
            return;
        }
        document_.Begin(tool_, style_);
        document_.AddPoint(local);
        drawing_ = true;
        SetCapture(window);
        Render();
        return;
    }

    StartWindowDrag(window);
}

void PinWindow::OnLeftButtonUp(HWND window) {
    if (dragging_) {
        EndWindowDrag(window);
        return;
    }
    if (drawing_ || mosaicPainting_) {
        FinishStroke();
        ReleaseCapture();
    }
}

void PinWindow::StartWindowDrag(HWND window) {
    GetCursorPos(&dragOrigin_);
    RECT rect{};
    GetWindowRect(window, &rect);
    dragWindow_ = POINT{ rect.left, rect.top };
    dragging_ = true;
    SetCapture(window);
}

void PinWindow::ContinueWindowDrag(HWND window) {
    POINT cursor{};
    GetCursorPos(&cursor);
    const int dx = cursor.x - dragOrigin_.x;
    const int dy = cursor.y - dragOrigin_.y;
    const int x = dragWindow_.x + dx;
    const int y = dragWindow_.y + dy;
    SetWindowPos(window, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    contentScreenX_ = x + contentLeft_;
    contentScreenY_ = y + contentTop_;
}

void PinWindow::EndWindowDrag(HWND window) {
    dragging_ = false;
    ReleaseCapture();
    if (annotating_) {
        UpdateLayout();
    }
    (void)window;
}

void PinWindow::OnMouseWheel(int delta) {
    if (delta == 0) {
        return;
    }
    if (annotating_) {
        AdjustBrush(delta > 0 ? 1 : -1);
        return;
    }
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        opacity_ = std::clamp(opacity_ + (delta > 0 ? 0.08f : -0.08f), 0.15f, 1.0f);
        wchar_t buffer[64] = {};
        std::swprintf(buffer, 64, L"不透明度 %d%%",
                      static_cast<int>(opacity_ * 100.0f + 0.5f));
        ShowHud(buffer);
        return;
    }

    const float factor = delta > 0 ? 1.12f : 1.0f / 1.12f;
    const float minZoom =
        std::max(0.12f, std::max(40.0f / static_cast<float>(contentWidth_),
                                 40.0f / static_cast<float>(contentHeight_)));
    const float target = std::clamp(zoom_ * factor, minZoom, 8.0f);
    if (std::fabs(target - zoom_) < 0.0005f) {
        return;
    }

    POINT cursor{};
    const bool hasCursor = GetCursorPos(&cursor) != FALSE;
    const float fractionX =
        hasCursor ? std::clamp(static_cast<float>(cursor.x - contentScreenX_) /
                                   static_cast<float>(displayWidth_),
                               0.0f, 1.0f)
                  : 0.5f;
    const float fractionY =
        hasCursor ? std::clamp(static_cast<float>(cursor.y - contentScreenY_) /
                                   static_cast<float>(displayHeight_),
                               0.0f, 1.0f)
                  : 0.5f;
    zoom_ = target;
    const int newWidth = std::max(40, static_cast<int>(std::lround(contentWidth_ * zoom_)));
    const int newHeight = std::max(40, static_cast<int>(std::lround(contentHeight_ * zoom_)));
    if (hasCursor) {
        contentScreenX_ = cursor.x - static_cast<int>(std::lround(fractionX * newWidth));
        contentScreenY_ = cursor.y - static_cast<int>(std::lround(fractionY * newHeight));
    } else {
        contentScreenX_ -= (newWidth - displayWidth_) / 2;
        contentScreenY_ -= (newHeight - displayHeight_) / 2;
    }
    wchar_t buffer[64] = {};
    std::swprintf(buffer, 64, L"缩放 %d%%", static_cast<int>(zoom_ * 100.0f + 0.5f));
    ShowHud(buffer);
}

void PinWindow::ResetSizeAndOpacity() {
    zoom_ = 1.0f;
    opacity_ = 1.0f;
    const int width = std::max(24, contentWidth_);
    const int height = std::max(24, contentHeight_);
    const int centerX = contentScreenX_ + displayWidth_ / 2;
    const int centerY = contentScreenY_ + displayHeight_ / 2;
    contentScreenX_ = centerX - width / 2;
    contentScreenY_ = centerY - height / 2;
    ShowHud(L"已恢复实际大小");
}

void PinWindow::OnKeyDown(WPARAM key) {
    if (menuOpen_) {
        if (key == VK_ESCAPE) {
            HideMenu();
        }
        return;
    }
    if (editorWindow_ != nullptr) {
        return;
    }
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    if (key == VK_SPACE) {
        SetAnnotationMode(!annotating_);
        return;
    }
    if (key == VK_ESCAPE) {
        if (annotating_) {
            SetAnnotationMode(false);
        } else {
            Close();
        }
        return;
    }

    if (!annotating_) {
        if (key == 'O' && !control) {
            RequestOcr();
        } else if (key == 'C' && control) {
            CopyImage();
        } else if (key == 'S' && control) {
            SaveImage();
        } else if ((key == '0' || key == VK_NUMPAD0) && control) {
            ResetSizeAndOpacity();
        }
        return;
    }

    if (control) {
        if (key == 'Z') {
            if (shift) {
                if (document_.Redo()) {
                    UpdateLayout();
                    Render();
                    ShowHud(L"已重做");
                }
            } else if (document_.Undo()) {
                UpdateLayout();
                Render();
                ShowHud(L"已撤销");
            }
        } else if (key == 'Y') {
            if (document_.Redo()) {
                UpdateLayout();
                Render();
                ShowHud(L"已重做");
            }
        } else if (key == 'B' && tool_ == core::AnnotationTool::Text) {
            style_.bold = !style_.bold;
            UpdateLayout();
            Render();
            ShowHud(style_.bold ? L"加粗" : L"取消加粗");
        } else if (key == 'I' && tool_ == core::AnnotationTool::Text) {
            style_.italic = !style_.italic;
            UpdateLayout();
            Render();
            ShowHud(style_.italic ? L"斜体" : L"取消斜体");
        }
        return;
    }

    switch (key) {
    case 'R':
    case 'E':
        shapeTool_ =
            key == 'R' ? core::AnnotationTool::Rectangle : core::AnnotationTool::Ellipse;
        style_.filled = shift;
        SetTool(shapeTool_);
        return;
    case 'A':
        SetTool(core::AnnotationTool::Arrow);
        return;
    case 'P':
        SetTool(core::AnnotationTool::Pen);
        return;
    case 'T':
        SetTool(core::AnnotationTool::Text);
        return;
    case 'H':
        SetTool(core::AnnotationTool::Highlighter);
        return;
    case 'M':
        SetTool(core::AnnotationTool::Mosaic);
        return;
    case 'N':
        SetTool(core::AnnotationTool::Step);
        return;
    case 'X':
        SetTool(core::AnnotationTool::Eraser);
        return;
    case 'F':
        style_.filled = !style_.filled;
        UpdateLayout();
        Render();
        ShowHud(style_.filled ? L"实心填充" : L"空心");
        return;
    case VK_OEM_4:
        AdjustBrush(-1);
        return;
    case VK_OEM_6:
        AdjustBrush(1);
        return;
    default:
        return;
    }
}

void PinWindow::OnSetCursor() {
    if (window_ == nullptr) {
        return;
    }
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(static_cast<HWND>(window_), &cursor);
    const wchar_t* name = IDC_SIZEALL;
    if (menuOpen_) {
        name = IDC_ARROW;
    } else if (annotating_) {
        name = (ToolbarButtonAt(cursor) >= 0 || OptionButtonAt(cursor) >= 0) ? IDC_HAND
                                                                            : IDC_CROSS;
    }
    SetCursor(LoadCursorW(nullptr, name));
}

// ---- 工具栏动作 ----

void PinWindow::HandleToolbar(const PinButton& button) {
    switch (button.action) {
    case PinAction::ToolShape:
        SetTool(shapeTool_);
        return;
    case PinAction::ToolArrow:
        SetTool(core::AnnotationTool::Arrow);
        return;
    case PinAction::ToolPen:
        SetTool(core::AnnotationTool::Pen);
        return;
    case PinAction::ToolHighlighter:
        SetTool(core::AnnotationTool::Highlighter);
        return;
    case PinAction::ToolEraser:
        SetTool(core::AnnotationTool::Eraser);
        return;
    case PinAction::ToolText:
        SetTool(core::AnnotationTool::Text);
        return;
    case PinAction::ToolMosaic:
        SetTool(core::AnnotationTool::Mosaic);
        return;
    case PinAction::ToolStep:
        SetTool(core::AnnotationTool::Step);
        return;
    case PinAction::Undo:
        if (document_.Undo()) {
            UpdateLayout();
            Render();
            ShowHud(L"已撤销");
        }
        return;
    case PinAction::Redo:
        if (document_.Redo()) {
            UpdateLayout();
            Render();
            ShowHud(L"已重做");
        }
        return;
    case PinAction::Clear:
        if (!document_.Items().empty()) {
            document_.Clear();
            UpdateLayout();
            Render();
            ShowHud(L"已清空标注");
        }
        return;
    case PinAction::CloseEdit:
    case PinAction::Done:
        SetAnnotationMode(false);
        return;
    default:
        return;
    }
}

void PinWindow::AdjustBrush(int direction) {
    wchar_t buffer[64] = {};
    switch (tool_) {
    case core::AnnotationTool::Text:
        style_.textSize = std::clamp(style_.textSize + direction * 2.0f,
                                     core::annotation_defaults::kMinTextSize,
                                     core::annotation_defaults::kMaxTextSize);
        std::swprintf(buffer, 64, L"字号 %d", static_cast<int>(style_.textSize + 0.5f));
        break;
    case core::AnnotationTool::Highlighter:
    case core::AnnotationTool::Pen:
    case core::AnnotationTool::Rectangle:
    case core::AnnotationTool::Ellipse:
    case core::AnnotationTool::Arrow:
        if (tool_ == core::AnnotationTool::Highlighter) {
            style_.thickness = std::clamp(style_.thickness + direction * 2.0f, 6.0f, 40.0f);
        } else {
            style_.thickness = std::clamp(style_.thickness + direction,
                                          core::annotation_defaults::kMinThickness,
                                          core::annotation_defaults::kMaxThickness);
        }
        std::swprintf(buffer, 64, L"粗细 %d", static_cast<int>(style_.thickness + 0.5f));
        break;
    case core::AnnotationTool::Mosaic:
        style_.mosaicBlockPx = std::clamp(style_.mosaicBlockPx + direction * 2, 6, 30);
        std::swprintf(buffer, 64, L"马赛克 %d", style_.mosaicBlockPx);
        break;
    case core::AnnotationTool::Eraser:
        eraserSize_ = std::clamp(eraserSize_ + direction * 8.0f, 8.0f, 120.0f);
        std::swprintf(buffer, 64, L"橡皮 %d", static_cast<int>(eraserSize_ + 0.5f));
        break;
    case core::AnnotationTool::Step:
        return;
    default:
        return;
    }
    UpdateLayout();
    ShowHud(buffer);
}

void PinWindow::HandleOption(PinAction action) {
    switch (action) {
    case PinAction::ShapeRectangle:
        shapeTool_ = core::AnnotationTool::Rectangle;
        tool_ = shapeTool_;
        break;
    case PinAction::ShapeEllipse:
        shapeTool_ = core::AnnotationTool::Ellipse;
        tool_ = shapeTool_;
        break;
    case PinAction::FillOn:
        style_.filled = false;
        break;
    case PinAction::FillOff:
        style_.filled = true;
        break;
    case PinAction::ThicknessDown:
    case PinAction::ThicknessUp:
        AdjustBrush(action == PinAction::ThicknessUp ? 1 : -1);
        return;
    case PinAction::TextSizeDown:
    case PinAction::TextSizeUp:
        AdjustBrush(action == PinAction::TextSizeUp ? 1 : -1);
        return;
    case PinAction::MosaicDown:
    case PinAction::MosaicUp:
        AdjustBrush(action == PinAction::MosaicUp ? 1 : -1);
        return;
    case PinAction::EraserDown:
    case PinAction::EraserUp:
        AdjustBrush(action == PinAction::EraserUp ? 1 : -1);
        return;
    case PinAction::Bold:
        style_.bold = !style_.bold;
        break;
    case PinAction::Italic:
        style_.italic = !style_.italic;
        break;
    case PinAction::ArrowEnd:
        style_.arrowHead = core::ArrowHeadMode::End;
        break;
    case PinAction::ArrowBoth:
        style_.arrowHead = core::ArrowHeadMode::Both;
        break;
    case PinAction::ArrowNone:
        style_.arrowHead = core::ArrowHeadMode::None;
        break;
    default:
        return;
    }
    UpdateLayout();
    Render();
}

// ---- 导出 ----

std::vector<std::uint8_t> PinWindow::ComposeImage(int* width, int* height) {
    *width = contentWidth_;
    *height = contentHeight_;
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(contentWidth_) * contentHeight_ * 4, 0);
    if (!EnsureGdiplus()) {
        return pixels;
    }
    Gdiplus::Bitmap canvas(contentWidth_, contentHeight_, contentWidth_ * 4,
                           PixelFormat32bppPARGB, pixels.data());
    Gdiplus::Graphics graphics(&canvas);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::Bitmap source(contentWidth_, contentHeight_, contentWidth_ * 4,
                           PixelFormat32bppRGB, content_.data());
    graphics.DrawImage(&source, Gdiplus::RectF(0.0f, 0.0f,
                                               static_cast<float>(contentWidth_),
                                               static_cast<float>(contentHeight_)));
    if (!document_.Items().empty()) {
        RenderAnnotations(graphics, 1.0f, 0.0f, 0.0f, document_.Items());
    }
    return pixels;
}

void PinWindow::CopyImage() {
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> pixels = ComposeImage(&width, &height);
    if (pixels.empty()) {
        return;
    }
    const bool copied = halocap::platform::win32::CopyBgraToClipboard(
        static_cast<HWND>(window_), pixels.data(), width, height, width * 4);
    ShowHud(copied ? (document_.Items().empty() ? L"已复制到剪贴板" : L"已复制（含标注）")
                   : L"复制失败，剪贴板可能被占用");
}

void PinWindow::SaveImage() {
    int width = 0;
    int height = 0;
    const std::vector<std::uint8_t> pixels = ComposeImage(&width, &height);
    if (pixels.empty()) {
        return;
    }

    wchar_t path[MAX_PATH] = {};
    const std::wstring name = TimestampName() + (options_.jpeg ? L".jpg" : L".png");
    std::wcsncpy(path, name.c_str(), std::size(path) - 1);
    const std::wstring directory = SaveDirectory();
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = static_cast<HWND>(window_);
    dialog.lpstrFilter = options_.jpeg ? L"JPEG 图片\0*.jpg\0所有文件\0*.*\0"
                                       : L"PNG 图片\0*.png\0所有文件\0*.*\0";
    dialog.lpstrFile = path;
    dialog.nMaxFile = static_cast<DWORD>(std::size(path));
    dialog.lpstrInitialDir = directory.empty() ? nullptr : directory.c_str();
    dialog.lpstrDefExt = options_.jpeg ? L"jpg" : L"png";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) {
        return;
    }

    const std::wstring target(path);
    const std::wstring parent = std::filesystem::path(target).parent_path().wstring();
    if (!parent.empty()) {
        std::error_code error;
        std::filesystem::create_directories(std::filesystem::path(parent), error);
    }
    const bool ok =
        options_.jpeg ? halocap::platform::win32::SaveBgraJpeg(target, pixels.data(), width,
                                                               height, width * 4, 90)
                      : halocap::platform::win32::SaveBgraPng(target, pixels.data(), width,
                                                              height, width * 4);
    ShowHud(ok ? L"图片已保存" : L"保存失败");
}

void PinWindow::RequestOcr() {
    if (!options_.ocrEnabled || !onOcr_ || content_.empty()) {
        return;
    }
    // 1.x 的 OcrImage 是原始图像：标注不参与识别。
    onOcr_(content_.data(), contentWidth_, contentHeight_, contentScreenX_, contentScreenY_);
    ShowHud(L"正在识别文字…");
}

// ---- 右键菜单 ----

void PinWindow::BuildMenuItems() {
    menuItems_.clear();
    menuItems_.push_back(MenuItem{ kMenuAnnotate, L"标注", L"Space", false, false, annotating_,
                                   true, RECT{} });
    menuItems_.push_back(MenuItem{ kMenuOcr, L"识别文字", L"O", false, false, false,
                                   options_.ocrEnabled && onOcr_ != nullptr, RECT{} });
    menuItems_.push_back(MenuItem{ 0, L"", L"", true, false, false, true, RECT{} });
    menuItems_.push_back(
        MenuItem{ kMenuCopy, L"复制图片", L"Ctrl+C", false, false, false, true, RECT{} });
    menuItems_.push_back(
        MenuItem{ kMenuSave, L"保存图片…", L"Ctrl+S", false, false, false, true, RECT{} });
    menuItems_.push_back(MenuItem{ 0, L"", L"", true, false, false, true, RECT{} });
    menuItems_.push_back(
        MenuItem{ kMenuReset, L"实际大小", L"Ctrl+0", false, false, false, true, RECT{} });
    menuItems_.push_back(MenuItem{ kMenuOutline, L"外描边", L"", false, true, options_.outline,
                                   true, RECT{} });
    menuItems_.push_back(MenuItem{ 0, L"", L"", true, false, false, true, RECT{} });
    menuItems_.push_back(
        MenuItem{ kMenuClose, L"关闭", L"Esc", false, false, false, true, RECT{} });
    menuItems_.push_back(MenuItem{ kMenuCloseAll, L"全部关闭", L"", false, false, false, true,
                                   RECT{} });
}

void PinWindow::ShowMenu(POINT screen) {
    if (window_ == nullptr || menuWindow_ != nullptr || !EnsureMenuClass() || !EnsureGdiplus()) {
        return;
    }
    BuildMenuItems();

    const int pad = ScaleInt(6.0f);
    const int itemPadX = ScaleInt(10.0f);
    const int itemHeight = ScaleInt(30.0f);
    const int separatorHeight = ScaleInt(9.0f);
    float maxLabel = 0.0f;
    float maxShortcut = 0.0f;
    for (const MenuItem& item : menuItems_) {
        if (item.separator) {
            continue;
        }
        maxLabel = std::max(maxLabel, MeasureTextWidth(item.label, fontFamilyW_, Scale(12.5f),
                                                       Gdiplus::FontStyleRegular));
        if (!item.shortcut.empty()) {
            maxShortcut = std::max(maxShortcut,
                                   MeasureTextWidth(item.shortcut, fontFamilyW_, Scale(10.5f),
                                                    Gdiplus::FontStyleRegular));
        }
    }
    const int checkWidth = ScaleInt(20.0f);
    menuWidth_ = std::max(
        ScaleInt(190.0f),
        static_cast<int>(itemPadX * 2 + maxLabel + Scale(20.0f) + maxShortcut + checkWidth) +
            pad * 2);

    int y = pad;
    for (MenuItem& item : menuItems_) {
        const int height = item.separator ? separatorHeight : itemHeight;
        item.rect = RECT{ pad, y, menuWidth_ - pad, y + height };
        y += height;
    }
    menuHeight_ = y + pad;

    menuWindow_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMenuClass,
        L"HaloCap pin menu", WS_POPUP, screen.x, screen.y, menuWidth_, menuHeight_, nullptr,
        nullptr, GetModuleHandleW(nullptr), this);
    if (menuWindow_ == nullptr) {
        return;
    }
    g_menuOwner = this;
    menuOpen_ = true;
    hoveredMenuIndex_ = -1;

    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &info);
    const int workLeft = static_cast<int>(info.rcWork.left);
    const int workTop = static_cast<int>(info.rcWork.top);
    const int workRight = static_cast<int>(info.rcWork.right);
    const int workBottom = static_cast<int>(info.rcWork.bottom);
    const int x = std::clamp(static_cast<int>(screen.x), workLeft,
                             std::max(workLeft, workRight - menuWidth_));
    int top = static_cast<int>(screen.y);
    if (top + menuHeight_ > workBottom) {
        top = screen.y - menuHeight_;
    }
    top = std::clamp(top, workTop, std::max(workTop, workBottom - menuHeight_));
    menuScreenX_ = x;
    menuScreenY_ = top;
    SetWindowPos(static_cast<HWND>(menuWindow_), HWND_TOPMOST, x, top, menuWidth_, menuHeight_,
                 SWP_NOACTIVATE);

    SetCapture(static_cast<HWND>(menuWindow_));
    ShowWindow(static_cast<HWND>(menuWindow_), SW_SHOWNOACTIVATE);
    RenderMenu();
}

void PinWindow::HideMenu() {
    if (menuWindow_ != nullptr) {
        if (GetCapture() == static_cast<HWND>(menuWindow_)) {
            ReleaseCapture();
        }
        DestroyWindow(static_cast<HWND>(menuWindow_));
        menuWindow_ = nullptr;
    }
    if (g_menuOwner == this) {
        g_menuOwner = nullptr;
    }
    menuOpen_ = false;
    hoveredMenuIndex_ = -1;
}

void PinWindow::RenderMenu() {
    if (menuWindow_ == nullptr) {
        return;
    }
    std::vector<std::uint8_t> pixels(
        static_cast<std::size_t>(menuWidth_) * menuHeight_ * 4, 0);
    {
        Gdiplus::Bitmap canvas(menuWidth_, menuHeight_, menuWidth_ * 4,
                               PixelFormat32bppPARGB, pixels.data());
        Gdiplus::Graphics graphics(&canvas);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

        Gdiplus::GraphicsPath panel;
        BuildRoundRect(panel,
                       Gdiplus::RectF(0.5f, 0.5f, static_cast<float>(menuWidth_) - 1.0f,
                                      static_cast<float>(menuHeight_) - 1.0f),
                       Scale(12.0f));
        Gdiplus::SolidBrush background(Gdiplus::Color(0xFC, 255, 255, 255));
        graphics.FillPath(&background, &panel);
        Gdiplus::Pen border(Gdiplus::Color(0x25, 0, 0, 0), 1.0f);
        graphics.DrawPath(&border, &panel);

        Gdiplus::Font font(fontFamilyW_.c_str(), Scale(12.5f), Gdiplus::FontStyleRegular,
                           Gdiplus::UnitPixel);
        Gdiplus::Font shortcutFont(fontFamilyW_.c_str(), Scale(10.5f),
                                   Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
        for (std::size_t index = 0; index < menuItems_.size(); ++index) {
            const MenuItem& item = menuItems_[index];
            Gdiplus::RectF rect(static_cast<float>(item.rect.left),
                                static_cast<float>(item.rect.top),
                                static_cast<float>(item.rect.right - item.rect.left),
                                static_cast<float>(item.rect.bottom - item.rect.top));
            if (item.separator) {
                Gdiplus::SolidBrush line(Gdiplus::Color(0x14, 0, 0, 0));
                graphics.FillRectangle(&line, rect.X + Scale(2.0f),
                                       rect.Y + rect.Height * 0.5f, rect.Width - Scale(4.0f),
                                       1.0f);
                continue;
            }
            if (static_cast<int>(index) == hoveredMenuIndex_ && item.enabled) {
                Gdiplus::GraphicsPath hoverPath;
                BuildRoundRect(hoverPath, rect, Scale(7.0f));
                Gdiplus::SolidBrush hover(Gdiplus::Color(0xF0, 0xF5, 0xFC));
                graphics.FillPath(&hover, &hoverPath);
            }
            const float alpha = item.enabled ? 1.0f : 0.36f;
            Gdiplus::SolidBrush ink(
                Gdiplus::Color(static_cast<BYTE>(255 * alpha), 0x1D, 0x1D, 0x1F));
            Gdiplus::StringFormat left;
            left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
            graphics.DrawString(item.label.c_str(), static_cast<INT>(item.label.size()), &font,
                                Gdiplus::RectF(rect.X + Scale(10.0f), rect.Y,
                                               rect.Width - Scale(20.0f), rect.Height),
                                &left, &ink);

            float rightEdge = rect.GetRight() - Scale(10.0f);
            if (item.checkable && item.checked) {
                Gdiplus::Pen check(Gdiplus::Color(0xFF, 0x00, 0x7A, 0xFF), Scale(1.7f));
                check.SetStartCap(Gdiplus::LineCapRound);
                check.SetEndCap(Gdiplus::LineCapRound);
                const float cx = rightEdge - Scale(7.0f);
                const float cy = rect.Y + rect.Height * 0.5f;
                graphics.DrawLine(&check, cx - Scale(5.0f), cy - Scale(0.5f),
                                  cx - Scale(2.0f), cy + Scale(2.5f));
                graphics.DrawLine(&check, cx - Scale(2.0f), cy + Scale(2.5f),
                                  cx + Scale(5.0f), cy - Scale(4.0f));
                rightEdge -= Scale(18.0f);
            }
            if (!item.shortcut.empty()) {
                Gdiplus::SolidBrush hint(
                    Gdiplus::Color(static_cast<BYTE>(255 * alpha), 0x8E, 0x8E, 0x93));
                Gdiplus::StringFormat right;
                right.SetAlignment(Gdiplus::StringAlignmentFar);
                right.SetLineAlignment(Gdiplus::StringAlignmentCenter);
                right.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
                graphics.DrawString(item.shortcut.c_str(),
                                    static_cast<INT>(item.shortcut.size()), &shortcutFont,
                                    Gdiplus::RectF(rect.X, rect.Y, rightEdge - rect.X,
                                                   rect.Height),
                                    &right, &hint);
            }
        }
    }

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = menuWidth_;
    info.bmiHeader.biHeight = -menuHeight_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib != nullptr && bits != nullptr) {
        std::memcpy(bits, pixels.data(), pixels.size());
        HGDIOBJ old = SelectObject(memory, dib);
        RECT windowRect{};
        GetWindowRect(static_cast<HWND>(menuWindow_), &windowRect);
        POINT destination{ windowRect.left, windowRect.top };
        SIZE size{ menuWidth_, menuHeight_ };
        POINT source{ 0, 0 };
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.SourceConstantAlpha = 255;
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(static_cast<HWND>(menuWindow_), screen, &destination, &size,
                            memory, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        DeleteObject(dib);
    }
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK PinWindow::MenuProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    PinWindow* self = g_menuOwner;
    if (self != nullptr && self->menuWindow_ == window) {
        return self->HandleMenu(window, message, wParam, lParam);
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT PinWindow::HandleMenu(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int hovered = -1;
        for (std::size_t index = 0; index < menuItems_.size(); ++index) {
            const MenuItem& item = menuItems_[index];
            if (!item.separator && item.enabled && point.x >= item.rect.left &&
                point.x < item.rect.right && point.y >= item.rect.top &&
                point.y < item.rect.bottom) {
                hovered = static_cast<int>(index);
                break;
            }
        }
        if (hovered != hoveredMenuIndex_) {
            hoveredMenuIndex_ = hovered;
            RenderMenu();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        int chosen = 0;
        for (const MenuItem& item : menuItems_) {
            if (!item.separator && item.enabled && point.x >= item.rect.left &&
                point.x < item.rect.right && point.y >= item.rect.top &&
                point.y < item.rect.bottom) {
                chosen = item.id;
                break;
            }
        }
        HideMenu();
        if (chosen != 0) {
            ExecuteMenu(chosen);
        }
        return 0;
    }
    case WM_RBUTTONDOWN:
        HideMenu();
        return 0;
    case WM_MOUSEWHEEL:
        return 0;
    default:
        break;
    }
    (void)window;
    (void)wParam;
    return DefWindowProcW(window, message, wParam, lParam);
}

void PinWindow::ExecuteMenu(int id) {
    switch (id) {
    case kMenuAnnotate:
        SetAnnotationMode(!annotating_);
        return;
    case kMenuOcr:
        RequestOcr();
        return;
    case kMenuCopy:
        CopyImage();
        return;
    case kMenuSave:
        SaveImage();
        return;
    case kMenuReset:
        ResetSizeAndOpacity();
        return;
    case kMenuOutline:
        options_.outline = !options_.outline;
        Render();
        if (onOutlineChanged_) {
            onOutlineChanged_(options_.outline);
        }
        return;
    case kMenuClose:
        Close();
        return;
    case kMenuCloseAll:
        if (onCloseAll_) {
            onCloseAll_();
        }
        return;
    default:
        return;
    }
}

// ---- 消息 ----

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
    case WM_MOUSEMOVE:
        OnMouseMove(window, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        return 0;
    case WM_LBUTTONDOWN:
        OnLeftButtonDown(window, POINT{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
        return 0;
    case WM_LBUTTONDBLCLK:
        Close();
        return 0;
    case WM_LBUTTONUP:
        OnLeftButtonUp(window);
        return 0;
    case WM_RBUTTONDOWN: {
        // 鼠标消息给的是客户区坐标，菜单要贴光标放，先换成屏幕坐标。
        POINT screen{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ClientToScreen(window, &screen);
        ShowMenu(screen);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT cursor{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(window, &cursor);
        if (annotating_ && IsOverChrome(cursor)) {
            return 0;
        }
        OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }
    case WM_KEYDOWN:
        OnKeyDown(wParam);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            OnSetCursor();
            return TRUE;
        }
        break;
    case WM_TIMER:
        if (wParam == kHudTimer) {
            KillTimer(window, kHudTimer);
            hudVisible_ = false;
            Render();
            return 0;
        }
        break;
    case kMsgCommitEditor:
        CommitTextEditor();
        return 0;
    case kMsgCancelEditor:
        CancelTextEditor();
        return 0;
    case WM_KILLFOCUS:
        if (startingEditor_) {
            return 0;
        }
        if (menuOpen_) {
            HideMenu();
        }
        if (editorWindow_ != nullptr) {
            PostMessageW(window, kMsgCommitEditor, 0, 0);
        }
        return 0;
    case WM_DESTROY:
        if (window_ == window) {
            window_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}
