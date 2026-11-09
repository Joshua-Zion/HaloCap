#include "screenshot_overlay.h"

#include "image_export.h"
#include "desktop_grab.h"
#include "mosaic.h"
#include "toolbar_icons.gen.h"
#include "window_at_point.h"
#include "path.h"

#include <windows.h>

#include <windowsx.h>

#include <shlobj.h>

#include <algorithm>
#include <filesystem>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>

namespace halocap::app {

namespace {

using core::AnnotationPlanOptions;
using core::AnnotationTool;
using core::Color;
using core::IntRect;
using core::Point;
using core::Rect;
using platform::win32::D2dRenderBackend;
using platform::win32::D2dFontRasterizer;
using platform::win32::Win32DesktopDuplication;
using platform::win32::Win32SurfaceWindow;

constexpr wchar_t kEditorClass[] = L"HaloCapTextEditor";

// 覆盖层同一时刻只有一个：键盘钩子与 EDIT 子类化都靠这两只静态指针。
ScreenshotOverlay* g_overlay = nullptr;
HHOOK g_keyHook = nullptr;
ScreenshotOverlay* g_editorOwner = nullptr;
WNDPROC g_oldEditProc = nullptr;

// WM_APP 区间的自定义消息：键盘钩子把它们投给覆盖层窗口。
constexpr UINT kMsgEscape = WM_APP + 1;
constexpr UINT kMsgUndo = WM_APP + 2;
constexpr UINT kMsgRedo = WM_APP + 3;
constexpr UINT kMsgCopy = WM_APP + 4;
constexpr UINT kMsgSave = WM_APP + 5;
constexpr UINT kMsgTool = WM_APP + 6;
constexpr UINT kMsgDelete = WM_APP + 8;
constexpr UINT kMsgSelect = WM_APP + 9;
constexpr UINT kMsgFill = WM_APP + 10;
constexpr UINT kMsgAnnotate = WM_APP + 11;   // 空格：切换标注态
constexpr UINT kMsgNudge = WM_APP + 12;      // 方向键微调：wParam = 方向，lParam = 步长
constexpr UINT kMsgCopyColor = WM_APP + 13;  // C：复制光标下的颜色（wParam = 1 用 rgb()）
constexpr UINT kMsgPin = WM_APP + 14;        // Ctrl+T
constexpr UINT kMsgTextStyle = WM_APP + 15;  // Ctrl+B / Ctrl+I（wParam 1=粗 2=斜）
constexpr UINT kMsgOption = WM_APP + 16;     // [ / ] 之类的选项步进，wParam = Action

// 1.x 的 AnnotationToolButtonStyle：30×30、两侧各 1 px 外边距、圆角 7；工具栏 padding 4。
constexpr int kToolbarButton = 30;
constexpr int kToolbarGap = 2;
constexpr int kToolbarPadding = 4;
constexpr int kToolbarHeight = kToolbarButton + kToolbarPadding * 2;   // 38
constexpr int kSeparatorWidth = 1;
constexpr int kSeparatorMargin = 4;
constexpr int kOptionButton = 26;
constexpr int kOptionHeight = kOptionButton + 8;   // 34
constexpr int kColorWidth = 22;
constexpr int kHandleSize = 7;

const AnnotationTool kTools[] = {
    AnnotationTool::Pen,       AnnotationTool::Rectangle, AnnotationTool::Ellipse,
    AnnotationTool::Arrow,     AnnotationTool::Text,      AnnotationTool::Highlighter,
    AnnotationTool::Mosaic,    AnnotationTool::Step,      AnnotationTool::Eraser,
};

// 复制文本到剪贴板（颜色复制用；选区的图片复制走平台层的 DIB 路径）。
bool CopyText(const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return false;
    }
    if (void* target = GlobalLock(memory); target != nullptr) {
        std::memcpy(target, text.c_str(), bytes);
        GlobalUnlock(memory);
    } else {
        GlobalFree(memory);
        return false;
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

// 1.x 图标的路径只解析一次；之后每帧只做坐标变换和几何缓存命中。
struct ParsedIcon {
    std::vector<core::PathCommand> first;
    std::vector<core::PathCommand> second;
    bool evenOddFirst = false;
    bool evenOddSecond = false;
    bool valid = false;
};

const ParsedIcon& ParsedIconFor(const char* name) {
    static std::unordered_map<std::string, ParsedIcon> cache;
    const auto found = cache.find(name);
    if (found != cache.end()) {
        return found->second;
    }
    ParsedIcon parsed;
    const IconPathData* data = FindIconPath(name);
    if (data != nullptr) {
        parsed.valid = core::ParsePathData(data->first, &parsed.first, &parsed.evenOddFirst);
        if (data->second != nullptr) {
            core::ParsePathData(data->second, &parsed.second, &parsed.evenOddSecond);
        }
    }
    return cache.emplace(name, std::move(parsed)).first->second;
}

const char* IconNameFor(ScreenshotOverlay::ToolbarIcon icon) {
    switch (icon) {
    case ScreenshotOverlay::ToolbarIcon::Select: return "Select";
    case ScreenshotOverlay::ToolbarIcon::Shape: return "Shape";
    case ScreenshotOverlay::ToolbarIcon::Arrow: return "Arrow";
    case ScreenshotOverlay::ToolbarIcon::Pen: return "Pen";
    case ScreenshotOverlay::ToolbarIcon::Highlighter: return "Highlighter";
    case ScreenshotOverlay::ToolbarIcon::Eraser: return "Eraser";
    case ScreenshotOverlay::ToolbarIcon::Text: return "Text";
    case ScreenshotOverlay::ToolbarIcon::Mosaic: return "Mosaic";
    case ScreenshotOverlay::ToolbarIcon::Step: return "Step";
    case ScreenshotOverlay::ToolbarIcon::Undo: return "Undo";
    case ScreenshotOverlay::ToolbarIcon::Redo: return "Redo";
    case ScreenshotOverlay::ToolbarIcon::Clear: return "Clear";
    case ScreenshotOverlay::ToolbarIcon::Save: return "Save";
    case ScreenshotOverlay::ToolbarIcon::Copy: return "Copy";
    case ScreenshotOverlay::ToolbarIcon::Pin: return "Pin";
    case ScreenshotOverlay::ToolbarIcon::Ocr: return "Ocr";
    case ScreenshotOverlay::ToolbarIcon::Close: return "Close";
    case ScreenshotOverlay::ToolbarIcon::Done: return "Done";
    case ScreenshotOverlay::ToolbarIcon::Rectangle: return "Rectangle";
    case ScreenshotOverlay::ToolbarIcon::Ellipse: return "Ellipse";
    case ScreenshotOverlay::ToolbarIcon::Outline: return "Outline";
    case ScreenshotOverlay::ToolbarIcon::Fill: return "Fill";
    case ScreenshotOverlay::ToolbarIcon::ArrowBoth: return "ArrowBoth";
    case ScreenshotOverlay::ToolbarIcon::ArrowLine: return "ArrowLine";
    case ScreenshotOverlay::ToolbarIcon::Bold: return "Bold";
    case ScreenshotOverlay::ToolbarIcon::Italic: return "Italic";
    case ScreenshotOverlay::ToolbarIcon::Decrease: return "Decrease";
    case ScreenshotOverlay::ToolbarIcon::Increase: return "Increase";
    case ScreenshotOverlay::ToolbarIcon::None:
    case ScreenshotOverlay::ToolbarIcon::Separator:
        break;
    }
    return nullptr;
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
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
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
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

bool EnsureDirectory(const std::wstring& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
    std::error_code check;
    return std::filesystem::is_directory(std::filesystem::path(path), check);
}

// 鼠标消息给的是客户区坐标；窗口左上角对应屏幕 (windowX_, windowY_)。
POINT ToScreen(POINT client, int windowX, int windowY) {
    return POINT{ client.x + windowX, client.y + windowY };
}

}  // namespace

ScreenshotOverlay::ScreenshotOverlay() = default;

ScreenshotOverlay::~ScreenshotOverlay() { Close(); }

bool ScreenshotOverlay::EnsureEditorClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &ScreenshotOverlay::EditorProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.lpszClassName = kEditorClass;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool ScreenshotOverlay::Open(const ScreenshotOverlayOptions& options) {
    Close();

    options_ = options;
    style_ = core::AnnotationStyle{};
    style_.color = core::annotation_defaults::PaletteColor(0);
    style_.thickness = core::annotation_defaults::kThickness;
    tool_ = AnnotationTool::Pen;
    document_.Clear();
    selection_.Cancel();
    phase_ = Phase::Selecting;
    annotating_ = false;
    hoverWindow_ = nullptr;
    hoverRect_ = IntRect{};
    hoverProbed_ = false;
    lastProbeMs_ = 0;
    lastClickMs_ = 0;
    dirty_ = true;
    pendingClose_ = false;
    errorKind_ = "ok";

    backend_ = std::make_unique<D2dRenderBackend>();
    if (!backend_->Initialize()) {
        errorKind_ = "backend";
        Close();
        return false;
    }

    rasterizer_ = std::make_unique<D2dFontRasterizer>(*backend_);
    if (!rasterizer_->Initialize()) {
        errorKind_ = "rasterizer";
        Close();
        return false;
    }

    // 截图走 GDI 抓屏：本机的 DuplicateOutput 回读是全黑的（见 desktop_grab.h）。
    // 这样还有一个好处：录制占用复制权时截图照样能开。
    // 抓的是整块虚拟桌面：1.x 的选区容器就是所有显示器的并集，拖动可以跨屏。
    platform::win32::DesktopImage desktop;
    if (!platform::win32::GrabVirtualDesktop(desktop)) {
        errorKind_ = "grab";
        Close();
        return false;
    }

    const platform::win32::DesktopBounds desktopBounds =
        platform::win32::VirtualDesktopBounds();
    windowX_ = desktopBounds.left;
    windowY_ = desktopBounds.top;
    screenWidth_ = desktop.width & ~1;
    screenHeight_ = desktop.height & ~1;
    if (screenWidth_ < 2 || screenHeight_ < 2) {
        errorKind_ = "invalid";
        Close();
        return false;
    }
    // 选区不能拖到画面外（之前只有 DebugSelect 设过边界，正常打开时是可以拖出去的）。
    selection_.SetLimit(Rect{ static_cast<float>(windowX_), static_cast<float>(windowY_),
                              static_cast<float>(windowX_ + screenWidth_),
                              static_cast<float>(windowY_ + screenHeight_) });

    // 偶数尺寸：裁掉右侧/底部 1px 时行距会变，重排一次。
    if (desktop.width != screenWidth_ || desktop.height != screenHeight_) {
        std::vector<std::uint8_t> packed(
            static_cast<std::size_t>(screenWidth_) * screenHeight_ * 4, 0);
        for (int y = 0; y < screenHeight_; ++y) {
            std::memcpy(packed.data() + static_cast<std::size_t>(y) * screenWidth_ * 4,
                        desktop.bgra.data() + static_cast<std::size_t>(y) * desktop.width * 4,
                        static_cast<std::size_t>(screenWidth_) * 4);
        }
        baseBgra_ = std::move(packed);
    } else {
        baseBgra_ = std::move(desktop.bgra);
    }

    if (!backend_->Resize(screenWidth_, screenHeight_)) {
        errorKind_ = "resize";
        Close();
        return false;
    }

    capturedHandle_ = backend_->RegisterImage(core::ImageRef{
        baseBgra_.data(), screenWidth_, screenHeight_, screenWidth_ * 4 });
    if (capturedHandle_ == 0) {
        errorKind_ = "register";
        Close();
        return false;
    }

    window_ = std::make_unique<Win32SurfaceWindow>();
    core::WindowBounds bounds{ windowX_, windowY_, screenWidth_, screenHeight_ };
    core::WindowOptions windowOptions;
    windowOptions.visible = true;
    windowOptions.focusable = false;   // 不抢焦点：键盘走低级钩子
    windowOptions.clickThrough = false;
    windowOptions.topMost = true;
    if (!window_->Create(bounds, windowOptions)) {
        errorKind_ = "window";
        Close();
        return false;
    }
    // UI 尺寸按 DIP 给，实际绘制是物理像素：先记住这台显示器的缩放。
    {
        const UINT dpi = GetDpiForWindow(static_cast<HWND>(window_->Handle()));
        uiScale_ = static_cast<float>(dpi >= 48 ? dpi : 96) / 96.0f;
        if (uiScale_ < 1.0f) {
            uiScale_ = 1.0f;
        }
    }

    window_->SetMessageHandler([this](HWND window, UINT message, WPARAM wParam, LPARAM lParam) {        return HandleMessage(window, message, wParam, lParam);
    });
    if (!backend_->AttachWindow(window_->Handle(), screenWidth_, screenHeight_)) {
        errorKind_ = "attach";
        Close();
        return false;
    }

    InstallKeyHook();
    g_overlay = this;
    active_ = true;

    // 1.x 覆盖层一出现就按光标位置探测一次：用快捷键打开时光标通常没动过，
    // 只靠 WM_MOUSEMOVE 会迟迟不显示窗口候选（看起来就像「自动识别没了」）。
    if (GetCursorPos(&lastMouse_)) {
        mouseInside_ = true;
        UpdateHoverCandidate(lastMouse_);
        UpdateCursor(lastMouse_);
    }
    SetTimer(static_cast<HWND>(window_->Handle()), 5, 150, nullptr);
    Render();
    return true;
}

void ScreenshotOverlay::Close() {
    if (g_overlay == this) {
        g_overlay = nullptr;
    }
    UninstallKeyHook();
    DestroyTextEditor();

    if (window_ != nullptr) {
        KillTimer(static_cast<HWND>(window_->Handle()), 5);
        window_->SetVisible(false);
    }
    if (backend_ != nullptr) {
        backend_->DetachWindow();
    }
    if (backend_ != nullptr && capturedHandle_ != 0) {
        backend_->ReleaseImage(capturedHandle_);
    }
    capturedHandle_ = 0;

    baseBgra_.clear();
    baseBgra_.shrink_to_fit();

    window_.reset();
    rasterizer_.reset();
    backend_.reset();

    active_ = false;
    dirty_ = true;
    pendingClose_ = false;
    phase_ = Phase::Selecting;
    selecting_ = false;
    resizing_ = false;
    drawing_ = false;
}

void ScreenshotOverlay::Pump() {
    if (!active_) {
        return;
    }
    if (pendingClose_) {
        Close();
        return;
    }
    if (dirty_) {
        Render();
    }
}

// ---- 消息 ----

LRESULT ScreenshotOverlay::HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_SETCURSOR: {
        // 光标形状跟着命中的东西走（把手方向 / 选区内部 / 工具栏），不是永远十字。
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(window, &point);
        UpdateCursor(ToScreen(point, windowX_, windowY_));
        return TRUE;
    }

    case WM_TIMER:
        if (wParam == 1) {
            StepHoverAnimation();
            return 0;
        }
        if (wParam == 2) {
            // 颜色复制的提示。
            if (GetTickCount64() >= toastUntilMs_) {
                toast_.clear();
                KillTimer(window, 2);
            }
            dirty_ = true;
            return 0;
        }
        if (wParam == 3) {
            StepToolbarAnimation();
            return 0;
        }
        if (wParam == 4) {
            // 悬停满 600 ms：把提示文字画出来。
            KillTimer(window, 4);
            dirty_ = true;
            return 0;
        }
        if (wParam == 5) {
            // 1.x 的空闲探测（150 ms）：覆盖层出现时/光标被别处挪动时，
            // 未必有 WM_MOUSEMOVE 送到这里，候选窗口照样要跟着光标更新。
            if (active_ && phase_ == Phase::Selecting && selection_.Bounds().IsEmpty() &&
                !selecting_ && !annotating_ && !movingSelection_ && !resizing_ && !drawing_ &&
                !mosaicPainting_) {
                POINT cursor{};
                if (GetCursorPos(&cursor) &&
                    (cursor.x != lastMouse_.x || cursor.y != lastMouse_.y)) {
                    OnMouseMove(cursor);
                } else if (hoverRect_.IsEmpty()) {
                    UpdateHoverCandidate(cursor);
                    dirty_ = true;
                }
            }
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        // 1.x：标注态直接滚滚轮就调当前工具的参数（不用按 Ctrl）。
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (!annotating_ || delta == 0) {
            return 0;
        }
        const float direction = delta > 0 ? 1.0f : -1.0f;
        switch (tool_) {
        case AnnotationTool::Pen:
            style_.thickness = std::clamp(style_.thickness + direction, 1.0f, 24.0f);
            break;
        case AnnotationTool::Highlighter:
            style_.thickness = std::clamp(style_.thickness + direction * 2.0f, 6.0f, 40.0f);
            break;
        case AnnotationTool::Text:
            style_.textSize = std::clamp(style_.textSize + direction * 2.0f, 10.0f, 48.0f);
            break;
        case AnnotationTool::Rectangle:
        case AnnotationTool::Ellipse:
        case AnnotationTool::Arrow:
            style_.thickness = std::clamp(style_.thickness + direction, 1.0f, 24.0f);
            break;
        case AnnotationTool::Step:
            style_.textSize = std::clamp(style_.textSize + direction * 2.0f, 10.0f, 48.0f);
            break;
        case AnnotationTool::Mosaic:
            style_.mosaicBlockPx =
                std::clamp(style_.mosaicBlockPx + static_cast<int>(direction) * 2, 6, 30);
            break;
        case AnnotationTool::Eraser:
            eraserSize_ = std::clamp(eraserSize_ + direction * 8.0f, 8.0f, 120.0f);
            break;
        default:
            return 0;
        }
        dirty_ = true;
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnMouseMove(ToScreen(point, windowX_, windowY_));
        return 0;
    }

    case WM_MOUSELEAVE:
        // 不跟踪离开事件，鼠标滑出窗口后放大镜会一直留在画面上。
        mouseInside_ = false;
        dirty_ = true;
        return 0;

    case WM_RBUTTONDOWN: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnRightButtonDown(ToScreen(point, windowX_, windowY_));
        return 0;
    }

    case WM_LBUTTONDOWN: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnLeftButtonDown(ToScreen(point, windowX_, windowY_));
        return 0;
    }

    case WM_LBUTTONUP: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnLeftButtonUp(ToScreen(point, windowX_, windowY_));
        return 0;
    }

    case WM_PAINT: {
        ValidateRect(window, nullptr);
        return 0;
    }

    case kMsgEscape:
        if (selectedAnnotation_ >= 0) {
            selectedAnnotation_ = -1;
            dirty_ = true;
        } else {
            Close();
        }
        return 0;

    case kMsgDelete:
        if (selectedAnnotation_ >= 0 &&
            document_.RemoveAnnotation(static_cast<std::size_t>(selectedAnnotation_))) {
            selectedAnnotation_ = -1;
            dirty_ = true;
        }
        return 0;

    case kMsgSelect:
        annotating_ = false;
        selectedAnnotation_ = -1;
        dirty_ = true;
        return 0;

    case kMsgAnnotate:
        if (!selection_.Bounds().IsEmpty()) {
            annotating_ = !annotating_;
            selectedAnnotation_ = -1;
            dirty_ = true;
        }
        return 0;

    case kMsgNudge: {
        if (annotating_ || selection_.Bounds().IsEmpty()) {
            return 0;
        }
        int dx = 0;
        int dy = 0;
        switch (static_cast<int>(wParam)) {
        case VK_LEFT: dx = -1; break;
        case VK_RIGHT: dx = 1; break;
        case VK_UP: dy = -1; break;
        case VK_DOWN: dy = 1; break;
        default: break;
        }
        NudgeSelection(dx * static_cast<int>(lParam), dy * static_cast<int>(lParam));
        return 0;
    }

    case kMsgFill:
        if (tool_ == AnnotationTool::Rectangle || tool_ == AnnotationTool::Ellipse) {
            style_.filled = !style_.filled;
            dirty_ = true;
        }
        return 0;

    case kMsgUndo:
        if (document_.Undo()) {
            dirty_ = true;
        }
        return 0;

    case kMsgRedo:
        if (document_.Redo()) {
            dirty_ = true;
        }
        return 0;

    case kMsgCopy:
        if (options_.mode == OverlayMode::Ocr) {
            RunOcr();
            Close();
        } else if (CopySelection()) {
            Close();
        }
        return 0;

    case kMsgSave:
        if (SaveSelection(nullptr)) {
            Close();
        }
        return 0;

    case kMsgTool:
        if (!selection_.Bounds().IsEmpty() && wParam < std::size(kTools)) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            // Shift+R / Shift+E：1.x 直接画实心形状。
            if (lParam != 0) {
                style_.filled = true;
            }
            SetTool(kTools[wParam]);
            if (kTools[wParam] == AnnotationTool::Rectangle ||
                kTools[wParam] == AnnotationTool::Ellipse) {
                shapeTool_ = kTools[wParam];
            }
        }
        return 0;

    case kMsgCopyColor:
        CopyColorUnderCursor(wParam != 0);
        return 0;

    case kMsgPin:
        if (PinSelection()) {
            pendingClose_ = true;
            dirty_ = true;
        }
        return 0;

    case kMsgTextStyle:
        if (tool_ == AnnotationTool::Text) {
            if (wParam == 1) {
                style_.bold = !style_.bold;
            } else {
                style_.italic = !style_.italic;
            }
            dirty_ = true;
        }
        return 0;

    case kMsgOption:
        HandleOption(static_cast<Action>(static_cast<std::int32_t>(wParam)));
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void ScreenshotOverlay::OnMouseMove(POINT point) {
    lastMouse_ = point;
    if (!mouseInside_) {
        mouseInside_ = true;
        TRACKMOUSEEVENT track{ sizeof(TRACKMOUSEEVENT), TME_LEAVE,
                               static_cast<HWND>(window_->Handle()), 0 };
        TrackMouseEvent(&track);
    }

    int hovered = -1;
    // 工具栏/选项栏的矩形是**客户区坐标**，鼠标消息给的是屏幕坐标——多屏时窗口原点不是 (0,0)，
    // 不换算就会永远命不中（现象就是「工具栏点不了、还会拖出新选区」）。
    const POINT client{ point.x - windowX_, point.y - windowY_ };
    if (toolbarVisible_) {
        for (std::size_t i = 0; i < toolbar_.size(); ++i) {
            const RECT& rect = toolbar_[i].rect;
            if (client.x >= rect.left && client.x < rect.right && client.y >= rect.top &&
                client.y < rect.bottom) {
                hovered = static_cast<int>(i);
                break;
            }
        }
    }
    if (hovered != hoveredToolbar_) {
        hoveredToolbar_ = hovered;
        hoverSinceMs_ = GetTickCount64();
        hoverAnimating_ = true;
        SetTimer(static_cast<HWND>(window_->Handle()), 1, 16, nullptr);
        if (hovered >= 0) {
            SetTimer(static_cast<HWND>(window_->Handle()), 4, 620, nullptr);
        }
        dirty_ = true;
    }

    if (movingAnnotation_) {
        if (selectedAnnotation_ >= 0) {
            const float dx = static_cast<float>(point.x - moveOrigin_.x);
            const float dy = static_cast<float>(point.y - moveOrigin_.y);
            if (dx != 0.0f || dy != 0.0f) {
                document_.MoveAnnotation(static_cast<std::size_t>(selectedAnnotation_),
                                         core::Point{ dx, dy });
                moveOrigin_ = point;
                dirty_ = true;
            }
        }
        return;
    }
    if (movingSelection_) {
        const float dx = static_cast<float>(point.x - lastPoint_.x);
        const float dy = static_cast<float>(point.y - lastPoint_.y);
        if (dx != 0.0f || dy != 0.0f) {
            selection_.MoveBy(core::Point{ dx, dy });
            lastPoint_ = point;
            dirty_ = true;
        }
        return;
    }

    if (selecting_) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        selection_.Update(Point{ static_cast<float>(point.x), static_cast<float>(point.y) }, shift);
        dirty_ = true;
        return;
    }

    if (resizing_) {
        selection_.ResizeTo(resizeHandle_,
                            Point{ static_cast<float>(point.x), static_cast<float>(point.y) });
        dirty_ = true;
        return;
    }

    if (mosaicPainting_) {
        const Rect bounds = selection_.Bounds();
        const Point local{ static_cast<float>(point.x) - bounds.left,
                           static_cast<float>(point.y) - bounds.top };
        const float dx = local.x - lastMosaicPoint_.x;
        const float dy = local.y - lastMosaicPoint_.y;
        const float brush = MosaicBrushRadius();
        if (dx * dx + dy * dy >= brush * brush * 0.25f) {
            // 鼠标画快了，两次消息之间可能跳出一大段：按不超过半个笔刷的间距补块，
            // 否则方块之间会露缝（就是「断触」）。
            const float distance = std::sqrt(dx * dx + dy * dy);
            const int steps = std::max(1, static_cast<int>(distance / (brush * 0.4f)));
            for (int step = 1; step <= steps; ++step) {
                const float t = static_cast<float>(step) / static_cast<float>(steps);
                PaintMosaicBlock(core::Point{ lastMosaicPoint_.x + dx * t,
                                             lastMosaicPoint_.y + dy * t });
            }
            lastMosaicPoint_ = local;
            dirty_ = true;
        }
        return;
    }

    if (drawing_) {
        const Rect bounds = selection_.Bounds();
        const Point local{ static_cast<float>(point.x) - bounds.left,
                           static_cast<float>(point.y) - bounds.top };

        if (tool_ == AnnotationTool::Eraser) {
            document_.EraseAt(local, eraserSize_ * 0.5f);
        } else if (tool_ == AnnotationTool::Pen || tool_ == AnnotationTool::Highlighter) {
            document_.AddPoint(local, 2.0f);
        } else if (tool_ == AnnotationTool::Arrow && (GetKeyState(VK_SHIFT) & 0x8000) != 0) {
            // Shift + 箭头：45° 一档吸附（同 1.x 的 SnapToAngle）。
            const core::Point start = document_.Active().points.empty()
                                          ? local
                                          : document_.Active().points.front();
            document_.SetEnd(core::SnapToAngle(start, local));
        } else {
            document_.SetEnd(local);
        }
        dirty_ = true;
        return;
    }

    // 没在拖：更新光标与「光标下窗口」的悬停候选（1.x 的十字线 + 窗口描边 + 放大镜）。
    UpdateCursor(point);
    if (phase_ == Phase::Selecting && selection_.Bounds().IsEmpty()) {
        UpdateHoverCandidate(point);
    } else {
        ClearHoverCandidate();
    }
    dirty_ = true;
}

void ScreenshotOverlay::UpdateHoverCandidate(POINT point) {
    // 1.x 的节流：24 ms 一次，光标挪不到 2 px 就不重探。
    constexpr int kProbeIntervalMs = 24;
    constexpr int kProbeMovePx = 2;
    const std::uint64_t now = GetTickCount64();
    const int dx = std::abs(point.x - lastProbePoint_.x);
    const int dy = std::abs(point.y - lastProbePoint_.y);
    if (hoverProbed_ && now - lastProbeMs_ < static_cast<std::uint64_t>(kProbeIntervalMs) &&
        dx < kProbeMovePx && dy < kProbeMovePx) {
        return;
    }
    lastProbeMs_ = now;
    lastProbePoint_ = point;
    hoverProbed_ = true;

    if (IsOverChrome(point)) {
        hoverWindow_ = nullptr;
        hoverRect_ = IntRect{};
        return;
    }
    RECT rect{};
    const HWND target = WindowAtPoint(point, static_cast<HWND>(window_->Handle()), 32);
    if (target != nullptr && WindowVisibleRect(target, &rect)) {
        // 夹进虚拟桌面：悬在屏幕外的窗口部分不参与挖洞，否则会出现负尺寸矩形。
        const IntRect clamped{ std::max<LONG>(rect.left, windowX_),
                               std::max<LONG>(rect.top, windowY_),
                               std::min<LONG>(rect.right, windowX_ + screenWidth_),
                               std::min<LONG>(rect.bottom, windowY_ + screenHeight_) };
        if (clamped.right - clamped.left >= 32 && clamped.bottom - clamped.top >= 32) {
            hoverWindow_ = target;
            hoverRect_ = clamped;
            return;
        }
    }
    // 1.x：认不出窗口（桌面、Shell，或候选太小/太靠边）就退回光标所在整块屏。
    // 高亮永远有候选，单击即全屏——2.0 之前这里留空，桌面上开截图看起来就像「识别没了」。
    hoverWindow_ = nullptr;
    hoverRect_ = MonitorBoundsAt(point);
}

void ScreenshotOverlay::ClearHoverCandidate() {
    hoverWindow_ = nullptr;
    hoverRect_ = IntRect{};
    hoverProbed_ = false;
}

core::IntRect ScreenshotOverlay::MonitorBoundsAt(POINT point) const {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    const HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(monitor, &info)) {
        return IntRect{ info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right,
                        info.rcMonitor.bottom };
    }
    return IntRect{ windowX_, windowY_, windowX_ + screenWidth_, windowY_ + screenHeight_ };
}

bool ScreenshotOverlay::IsOverChrome(POINT point) const {
    // point 是屏幕坐标；工具栏/选项栏是客户区坐标。
    const int x = point.x - windowX_;
    const int y = point.y - windowY_;
    if (toolbarVisible_ && x >= toolbarBounds_.left && x < toolbarBounds_.right &&
        y >= toolbarBounds_.top && y < toolbarBounds_.bottom) {
        return true;
    }
    if (optionBounds_.right > optionBounds_.left && x >= optionBounds_.left &&
        x < optionBounds_.right && y >= optionBounds_.top && y < optionBounds_.bottom) {
        return true;
    }
    return false;
}

void ScreenshotOverlay::UpdateCursor(POINT point) {
    if (window_ == nullptr) {
        return;
    }
    const wchar_t* name = IDC_CROSS;
    if (IsOverChrome(point)) {
        name = IDC_ARROW;
    } else if (!annotating_ && !selection_.Bounds().IsEmpty()) {
        switch (selection_.HitTest(Point{ static_cast<float>(point.x),
                                          static_cast<float>(point.y) })) {
        case core::SelectionHandle::TopLeft:
        case core::SelectionHandle::BottomRight:
            name = IDC_SIZENWSE;
            break;
        case core::SelectionHandle::TopRight:
        case core::SelectionHandle::BottomLeft:
            name = IDC_SIZENESW;
            break;
        case core::SelectionHandle::Left:
        case core::SelectionHandle::Right:
            name = IDC_SIZEWE;
            break;
        case core::SelectionHandle::Top:
        case core::SelectionHandle::Bottom:
            name = IDC_SIZENS;
            break;
        case core::SelectionHandle::Move:
            name = IDC_SIZEALL;
            break;
        case core::SelectionHandle::None:
            break;
        }
    }
    SetCursor(LoadCursorW(nullptr, name));
}

void ScreenshotOverlay::StartNewSelection(POINT point) {
    // 1.x：在选区外按下就是「重来」——不保留标注，也没有中间的「取消选中」态。
    selection_.Cancel();
    document_.Clear();
    annotating_ = false;
    selectedAnnotation_ = -1;
    movingAnnotation_ = false;
    phase_ = Phase::Selecting;
    hoverWindow_ = nullptr;
    hoverRect_ = IntRect{};
    hoverProbed_ = false;

    // 松手时如果短于一拖，就用按下那一刻的窗口/显示器兜底。
    RECT windowRect{};
    const HWND target = WindowAtPoint(point, static_cast<HWND>(window_->Handle()), 32);
    pressWindowRect_ = (target != nullptr && WindowVisibleRect(target, &windowRect))
                           ? IntRect{ windowRect.left, windowRect.top, windowRect.right,
                                      windowRect.bottom }
                           : IntRect{};
    pressMonitorRect_ = MonitorBoundsAt(point);

    selecting_ = true;
    selection_.Begin(Point{ static_cast<float>(point.x), static_cast<float>(point.y) });
    dirty_ = true;
}

void ScreenshotOverlay::SelectWindowOrMonitor(POINT point) {
    const IntRect fallback =
        !pressWindowRect_.IsEmpty() ? pressWindowRect_ : MonitorBoundsAt(point);
    if (fallback.IsEmpty()) {
        return;
    }
    selection_.Begin(Point{ static_cast<float>(fallback.left),
                            static_cast<float>(fallback.top) });
    selection_.Update(Point{ static_cast<float>(fallback.right),
                             static_cast<float>(fallback.bottom) },
                      false);
    selection_.Commit();
}

void ScreenshotOverlay::NudgeSelection(int dx, int dy) {
    if (selection_.MoveBy(Point{ static_cast<float>(dx), static_cast<float>(dy) })) {
        dirty_ = true;
    }
}

void ScreenshotOverlay::OnRightButtonDown(POINT point) {
    if (editorWindow_ != nullptr) {
        CommitTextEditor();
    }
    if (IsOverChrome(point)) {
        return;
    }
    if (annotating_) {
        // 标注态：右键先退回移动/选择态，选区保留。
        annotating_ = false;
        selectedAnnotation_ = -1;
        movingAnnotation_ = false;
        dirty_ = true;
        return;
    }
    if (!selection_.Bounds().IsEmpty()) {
        // 已有选区：清掉它，回到悬停态。
        selection_.Cancel();
        document_.Clear();
        phase_ = Phase::Selecting;
        ClearHoverCandidate();
        dirty_ = true;
        return;
    }
    Close();
}

void ScreenshotOverlay::OnLeftButtonDown(POINT point) {
    // 文字编辑器开着时，点别处就是「确认」——1.x 也是这样，不需要按回车。
    // （点在 EDIT 自己身上的消息不会到这里，所以不会误提交。）
    if (editorWindow_ != nullptr) {
        CommitTextEditor();
    }
    if (options_.mode == OverlayMode::RecordRegion) {
        if (phase_ == Phase::Selecting) {
            StartNewSelection(point);
        }
        return;
    }

    // 选项栏/工具栏的矩形是客户区坐标，这里是屏幕坐标，先换算。
    const POINT client{ point.x - windowX_, point.y - windowY_ };

    // 选项栏在当前工具下展开时优先命中。
    if (optionBounds_.right > optionBounds_.left && client.x >= optionBounds_.left &&
        client.x < optionBounds_.right && client.y >= optionBounds_.top &&
        client.y < optionBounds_.bottom) {
        for (const OptionButton& option : optionButtons_) {
            if (client.x >= option.rect.left && client.x < option.rect.right &&
                client.y >= option.rect.top && client.y < option.rect.bottom) {
                HandleOption(option.action);
                return;
            }
        }
        return;
    }

    // 工具栏优先：它在选区之上，点按钮不能落成标注。
    if (toolbarVisible_ && client.x >= toolbarBounds_.left && client.x < toolbarBounds_.right &&
        client.y >= toolbarBounds_.top && client.y < toolbarBounds_.bottom) {
        for (const ToolbarButton& button : toolbar_) {
            if (client.x >= button.rect.left && client.x < button.rect.right &&
                client.y >= button.rect.top && client.y < button.rect.bottom) {
                if (button.enabled) {
                    HandleToolbar(button);
                }
                return;
            }
        }
        return;
    }

    const Point screen{ static_cast<float>(point.x), static_cast<float>(point.y) };

    // 双击 = 复制（1.x 只在截图模式的选区内部生效）。
    const std::uint64_t now = GetTickCount64();
    const bool doubleClick = now - lastClickMs_ <= 400 &&
                             std::abs(point.x - lastClickPoint_.x) <= 4 &&
                             std::abs(point.y - lastClickPoint_.y) <= 4;
    lastClickMs_ = now;
    lastClickPoint_ = point;

    if (phase_ == Phase::Selecting) {
        StartNewSelection(point);
        return;
    }

    // 完成之后默认是「移动/选择」态；只有标注态里鼠标才画图。
    if (!annotating_) {
        const core::SelectionHandle handle = selection_.HitTest(screen, core::SelectionModel::kHandleTolerance * uiScale_);
        if (handle != core::SelectionHandle::None && handle != core::SelectionHandle::Move) {
            resizing_ = true;
            resizeHandle_ = handle;
            ClearHoverCandidate();
            return;
        }
        if (handle == core::SelectionHandle::Move) {
            if (doubleClick && options_.mode == OverlayMode::Screenshot) {
                if (CopySelection()) {
                    Close();
                }
                return;
            }
            movingSelection_ = true;
            lastPoint_ = point;
            ClearHoverCandidate();
            return;
        }
        // 选区外按下 = 重新框选（标注一起丢掉，1.x 同语义）。
        StartNewSelection(point);
        return;
    }

    if (!selection_.Bounds().Contains(screen)) {
        // 已经有标注时，点选区外不重开选区：那会把手上的标注全丢掉（1.x 在标注态
        // 也是把按下当落笔，不重开）。要重选先右键清掉选区/标注。
        if (!document_.Items().empty()) {
            return;
        }
        StartNewSelection(point);
        return;
    }

    const Rect bounds = selection_.Bounds();
    const Point local{ screen.x - bounds.left, screen.y - bounds.top };

    if (tool_ == AnnotationTool::Eraser) {
        drawing_ = true;
        document_.EraseAt(local, eraserSize_ * 0.5f);
        dirty_ = true;
        return;
    }

    if (tool_ == AnnotationTool::Mosaic) {
        // 马赛克笔：按下就落第一块，之后跟着鼠标一路抹。
        mosaicPainting_ = true;
        lastMosaicPoint_ = local;
        // 一笔开始时合成一次「底图 + 当下所有标注」；这一笔的每个块都从它采样。
        // 合成走独立离屏目标，不碰窗口交换链，所以不会闪。
        const Rect selectionBounds = selection_.Bounds();
        const bool cacheHit = mosaicBaseValid_ &&
                              mosaicBaseRevision_ == document_.Revision() &&
                              mosaicBaseSelection_ == selectionBounds;
        if (!cacheHit) {
            mosaicBaseValid_ = false;
            mosaicBase_.clear();
            if (!document_.Items().empty()) {
                const IntRect region{ static_cast<int>(selectionBounds.left) - windowX_,
                                      static_cast<int>(selectionBounds.top) - windowY_,
                                      static_cast<int>(selectionBounds.right) - windowX_,
                                      static_cast<int>(selectionBounds.bottom) - windowY_ };
                int composedWidth = 0;
                int composedHeight = 0;
                mosaicBaseValid_ = ComposeRegion(region, document_.Items(), mosaicBase_,
                                                 composedWidth, composedHeight);
                mosaicBaseWidth_ = composedWidth;
                mosaicBaseHeight_ = composedHeight;
                if (mosaicBaseValid_) {
                    mosaicBaseRevision_ = document_.Revision();
                    mosaicBaseSelection_ = selectionBounds;
                }
            }
        }
        PaintMosaicBlock(local);
        dirty_ = true;
        return;
    }

    if (tool_ == AnnotationTool::Text) {
        // 点在已有的文字上就改那一条（1.x 的行为），否则新建一条；支持任意多条。
        int hitIndex = -1;
        for (int index = static_cast<int>(document_.Items().size()) - 1; index >= 0; --index) {
            const core::Annotation& item = document_.Items()[static_cast<std::size_t>(index)];
            if (item.tool == AnnotationTool::Text && !item.points.empty() &&
                core::HitTest(item, local, 8.0f * uiScale_)) {
                hitIndex = index;
                break;
            }
        }
        if (hitIndex >= 0) {
            editingTextIndex_ = hitIndex;
            OpenTextEditor(document_.Items()[static_cast<std::size_t>(hitIndex)].points.front());
        } else {
            editingTextIndex_ = -1;
            OpenTextEditor(local);
        }
        return;
    }

    document_.Begin(tool_, style_);
    document_.AddPoint(local);
    if (tool_ == AnnotationTool::Step) {
        // 序号是单击落点，不需要拖。
        document_.Commit();
    } else {
        drawing_ = true;
    }
    dirty_ = true;
}

void ScreenshotOverlay::OnLeftButtonUp(POINT point) {
    if (selecting_) {
        selection_.Commit();
        selecting_ = false;
        if (selection_.Bounds().IsEmpty()) {
            // 短于一拖 = 单击：选光标下的窗口，没有窗口就选整块屏（1.x 的点击语义）。
            SelectWindowOrMonitor(point);
        }
        if (!selection_.Bounds().IsEmpty()) {
            phase_ = Phase::Annotating;
            // 刚拖完不是标注态：此时在选区里拖动是「移动整块」。
            annotating_ = false;
            ClearHoverCandidate();
            if (options_.mode == OverlayMode::RecordRegion) {
                const core::Rect bounds = selection_.Bounds();
                const core::IntRect rect{ static_cast<int>(bounds.left),
                                          static_cast<int>(bounds.top),
                                          static_cast<int>(bounds.left) +
                                              (static_cast<int>(bounds.Width()) & ~1),
                                          static_cast<int>(bounds.top) +
                                              (static_cast<int>(bounds.Height()) & ~1) };
                const auto handler = options_.onRegion;
                Close();
                if (handler) {
                    handler(rect);
                }
                return;
            }
        }
        dirty_ = true;
        return;
    }

    if (resizing_) {
        resizing_ = false;
        dirty_ = true;
        return;
    }

    if (movingSelection_) {
        movingSelection_ = false;
        dirty_ = true;
        return;
    }

    if (movingAnnotation_) {
        movingAnnotation_ = false;
        document_.CommitAnnotationChange();
        dirty_ = true;
        return;
    }

    if (mosaicPainting_) {
        mosaicPainting_ = false;
        dirty_ = true;
        return;
    }

    if (drawing_) {
        const AnnotationTool tool = document_.Active().tool;
        if (tool != AnnotationTool::Eraser) {
            document_.Commit();
            if (tool == AnnotationTool::Mosaic && !document_.Items().empty()) {
                UpdateMosaicTile(const_cast<core::Annotation&>(document_.Items().back()));
            }
        }
        drawing_ = false;
        dirty_ = true;
    }
}

void ScreenshotOverlay::SetTool(AnnotationTool tool) {
    tool_ = tool;
    if (tool == AnnotationTool::Highlighter) {
        style_.thickness = core::annotation_defaults::kHighlighterThickness;
    } else if (style_.thickness > core::annotation_defaults::kMaxThickness / 2.0f) {
        style_.thickness = core::annotation_defaults::kThickness;
    }
    dirty_ = true;
}

void ScreenshotOverlay::HandleToolbar(const ToolbarButton& button) {
    const bool hasSelection = !selection_.Bounds().IsEmpty();

    switch (button.action) {
    case Action::ToolSelect:
        annotating_ = false;
        selectedAnnotation_ = -1;
        break;

    case Action::ToolShape:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(shapeTool_);
        }
        break;

    case Action::ToolArrow:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Arrow);
        }
        break;

    case Action::ToolPen:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Pen);
        }
        break;

    case Action::ToolHighlighter:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Highlighter);
        }
        break;

    case Action::ToolEraser:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Eraser);
        }
        break;

    case Action::ToolText:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Text);
        }
        break;

    case Action::ToolMosaic:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Mosaic);
        }
        break;

    case Action::ToolStep:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            SetTool(AnnotationTool::Step);
        }
        break;

    // 工具栏上已经没有矩形/椭圆两个独立按钮了（1.x 用「形状」+ 选项栏），
    // 但保留这两个分支给旧的输入路径。
    case Action::ToolRectangle:
    case Action::ToolEllipse:
        if (hasSelection) {
            annotating_ = true;
            selectedAnnotation_ = -1;
            shapeTool_ = button.action == Action::ToolRectangle ? AnnotationTool::Rectangle
                                                                : AnnotationTool::Ellipse;
            SetTool(shapeTool_);
        }
        break;

    case Action::Color:
        style_.color = core::annotation_defaults::PaletteColor(button.payload);
        break;

    case Action::Undo:
        document_.Undo();
        break;

    case Action::Redo:
        document_.Redo();
        break;

    case Action::ClearAnnotations:
        document_.Clear();
        selectedAnnotation_ = -1;
        break;

    case Action::Ocr:
        if (options_.mode == OverlayMode::Ocr) {
            RunOcr();
            pendingClose_ = true;
        }
        break;

    case Action::DoneAndCopy:
    case Action::Copy:
        if (options_.mode == OverlayMode::Ocr) {
            RunOcr();
            pendingClose_ = true;
        } else if (CopySelection()) {
            pendingClose_ = true;
        }
        break;

    case Action::Save:
        if (SaveSelection(nullptr)) {
            pendingClose_ = true;
        }
        break;

    case Action::Pin:
        if (PinSelection()) {
            pendingClose_ = true;
        }
        break;

    case Action::CancelOverlay:
    case Action::CloseOverlay:
        pendingClose_ = true;
        break;

    case Action::None:
        break;
    }

    // 导出期间窗口还活着；Close 推迟到 Pump，避免在窗口过程里销毁自己的窗口。
    dirty_ = true;
}

// ---- 工具栏布局与绘制 ----

void ScreenshotOverlay::HandleOption(Action action) {
    switch (action) {
    case Action::FillOn:
        style_.filled = true;
        break;
    case Action::FillOff:
        style_.filled = false;
        break;
    case Action::ThicknessDown:
        style_.thickness = std::max(1.0f, style_.thickness - 1.0f);
        break;
    case Action::ThicknessUp:
        style_.thickness = std::min(24.0f, style_.thickness + 1.0f);
        break;
    case Action::TextSizeDown:
        style_.textSize = std::max(10.0f, style_.textSize - 2.0f);
        break;
    case Action::TextSizeUp:
        style_.textSize = std::min(48.0f, style_.textSize + 2.0f);
        break;
    case Action::Bold:
        style_.bold = !style_.bold;
        break;
    case Action::Italic:
        style_.italic = !style_.italic;
        break;
    case Action::MosaicDown:
        style_.mosaicBlockPx = std::max(6, style_.mosaicBlockPx - 2);
        break;
    case Action::MosaicUp:
        style_.mosaicBlockPx = std::min(30, style_.mosaicBlockPx + 2);
        break;
    case Action::ShapeRectangle:
        shapeTool_ = AnnotationTool::Rectangle;
        tool_ = shapeTool_;
        break;
    case Action::ShapeEllipse:
        shapeTool_ = AnnotationTool::Ellipse;
        tool_ = shapeTool_;
        break;
    case Action::EraserDown:
        eraserSize_ = std::max(8.0f, eraserSize_ - 8.0f);
        break;
    case Action::EraserUp:
        eraserSize_ = std::min(120.0f, eraserSize_ + 8.0f);
        break;
    case Action::ArrowEnd:
        style_.arrowHead = core::ArrowHeadMode::End;
        break;
    case Action::ArrowBoth:
        style_.arrowHead = core::ArrowHeadMode::Both;
        break;
    case Action::ArrowNone:
        style_.arrowHead = core::ArrowHeadMode::None;
        break;
    default:
        break;
    }
    dirty_ = true;
}

void ScreenshotOverlay::LayoutOptionBar() {
    optionButtons_.clear();
    optionBounds_ = RECT{};

    if (!annotating_ || !toolbarVisible_) {
        return;
    }

    const int optionButton = Scale(kOptionButton);
    const int optionHeight = Scale(kOptionHeight);
    const int colorWidth = Scale(kColorWidth);
    const int gap = Scale(kToolbarGap);
    const int padding = Scale(kToolbarPadding);
    const int separatorMargin = Scale(kSeparatorMargin);

    const auto addIcon = [&](Action action, ToolbarIcon icon, int width, bool active) {
        OptionButton button;
        button.action = action;
        button.icon = icon;
        button.rect = RECT{ 0, 0, width, optionButton };
        button.active = active;
        optionButtons_.push_back(button);
    };
    const auto addText = [&](Action action, const std::string& text, int width, bool active) {
        OptionButton button;
        button.action = action;
        button.text = text;
        button.rect = RECT{ 0, 0, width, optionButton };
        button.active = active;
        optionButtons_.push_back(button);
    };
    const auto addSeparator = [&] {
        OptionButton button;
        button.action = Action::None;
        button.rect = RECT{ 0, 0, 1, optionButton };
        optionButtons_.push_back(button);
    };

    // 1.x 的选项栏：色板在最左，然后才是当前工具的参数。
    for (int i = 0; i < core::annotation_defaults::kPaletteCount; ++i) {
        OptionButton button;
        button.action = Action::Color;
        button.rect = RECT{ 0, 0, colorWidth, optionButton };
        button.colorIndex = i;
        button.active = style_.color == core::annotation_defaults::PaletteColor(i);
        optionButtons_.push_back(button);
    }
    addSeparator();

    switch (tool_) {
    case AnnotationTool::Rectangle:
    case AnnotationTool::Ellipse:
        addIcon(Action::ShapeRectangle, ToolbarIcon::Rectangle, optionButton,
                tool_ == AnnotationTool::Rectangle);
        addIcon(Action::ShapeEllipse, ToolbarIcon::Ellipse, optionButton,
                tool_ == AnnotationTool::Ellipse);
        addIcon(Action::FillOff, ToolbarIcon::Outline, optionButton, !style_.filled);
        addIcon(Action::FillOn, ToolbarIcon::Fill, optionButton, style_.filled);
        addIcon(Action::ThicknessDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(style_.thickness))),
                Scale(30), false);
        addIcon(Action::ThicknessUp, ToolbarIcon::Increase, optionButton, false);
        break;
    case AnnotationTool::Pen:
    case AnnotationTool::Highlighter:
        addIcon(Action::ThicknessDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(style_.thickness))),
                Scale(30), false);
        addIcon(Action::ThicknessUp, ToolbarIcon::Increase, optionButton, false);
        break;
    case AnnotationTool::Arrow:
        addIcon(Action::ArrowEnd, ToolbarIcon::Arrow, optionButton,
                style_.arrowHead == core::ArrowHeadMode::End);
        addIcon(Action::ArrowBoth, ToolbarIcon::ArrowBoth, optionButton,
                style_.arrowHead == core::ArrowHeadMode::Both);
        addIcon(Action::ArrowNone, ToolbarIcon::ArrowLine, optionButton,
                style_.arrowHead == core::ArrowHeadMode::None);
        addIcon(Action::ThicknessDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(style_.thickness))),
                Scale(30), false);
        addIcon(Action::ThicknessUp, ToolbarIcon::Increase, optionButton, false);
        break;
    case AnnotationTool::Text:
        addIcon(Action::TextSizeDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(style_.textSize))),
                Scale(30), false);
        addIcon(Action::TextSizeUp, ToolbarIcon::Increase, optionButton, false);
        addIcon(Action::Bold, ToolbarIcon::Bold, optionButton, style_.bold);
        addIcon(Action::Italic, ToolbarIcon::Italic, optionButton, style_.italic);
        break;
    case AnnotationTool::Mosaic:
        addIcon(Action::MosaicDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(style_.mosaicBlockPx), Scale(30), false);
        addIcon(Action::MosaicUp, ToolbarIcon::Increase, optionButton, false);
        break;
    case AnnotationTool::Step:
        addIcon(Action::TextSizeDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(style_.textSize))),
                Scale(30), false);
        addIcon(Action::TextSizeUp, ToolbarIcon::Increase, optionButton, false);
        break;
    case AnnotationTool::Eraser:
        addIcon(Action::EraserDown, ToolbarIcon::Decrease, optionButton, false);
        addText(Action::None, std::to_string(static_cast<int>(std::lround(eraserSize_))),
                Scale(30), false);
        addIcon(Action::EraserUp, ToolbarIcon::Increase, optionButton, false);
        break;
    default:
        break;
    }

    if (optionButtons_.empty()) {
        return;
    }

    // 先在「选项栏左上角 = (0,0)」的坐标系里摆好：背景宽度取最右边界，写命中区用同一套
    // 推进规则。之前宽度和摆放各算一遍，最后几个按钮跑到背景外面 —— 既看着「没显示全」，
    // 又因为落在 optionBounds_ 之外而点不到（调不了粗细/字号就是这个）。
    int cursor = padding;
    for (OptionButton& option : optionButtons_) {
        const int w = option.rect.right;   // 宽度先暂存在 right 里
        const bool separator = w <= 1;
        if (separator) {
            cursor += separatorMargin - gap;
        }
        option.rect = RECT{ cursor, 0, cursor + w, optionButton };
        cursor += w + (separator ? separatorMargin : gap);
    }
    int width = 0;
    for (const OptionButton& option : optionButtons_) {
        width = std::max(width, static_cast<int>(option.rect.right));
    }
    width += padding;

    // 位置自动判断：默认贴工具栏下方；如果那块压在选区上（工具栏本身翻到选区上方时
    // 就很常见），改放工具栏上方；两边都压选区时取不越界的那一个。
    const Rect selection = selection_.Bounds();
    const RECT selectionLocal{
        static_cast<LONG>(selection.left) - windowX_, static_cast<LONG>(selection.top) - windowY_,
        static_cast<LONG>(selection.right) - windowX_,
        static_cast<LONG>(selection.bottom) - windowY_ };
    const auto overlapsSelection = [&](int top) {
        if (selection.IsEmpty()) {
            return false;
        }
        const bool vertical = top < selectionLocal.bottom && top + optionHeight > selectionLocal.top;
        return vertical;   // x 方向上选项栏本来就贴着工具栏，横向几乎必然重叠，只看纵向。
    };
    const int gapToToolbar = Scale(6);
    const int screenEdge = Scale(8);
    int below = static_cast<int>(toolbarBounds_.bottom) + gapToToolbar;
    if (below + optionHeight > screenHeight_ - screenEdge) {
        below = -1;
    }
    int above = static_cast<int>(toolbarBounds_.top) - optionHeight - gapToToolbar;
    if (above < screenEdge) {
        above = -1;
    }
    int y = -1;
    if (below >= 0 && !overlapsSelection(below)) {
        y = below;
    } else if (above >= 0 && !overlapsSelection(above)) {
        y = above;
    } else if (below >= 0) {
        y = below;
    } else if (above >= 0) {
        y = above;
    } else {
        y = std::clamp(static_cast<int>(toolbarBounds_.bottom) + gapToToolbar, screenEdge,
                       std::max(screenEdge, screenHeight_ - optionHeight - screenEdge));
    }
    int x = static_cast<int>(toolbarBounds_.left);
    x = std::clamp(x, Scale(8), std::max(Scale(8), screenWidth_ - width - Scale(8)));
    y = std::clamp(y, Scale(8), std::max(Scale(8), screenHeight_ - optionHeight - Scale(8)));

    for (OptionButton& option : optionButtons_) {
        option.rect.left += x;
        option.rect.right += x;
        option.rect.top += y + Scale(4);
        option.rect.bottom += y + Scale(4);
    }
    optionBounds_ = RECT{ x, y, x + width, y + optionHeight };
}

void ScreenshotOverlay::RenderOptionBar(core::FramePlan& plan) {
    if (optionButtons_.empty() || rasterizer_ == nullptr) {
        return;
    }
    const float radius = Scale(10.0f);
    const Rect background{ static_cast<float>(optionBounds_.left),
                           static_cast<float>(optionBounds_.top),
                           static_cast<float>(optionBounds_.right),
                           static_cast<float>(optionBounds_.bottom) };
    plan.Add(core::FillRoundedRect{ background, radius,
                                    Color{ 0.98f, 0.976f, 0.984f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ background, radius, Color{ 0.0f, 0.0f, 0.0f, 0.13f },
                                      Scale(1.0f) });

    for (const OptionButton& option : optionButtons_) {
        const Rect rect{ static_cast<float>(option.rect.left),
                         static_cast<float>(option.rect.top),
                         static_cast<float>(option.rect.right),
                         static_cast<float>(option.rect.bottom) };

        if (option.colorIndex >= 0) {
            // 1.x 的色板：12 DIP 圆点，选中时外面套 2 DIP 环。
            const Color color = core::annotation_defaults::PaletteColor(option.colorIndex);
            const float cx = (rect.left + rect.right) * 0.5f;
            const float cy = (rect.top + rect.bottom) * 0.5f;
            const float dot = Scale(6.0f);
            const Rect dotRect{ cx - dot, cy - dot, cx + dot, cy + dot };
            plan.Add(core::FillEllipse{ dotRect, color });
            if (option.colorIndex == 6 || option.colorIndex == 7) {
                plan.Add(core::StrokeEllipse{ dotRect, Color{ 0.0f, 0.0f, 0.0f, 0.15f },
                                              Scale(1.0f) });
            }
            if (option.active) {
                const float ring = Scale(1.0f);
                plan.Add(core::StrokeEllipse{ Rect{ rect.left + ring, rect.top + Scale(2.0f),
                                                    rect.right - ring,
                                                    rect.bottom - Scale(2.0f) },
                                              Color{ 0.184f, 0.486f, 0.965f, 1.0f },
                                              Scale(2.0f) });
            }
            continue;
        }

        if (option.rect.right - option.rect.left == 1) {
            const float dividerX = rect.left + 0.5f;
            plan.Add(core::FillRect{ Rect{ dividerX, rect.top + Scale(4.0f), dividerX + 1.0f,
                                           rect.bottom - Scale(4.0f) },
                                     Color{ 0.0f, 0.0f, 0.0f, 0.10f } });
            continue;
        }

        if (option.active) {
            plan.Add(core::FillRoundedRect{ rect, Scale(7.0f),
                                            Color{ 0.184f, 0.486f, 0.965f, 1.0f } });
        }

        if (option.text.empty() && option.icon != ToolbarIcon::None) {
            RenderToolbarIcon(plan, rect, option.icon, option.active, true);
            continue;
        }
        if (option.text.empty()) {
            continue;
        }

        const bool valueOnly = option.action == Action::None;
        const core::FontSpec spec{ options_.fontFamily, valueOnly ? 400 : 600, Scale(12.0f),
                                   option.active ? Color{ 1.0f, 1.0f, 1.0f, 1.0f }
                                                 : Color{ 0.235f, 0.259f, 0.314f, 1.0f } };
        const core::TextCell measured =
            rasterizer_->LayOut(option.text, spec, core::Point{ 0.0f, 0.0f });
        if (measured.atlas == 0) {
            continue;
        }
        const float textWidth = measured.destination.Width();
        const float textHeight = measured.destination.Height();
        const core::TextCell cell = rasterizer_->LayOut(
            option.text, spec, core::Point{ rect.left + (rect.Width() - textWidth) * 0.5f,
                                            rect.top + (rect.Height() - textHeight) * 0.5f });
        if (cell.atlas != 0) {
            plan.Add(cell.ToBlit());
        }
    }
}

void ScreenshotOverlay::RenderSizeBadge(core::FramePlan& plan) {
    if (rasterizer_ == nullptr || annotating_) {
        return;
    }
    // 1.x：只在「正拖着」与「悬停某个候选（窗口或整块屏）」时显示；选区完成后就收起。
    const bool dragging = selecting_ || resizing_ || movingSelection_;
    const bool hovering = selection_.Bounds().IsEmpty() && !hoverRect_.IsEmpty();
    if (!dragging && !hovering) {
        return;
    }

    IntRect rectPx = hoverRect_;
    if (!hovering) {
        const core::Rect bounds = selection_.Bounds();
        rectPx = IntRect{ static_cast<int>(bounds.left), static_cast<int>(bounds.top),
                          static_cast<int>(bounds.right), static_cast<int>(bounds.bottom) };
    }
    if (rectPx.IsEmpty()) {
        return;
    }

    const std::string text = std::to_string(rectPx.right - rectPx.left) + " × " +
                             std::to_string(rectPx.bottom - rectPx.top);
    const core::FontSpec spec{ options_.fontFamily, 400, Scale(11.0f),
                               Color{ 0x3C / 255.0f, 0x42 / 255.0f, 0x50 / 255.0f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(text, spec, core::Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }
    const float width = measured.destination.Width() + Scale(16.0f);
    const float height = Scale(24.0f);
    const float margin = Scale(6.0f);
    const float left = std::clamp(static_cast<float>(rectPx.left) - windowX_, margin,
                                  std::max(margin,
                                           static_cast<float>(screenWidth_) - width - margin));
    // 1.x：贴选区左边、上边再往上 8 DIP；放不下就挪到上边线下面 9 DIP。
    float top = static_cast<float>(rectPx.top) - windowY_ - Scale(8.0f) - height;
    if (top < margin) {
        top = static_cast<float>(rectPx.top) - windowY_ + Scale(9.0f);
    }
    top = std::clamp(top, margin,
                     std::max(margin, static_cast<float>(screenHeight_) - height - margin));

    plan.Add(core::FillRoundedRect{ core::Rect{ left, top, left + width, top + height },
                                    Scale(6.0f), Color{ 1.0f, 1.0f, 1.0f, 0xF2 / 255.0f } });
    plan.Add(core::StrokeRoundedRect{ core::Rect{ left, top, left + width, top + height },
                                      Scale(6.0f), Color{ 0.0f, 0.0f, 0.0f, 0x1F / 255.0f },
                                      Scale(1.0f) });
    const core::TextCell cell = rasterizer_->LayOut(
        text, spec,
        core::Point{ left + Scale(8.0f), top + (height - measured.destination.Height()) * 0.5f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

void ScreenshotOverlay::RenderMagnifier(core::FramePlan& plan) {
    if (!mouseInside_ || rasterizer_ == nullptr || baseBgra_.empty()) {
        return;
    }
    // 1.x：空闲悬停与拖动/缩放时显示；标注态、移动整块、选区完成后都不显示。
    if (annotating_ || movingSelection_ || IsOverChrome(lastMouse_)) {
        return;
    }
    if (!selection_.Bounds().IsEmpty() && !selecting_ && !resizing_) {
        return;
    }

    // 1.x 的放大镜：136 DIP 取景框（深色底、内描边）+ 间隔 5 DIP 的信息面板。
    const float size = Scale(136.0f);
    const float infoHeight = Scale(46.0f);
    const float gap = Scale(5.0f);
    const float cardWidth = size;
    const float cardHeight = size + gap + infoHeight;
    const float offset = Scale(24.0f);
    // 一律换算到窗口本地坐标再夹紧：多屏时窗口原点不是 (0,0)。
    const float sampleX = static_cast<float>(lastMouse_.x - windowX_);
    const float sampleY = static_cast<float>(lastMouse_.y - windowY_);
    float x = sampleX + offset;
    float y = sampleY + offset;
    if (x + cardWidth > static_cast<float>(screenWidth_) - Scale(6.0f)) {
        x = sampleX - offset - cardWidth;
    }
    if (y + cardHeight > static_cast<float>(screenHeight_) - Scale(6.0f)) {
        y = sampleY - offset - cardHeight;
    }
    const float margin = Scale(6.0f);
    x = std::clamp(x, margin,
                   std::max(margin, static_cast<float>(screenWidth_) - cardWidth - margin));
    y = std::clamp(y, margin,
                   std::max(margin, static_cast<float>(screenHeight_) - cardHeight - margin));

    // 取景框：17×17 的设备像素放进 136 DIP，约 8 倍放大。
    const core::Rect viewport{ x, y, x + size, y + size };
    plan.Add(core::FillRoundedRect{ viewport, Scale(10.0f),
                                    Color{ 0x0B / 255.0f, 0x0F / 255.0f, 0x16 / 255.0f, 1.0f } });
    plan.Add(core::Blit{ capturedHandle_,
                         core::Rect{ sampleX - 8.0f, sampleY - 8.0f, sampleX + 9.0f,
                                     sampleY + 9.0f },
                         core::Rect{ x + Scale(1.0f), y + Scale(1.0f), x + size - Scale(1.0f),
                                     y + size - Scale(1.0f) },
                         1.0f });
    // 中心像素：9 DIP 的模式色方环。
    const float centerX = x + size * 0.5f;
    const float centerY = y + size * 0.5f;
    const float ring = Scale(4.5f);
    plan.Add(core::StrokeRect{ core::Rect{ centerX - ring, centerY - ring, centerX + ring,
                                           centerY + ring },
                               SelectionAccent(), Scale(1.5f) });
    plan.Add(core::StrokeRoundedRect{ core::Rect{ x + Scale(1.0f), y + Scale(1.0f),
                                                  x + size - Scale(1.0f),
                                                  y + size - Scale(1.0f) },
                                      Scale(9.0f), Color{ 1.0f, 1.0f, 1.0f, 0x5C / 255.0f },
                                      Scale(1.0f) });

    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    const int px = static_cast<int>(sampleX);
    const int py = static_cast<int>(sampleY);
    if (px >= 0 && py >= 0 && px < screenWidth_ && py < screenHeight_) {
        const std::size_t index = (static_cast<std::size_t>(py) * screenWidth_ + px) * 4;
        b = baseBgra_[index + 0];
        g = baseBgra_[index + 1];
        r = baseBgra_[index + 2];
    }

    char hex[16] = {};
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    char coordinates[32] = {};
    // 坐标读数是屏幕坐标（虚拟桌面），不是窗口本地坐标。
    std::snprintf(coordinates, sizeof(coordinates), "%d, %d", px + windowX_, py + windowY_);

    // 信息面板：#F210141C 底、#33FFFFFF 边、8 DIP 圆角。
    const core::Rect info{ x, y + size + gap, x + cardWidth, y + cardHeight };
    plan.Add(core::FillRoundedRect{ info, Scale(8.0f),
                                    Color{ 0x10 / 255.0f, 0x14 / 255.0f, 0x1C / 255.0f,
                                           0xF2 / 255.0f } });
    plan.Add(core::StrokeRoundedRect{ info, Scale(8.0f),
                                      Color{ 1.0f, 1.0f, 1.0f, 0x33 / 255.0f }, Scale(1.0f) });
    const float swatchX = x + Scale(8.0f);
    const float swatchY = y + size + gap + Scale(8.0f);
    const float swatch = Scale(11.0f);
    plan.Add(core::FillRoundedRect{ core::Rect{ swatchX, swatchY, swatchX + swatch,
                                                swatchY + swatch },
                                    Scale(3.0f),
                                    Color{ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f } });
    plan.Add(core::StrokeRoundedRect{ core::Rect{ swatchX, swatchY, swatchX + swatch,
                                                  swatchY + swatch },
                                      Scale(3.0f), Color{ 1.0f, 1.0f, 1.0f, 0.18f },
                                      Scale(1.0f) });

    const core::FontSpec hexSpec{ options_.fontFamily, 600, Scale(11.0f),
                                  Color{ 0xE8 / 255.0f, 0xED / 255.0f, 0xF6 / 255.0f, 1.0f } };
    const core::FontSpec coordSpec{ options_.fontFamily, 400, Scale(10.0f),
                                    Color{ 0x8D / 255.0f, 0x9A / 255.0f, 0xAF / 255.0f, 1.0f } };
    const core::TextCell hexCell =
        rasterizer_->LayOut(hex, hexSpec,
                            core::Point{ swatchX + Scale(17.0f), swatchY - Scale(2.0f) });
    if (hexCell.atlas != 0) {
        plan.Add(hexCell.ToBlit());
    }
    const core::TextCell coordCell = rasterizer_->LayOut(
        coordinates, coordSpec,
        core::Point{ swatchX + Scale(17.0f), swatchY + Scale(13.0f) });
    if (coordCell.atlas != 0) {
        plan.Add(coordCell.ToBlit());
    }
}

void ScreenshotOverlay::StepHoverAnimation() {
    bool settled = true;
    for (int i = 0; i < 32; ++i) {
        const float target = (i == hoveredToolbar_) ? 1.0f : 0.0f;
        if (hoverAlpha_[i] < target) {
            hoverAlpha_[i] = std::min(target, hoverAlpha_[i] + 0.18f);
            settled = false;
        } else if (hoverAlpha_[i] > target) {
            hoverAlpha_[i] = std::max(target, hoverAlpha_[i] - 0.18f);
            settled = false;
        }
    }
    hoverAnimating_ = !settled;
    if (settled) {
        KillTimer(static_cast<HWND>(window_->Handle()), 1);
    }
    dirty_ = true;
}

void ScreenshotOverlay::StepToolbarAnimation() {
    toolbarAnim_ = std::min(1.0f, toolbarAnim_ + 0.2f);
    if (toolbarAnim_ >= 1.0f) {
        KillTimer(static_cast<HWND>(window_->Handle()), 3);
    }
    dirty_ = true;
}

void ScreenshotOverlay::CopyColorUnderCursor(bool asRgb) {
    // 1.x 的 C：随时可用的取色，不受选区/工具影响。
    const int px = lastMouse_.x - windowX_;
    const int py = lastMouse_.y - windowY_;
    if (baseBgra_.empty() || px < 0 || py < 0 || px >= screenWidth_ || py >= screenHeight_) {
        toast_ = L"无法读取光标下的颜色";
    } else {
        const std::size_t index = (static_cast<std::size_t>(py) * screenWidth_ + px) * 4;
        const int b = baseBgra_[index + 0];
        const int g = baseBgra_[index + 1];
        const int r = baseBgra_[index + 2];
        wchar_t text[48] = {};
        if (asRgb) {
            std::swprintf(text, 48, L"rgb(%d, %d, %d)", r, g, b);
        } else {
            std::swprintf(text, 48, L"#%02X%02X%02X", r, g, b);
        }
        toast_ = CopyText(text) ? std::wstring(L"已复制 ") + text : L"复制颜色失败";
        colorAsRgb_ = asRgb;
    }
    // 取色本身就是要「拿走颜色」，所以不弹提示、直接收工（1.x 会留一条 HUD，
    // 但这里按产品要求关掉选区）。
    toast_.clear();
    pendingClose_ = true;
    dirty_ = true;
}

void ScreenshotOverlay::RenderToast(core::FramePlan& plan) {
    if (toast_.empty() || rasterizer_ == nullptr) {
        return;
    }
    const std::string text = WideToUtf8(toast_);
    const core::FontSpec spec{ options_.fontFamily, 500, Scale(12.0f),
                               Color{ 0.91f, 0.93f, 0.96f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(text, spec, Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }
    const float width = measured.destination.Width() + Scale(20.0f);
    const float height = Scale(30.0f);
    float x = static_cast<float>(lastMouse_.x - windowX_) + Scale(20.0f);
    float y = static_cast<float>(lastMouse_.y - windowY_) + Scale(40.0f);
    const float margin = Scale(8.0f);
    x = std::clamp(x, margin, std::max(margin, static_cast<float>(screenWidth_) - width - margin));
    y = std::clamp(y, margin,
                   std::max(margin, static_cast<float>(screenHeight_) - height - margin));
    plan.Add(core::FillRoundedRect{ Rect{ x, y, x + width, y + height }, Scale(8.0f),
                                    Color{ 0.086f, 0.102f, 0.133f, 0.94f } });
    const core::TextCell cell = rasterizer_->LayOut(
        text, spec,
        Point{ x + Scale(10.0f), y + (height - measured.destination.Height()) * 0.5f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

void ScreenshotOverlay::RenderTooltip(core::FramePlan& plan) {
    if (!toolbarVisible_ || hoveredToolbar_ < 0 ||
        hoveredToolbar_ >= static_cast<int>(toolbar_.size()) || rasterizer_ == nullptr) {
        return;
    }
    // 1.x 是 WPF 的默认 tooltip（悬停约 600 ms 出现），这里用同一套浅色卡片。
    if (GetTickCount64() - hoverSinceMs_ < 600) {
        return;
    }
    const ToolbarButton& button = toolbar_[static_cast<std::size_t>(hoveredToolbar_)];
    if (button.label.empty() || button.icon == ToolbarIcon::Separator) {
        return;
    }
    const core::FontSpec spec{ options_.fontFamily, 400, Scale(12.0f),
                               Color{ 0x3C / 255.0f, 0x42 / 255.0f, 0x50 / 255.0f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(button.label, spec, Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }
    const float width = measured.destination.Width() + Scale(16.0f);
    const float height = measured.destination.Height() + Scale(10.0f);
    const float margin = Scale(8.0f);
    float x = static_cast<float>(button.rect.left) +
              (button.rect.right - button.rect.left - static_cast<LONG>(width)) * 0.5f;
    // 提示放在按钮上方：下方就是二级工具栏，放下面会互相遮挡。
    float y = static_cast<float>(button.rect.top) - height - Scale(8.0f);
    if (y < margin) {
        y = static_cast<float>(button.rect.bottom) + Scale(8.0f);
    }
    x = std::clamp(x, margin, std::max(margin, static_cast<float>(screenWidth_) - width - margin));
    plan.Add(core::FillRoundedRect{ Rect{ x, y, x + width, y + height }, Scale(6.0f),
                                    Color{ 1.0f, 1.0f, 1.0f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ Rect{ x, y, x + width, y + height }, Scale(6.0f),
                                      Color{ 0.0f, 0.0f, 0.0f, 0.14f }, Scale(1.0f) });
    const core::TextCell cell = rasterizer_->LayOut(
        button.label, spec,
        Point{ x + Scale(8.0f), y + (height - measured.destination.Height()) * 0.5f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

void ScreenshotOverlay::LayoutToolbar() {
    toolbar_.clear();

    const bool hasSelection = !selection_.Bounds().IsEmpty();
    const bool recordMode = options_.mode == OverlayMode::RecordRegion;
    const bool ocrMode = options_.mode == OverlayMode::Ocr;

    // 1.x 的尺寸是 DIP，这里按屏幕缩放换算成物理像素（150% 时 30 → 45）。
    const int button = Scale(kToolbarButton);
    const int gap = Scale(kToolbarGap);
    const int padding = Scale(kToolbarPadding);

    int cursorX = padding;
    const int top = padding;

    const auto add = [&](Action action, ToolbarIcon icon, const char* label, bool enabled,
                         bool active) {
        ToolbarButton entry;
        entry.rect = RECT{ cursorX, top, cursorX + button, top + button };
        entry.action = action;
        entry.icon = icon;
        entry.label = label;
        entry.enabled = enabled;
        entry.active = active;
        toolbar_.push_back(entry);
        cursorX += button + gap;
    };
    const auto separator = [&] {
        const int margin = Scale(kSeparatorMargin);
        const int width = std::max(1, Scale(kSeparatorWidth));
        cursorX += margin - gap;
        ToolbarButton entry;
        entry.rect = RECT{ cursorX, top, cursorX + width, top + button };
        entry.icon = ToolbarIcon::Separator;
        toolbar_.push_back(entry);
        cursorX += width + margin;
    };
    const auto isShape = [&] {
        return tool_ == AnnotationTool::Rectangle || tool_ == AnnotationTool::Ellipse;
    };

    // 1.x 的分组与顺序：选择 | 形状·箭头 | 笔·荧光·橡皮·文字·马赛克·序号 |
    // 撤·重·清空 | 存·贴·识别 | 取消·完成。颜色在选项栏里，不占工具栏。
    add(Action::ToolSelect, ToolbarIcon::Select, "选择 / 移动 (V)", true, !annotating_);
    separator();
    add(Action::ToolShape, ToolbarIcon::Shape, "形状 (R/E) · 在选项栏切换矩形 / 椭圆",
        hasSelection, annotating_ && isShape());
    add(Action::ToolArrow, ToolbarIcon::Arrow, "箭头 (A) · Shift 锁定角度", hasSelection,
        annotating_ && tool_ == AnnotationTool::Arrow);
    separator();
    add(Action::ToolPen, ToolbarIcon::Pen, "画笔 (P) · 滚轮调粗细", hasSelection,
        annotating_ && tool_ == AnnotationTool::Pen);
    add(Action::ToolHighlighter, ToolbarIcon::Highlighter, "荧光笔 (H) · 滚轮调粗细",
        hasSelection, annotating_ && tool_ == AnnotationTool::Highlighter);
    add(Action::ToolEraser, ToolbarIcon::Eraser, "橡皮擦 (X) · 滚轮调大小 · 划过删除整条标注",
        hasSelection, annotating_ && tool_ == AnnotationTool::Eraser);
    add(Action::ToolText, ToolbarIcon::Text, "文字 (T) · 滚轮调字号", hasSelection,
        annotating_ && tool_ == AnnotationTool::Text);
    add(Action::ToolMosaic, ToolbarIcon::Mosaic, "马赛克 (M) · 滚轮调密度", hasSelection,
        annotating_ && tool_ == AnnotationTool::Mosaic);
    add(Action::ToolStep, ToolbarIcon::Step, "序号 (N)", hasSelection,
        annotating_ && tool_ == AnnotationTool::Step);
    separator();
    add(Action::Undo, ToolbarIcon::Undo, "撤销 (Ctrl+Z)", document_.CanUndo(), false);
    add(Action::Redo, ToolbarIcon::Redo, "重做 (Ctrl+Y)", document_.CanRedo(), false);
    add(Action::ClearAnnotations, ToolbarIcon::Clear, "清空标注", !document_.Items().empty(),
        false);

    separator();
    if (recordMode) {
        add(Action::CancelOverlay, ToolbarIcon::Close, "取消 (Esc)", true, false);
    } else {
        add(Action::Save, ToolbarIcon::Save, "保存为图片 (Ctrl+S)", hasSelection, false);
        add(Action::Pin, ToolbarIcon::Pin, "贴图 (Ctrl+T)", hasSelection, false);
        if (!ocrMode) {
            add(Action::Ocr, ToolbarIcon::Ocr, "识别文字", hasSelection, false);
        }
        separator();
        add(Action::CancelOverlay, ToolbarIcon::Close, "取消 (Esc)", true, false);
        if (!ocrMode) {
            // 1.x 的「完成」就是复制到剪贴板；OCR 模式下松手即识别，所以不显示。
            add(Action::DoneAndCopy, ToolbarIcon::Done, "完成并复制到剪贴板 (Enter)",
                hasSelection, false);
        }
    }

    const int width = cursorX - gap + padding;
    const int height = button + padding * 2;
    const int gapToSelection = Scale(12);
    const int screenEdge = Scale(8);

    const Rect selection = selection_.Bounds();
    const int localRight = static_cast<int>(selection.right) - windowX_;
    const int localTop = static_cast<int>(selection.top) - windowY_;
    const int localBottom = static_cast<int>(selection.bottom) - windowY_;
    int x = 0;
    int y = 0;
    if (selection.IsEmpty()) {
        x = (screenWidth_ - width) / 2;
        y = Scale(20);
    } else {
        // 1.x：贴选区右下角、往下 12 DIP；下面放不下就翻到上面，再放不下就压进选区底部。
        // 横向只夹紧、不翻到左边。
        x = localRight - width;
        y = localBottom + gapToSelection;
        if (y + height > screenHeight_ - screenEdge) {
            y = localTop - height - gapToSelection;
        }
        if (y < screenEdge) {
            y = std::clamp(localBottom - height - Scale(10), screenEdge,
                           std::max(screenEdge, screenHeight_ - height - screenEdge));
        }
        x = std::clamp(x, screenEdge, std::max(screenEdge, screenWidth_ - width - screenEdge));
        y = std::clamp(y, screenEdge, std::max(screenEdge, screenHeight_ - height - screenEdge));
    }

    // 1.x：刚拖完的选区不算「完成」，工具栏等松手之后才出现；出现时从下往上滑 6 DIP。
    const bool visible = phase_ == Phase::Annotating && !selection.IsEmpty();
    if (visible && !toolbarVisible_) {
        toolbarAnim_ = 0.0f;
        SetTimer(static_cast<HWND>(window_->Handle()), 3, 16, nullptr);
    }
    toolbarVisible_ = visible;
    y += static_cast<int>((1.0f - toolbarAnim_) * Scale(6.0f));

    for (ToolbarButton& entry : toolbar_) {
        entry.rect.left += x;
        entry.rect.right += x;
        entry.rect.top += y;
        entry.rect.bottom += y;
    }
    toolbarBounds_ = RECT{ x, y, x + width, y + height };
}

void ScreenshotOverlay::RenderToolbarIcon(core::FramePlan& plan, const core::Rect& rect,
                                          ToolbarIcon icon, bool active, bool enabled) {
    const char* name = IconNameFor(icon);
    if (name == nullptr) {
        return;
    }
    // 1.x：禁用态整块 0.32 不透明度。
    const float alpha = enabled ? 1.0f : 0.32f;
    const core::Color ink = active ? core::Color{ 1.0f, 1.0f, 1.0f, alpha }
                                   : core::Color{ 0.235f, 0.259f, 0.314f, alpha };
    AppendIconPath(plan, rect, icon, ink);
}

void ScreenshotOverlay::AppendIconPath(core::FramePlan& plan, const core::Rect& rect,
                                       ToolbarIcon icon, const core::Color& color) {
    const char* name = IconNameFor(icon);
    if (name == nullptr) {
        return;
    }
    const ParsedIcon& parsed = ParsedIconFor(name);
    if (!parsed.valid) {
        return;
    }
    // 1.x 用 20 DIP 的 Viewbox 装 160×160 的路径。
    const float size = Scale(20.0f);
    // 1.x 是 160 单位的路径画在 200×200 的 Path 里，再被 20 DIP 的 Viewbox 整体缩到 0.1。
    // 所以这里除以 200而不是 160：图形落在 20 DIP 盒子的中间（约 16 DIP 大），
    // 除以 160 会让图形偏右下 12.5%，看起来就是「图标没居中」。
    const float scale = size / 200.0f;
    const float originX = (rect.left + rect.right) * 0.5f - size * 0.5f;
    const float originY = (rect.top + rect.bottom) * 0.5f - size * 0.5f;
    const auto transform = [&](core::Point point) {
        return core::Point{ originX + point.x * scale, originY + point.y * scale };
    };
    const int key = static_cast<int>(icon) * 1000003 +
                    static_cast<int>(rect.left) * 7919 + static_cast<int>(rect.top) * 104729 +
                    static_cast<int>(rect.right) * 31 + static_cast<int>(rect.bottom) * 17;

    const auto emit = [&](const std::vector<core::PathCommand>& commands, bool evenOdd) {
        if (commands.empty()) {
            return;
        }
        core::PathShape shape;
        shape.evenOdd = evenOdd;
        shape.color = color;
        shape.cacheKey = key;
        shape.commands.reserve(commands.size());
        for (const core::PathCommand& command : commands) {
            core::PathCommand copy = command;
            switch (command.kind) {
            case core::PathCommand::Kind::Move:
            case core::PathCommand::Kind::Line:
                copy.points[0] = transform(command.points[0]);
                break;
            case core::PathCommand::Kind::Cubic:
                copy.points[0] = transform(command.points[0]);
                copy.points[1] = transform(command.points[1]);
                copy.points[2] = transform(command.points[2]);
                break;
            case core::PathCommand::Kind::Quad:
                copy.points[0] = transform(command.points[0]);
                copy.points[2] = transform(command.points[2]);
                break;
            case core::PathCommand::Kind::Close:
                break;
            }
            shape.commands.push_back(copy);
        }
        plan.Add(shape);
    };

    emit(parsed.first, parsed.evenOddFirst);
    // CombinedGeometry 的并集：两条同色画一遍。第二条缓存键 +1，避免与第一条撞车。
    if (!parsed.second.empty()) {
        core::PathShape shape;
        shape.evenOdd = parsed.evenOddSecond;
        shape.color = color;
        shape.cacheKey = key + 1;
        shape.commands.reserve(parsed.second.size());
        for (const core::PathCommand& command : parsed.second) {
            core::PathCommand copy = command;
            switch (command.kind) {
            case core::PathCommand::Kind::Move:
            case core::PathCommand::Kind::Line:
                copy.points[0] = transform(command.points[0]);
                break;
            case core::PathCommand::Kind::Cubic:
                copy.points[0] = transform(command.points[0]);
                copy.points[1] = transform(command.points[1]);
                copy.points[2] = transform(command.points[2]);
                break;
            case core::PathCommand::Kind::Quad:
                copy.points[0] = transform(command.points[0]);
                copy.points[2] = transform(command.points[2]);
                break;
            case core::PathCommand::Kind::Close:
                break;
            }
            shape.commands.push_back(copy);
        }
        plan.Add(shape);
    }
}

void ScreenshotOverlay::RenderToolbar(core::FramePlan& plan) {
    // 与鼠标命中同一套坐标：LayoutToolbar 已经产出客户区坐标，这里不再减窗口原点。
    const Rect background{ static_cast<float>(toolbarBounds_.left),
                           static_cast<float>(toolbarBounds_.top),
                           static_cast<float>(toolbarBounds_.right),
                           static_cast<float>(toolbarBounds_.bottom) };
    // 1.x 的工具栏：浅色 #FAF8F9FB、10 px 圆角、#22000000 边框 + 一层柔和投影。
    for (int i = 3; i >= 1; --i) {
        const float grow = Scale(static_cast<float>(i) * 2.0f);
        const float drop = Scale(static_cast<float>(i) * 1.5f);
        plan.Add(core::FillRoundedRect{
            Rect{ background.left - grow, background.top - grow + drop,
                  background.right + grow, background.bottom + grow + drop },
            Scale(10.0f) + grow, Color{ 0.0f, 0.0f, 0.0f, 0.05f } });
    }
    plan.Add(core::FillRoundedRect{ background, Scale(10.0f),
                                    Color{ 0.98f, 0.976f, 0.984f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ background, Scale(10.0f), Color{ 0.0f, 0.0f, 0.0f, 0.13f },
                                      Scale(1.0f) });

    for (std::size_t i = 0; i < toolbar_.size(); ++i) {
        const ToolbarButton& button = toolbar_[i];
        const Rect rect{ static_cast<float>(button.rect.left),
                         static_cast<float>(button.rect.top),
                         static_cast<float>(button.rect.right),
                         static_cast<float>(button.rect.bottom) };

        if (button.icon == ToolbarIcon::Separator) {
            const float dividerX = (rect.left + rect.right) * 0.5f;
            plan.Add(core::FillRect{ Rect{ dividerX, rect.top + Scale(4.0f), dividerX + 1.0f,
                                           rect.bottom - Scale(4.0f) },
                                     Color{ 0.0f, 0.0f, 0.0f, 0.10f } });
            continue;
        }

        // 1.x：悬停 #14000000、按下 #22000000、激活工具/选择态用强调色实底。
        if (button.active) {
            plan.Add(core::FillRoundedRect{ rect, Scale(7.0f),
                                            Color{ 0.184f, 0.486f, 0.965f, 1.0f } });
        } else if (i < 32 && hoverAlpha_[i] > 0.01f) {
            plan.Add(core::FillRoundedRect{ rect, Scale(7.0f),
                                            Color{ 0.0f, 0.0f, 0.0f, 0.08f * hoverAlpha_[i] } });
        }

        RenderToolbarIcon(plan, rect, button.icon, button.active, button.enabled);
    }
}

void ScreenshotOverlay::RenderSelection(core::FramePlan& plan) {
    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);
    const float width = static_cast<float>(screenWidth_);
    const float height = static_cast<float>(screenHeight_);

    // 1.x 的两档暗度：还没选区时 36% 黑，选区出现后 55% 黑。
    const Color hoverDim{ 0.0f, 0.0f, 0.0f, 0x5C / 255.0f };
    const Color activeDim{ 0.0f, 0.0f, 0.0f, 0x8C / 255.0f };
    const Color accent = SelectionAccent();
    const float border = Scale(1.5f);

    const Rect selection = selection_.Bounds();
    if (selection.IsEmpty()) {
        // 悬停候选（窗口或整块屏）：把那块挖出来（同 hover 暗度）+ 1.5 DIP 描边，其余压暗。
        if (!hoverRect_.IsEmpty()) {
            const Rect hole{ static_cast<float>(hoverRect_.left) - ox,
                             static_cast<float>(hoverRect_.top) - oy,
                             static_cast<float>(hoverRect_.right) - ox,
                             static_cast<float>(hoverRect_.bottom) - oy };
            AddDimAround(plan, hole, hoverDim, width, height);
            plan.Add(core::StrokeRect{ hole, accent, border });
            return;
        }
        plan.Add(core::FillRect{ Rect{ 0.0f, 0.0f, width, height }, hoverDim });
        return;
    }

    const Rect local{ selection.left - ox, selection.top - oy, selection.right - ox,
                      selection.bottom - oy };
    AddDimAround(plan, local, activeDim, width, height);
    plan.Add(core::StrokeRect{ local, accent, border });

    // 1.x：标注态不画把手，其余时候（拖选中 / 完成后移动缩放）都画。
    if (annotating_) {
        return;
    }

    const float cx = (local.left + local.right) * 0.5f;
    const float cy = (local.top + local.bottom) * 0.5f;
    const float half = Scale(3.5f);
    const Point handles[] = { { local.left, local.top },     { cx, local.top },
                              { local.right, local.top },    { local.right, cy },
                              { local.right, local.bottom }, { cx, local.bottom },
                              { local.left, local.bottom },  { local.left, cy } };

    for (const Point& handle : handles) {
        const Rect rect{ handle.x - half, handle.y - half, handle.x + half, handle.y + half };
        plan.Add(core::FillRect{ rect, accent });
    }
}

void ScreenshotOverlay::AddDimAround(core::FramePlan& plan, const Rect& hole, const Color& dim,
                                     float width, float height) {
    // 洞之外压暗：四条矩形拼起来，不需要额外的遮罩纹理。
    plan.Add(core::FillRect{ Rect{ 0.0f, 0.0f, width, hole.top }, dim });
    plan.Add(core::FillRect{ Rect{ 0.0f, hole.bottom, width, height }, dim });
    plan.Add(core::FillRect{ Rect{ 0.0f, hole.top, hole.left, hole.bottom }, dim });
    plan.Add(core::FillRect{ Rect{ hole.right, hole.top, width, hole.bottom }, dim });
}

core::Color ScreenshotOverlay::SelectionAccent() const {
    // 1.x 按模式换描边色：截图 #7CA8FF、录制 #FF7A7A、OCR #B08CFF。
    switch (options_.mode) {
    case OverlayMode::RecordRegion:
        return Color{ 1.0f, 0.478f, 0.478f, 1.0f };
    case OverlayMode::Ocr:
        return Color{ 0x69 / 255.0f, 0x8C / 255.0f, 1.0f, 1.0f };
    case OverlayMode::Screenshot:
    default:
        return Color{ 0x7C / 255.0f, 0xA8 / 255.0f, 1.0f, 1.0f };
    }
}

void ScreenshotOverlay::RenderCrosshair(core::FramePlan& plan) {
    // 1.x：只有「还没选区、也没在拖」的时候画，1 px、55% 白，贯穿整块画面。
    if (!mouseInside_ || !selection_.Bounds().IsEmpty() || selecting_ || annotating_) {
        return;
    }
    if (IsOverChrome(lastMouse_)) {
        return;
    }
    const Color ink{ 1.0f, 1.0f, 1.0f, 0x8C / 255.0f };
    const float x = static_cast<float>(lastMouse_.x - windowX_);
    const float y = static_cast<float>(lastMouse_.y - windowY_);
    plan.Add(core::FillRect{ Rect{ x, 0.0f, x + 1.0f, static_cast<float>(screenHeight_) }, ink });
    plan.Add(core::FillRect{ Rect{ 0.0f, y, static_cast<float>(screenWidth_), y + 1.0f }, ink });
}

void ScreenshotOverlay::RenderHint(core::FramePlan& plan) {
    if (rasterizer_ == nullptr) {
        return;
    }
    // 1.x：提示条只在还没选区时显示；选区一出现就收起（快捷键交给工具栏 tooltip）。
    if (!selection_.Bounds().IsEmpty()) {
        return;
    }

    const char* hint = "拖动框选或单击窗口 · Enter 复制 · Esc 取消";
    switch (options_.mode) {
    case OverlayMode::RecordRegion:
        hint = "拖动框选或单击窗口 · Esc 取消";
        break;
    case OverlayMode::Ocr:
        hint = "拖动框选或单击窗口 · Enter 识别 · Esc 取消";
        break;
    case OverlayMode::Screenshot:
    default:
        break;
    }
    // 文案尺寸也按 DIP：150% 屏上 12 DIP 就是 18 px，之前按 12 px 画所以显得小。
    const core::FontSpec spec{ options_.fontFamily, 400, Scale(12.0f),
                               Color{ 0x3C / 255.0f, 0x42 / 255.0f, 0x50 / 255.0f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(hint, spec, Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }

    // 1.x 的提示条：白色圆角、左边一个模式色圆点、深色文字；贴在光标所在那块屏的顶部。
    const float textWidth = measured.destination.Width();
    const float textHeight = measured.destination.Height();
    const float chipWidth = textWidth + Scale(36.0f);
    const float chipHeight = textHeight + Scale(18.0f);
    const IntRect monitor = MonitorBoundsAt(lastMouse_);
    const float chipLeft = static_cast<float>(monitor.left) +
                           (monitor.right - monitor.left - chipWidth) * 0.5f - windowX_;
    const float chipTop = static_cast<float>(monitor.top) + Scale(26.0f) - windowY_;

    plan.Add(core::FillRoundedRect{ Rect{ chipLeft, chipTop, chipLeft + chipWidth,
                                         chipTop + chipHeight },
                                    Scale(10.0f), Color{ 1.0f, 1.0f, 1.0f, 0xF7 / 255.0f } });
    plan.Add(core::StrokeRoundedRect{ Rect{ chipLeft, chipTop, chipLeft + chipWidth,
                                            chipTop + chipHeight },
                                      Scale(10.0f), Color{ 0.0f, 0.0f, 0.0f, 0x1F / 255.0f },
                                      Scale(1.0f) });
    const float dotX = chipLeft + Scale(14.0f);
    const float dotY = chipTop + chipHeight * 0.5f;
    const float dot = Scale(3.0f);
    plan.Add(core::FillEllipse{ Rect{ dotX - dot, dotY - dot, dotX + dot, dotY + dot },
                                SelectionAccent() });
    const core::TextCell cell = rasterizer_->LayOut(
        hint, spec, Point{ dotX + Scale(9.0f), chipTop + (chipHeight - textHeight) * 0.5f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

void ScreenshotOverlay::Render() {
    if (!active_ || backend_ == nullptr || window_ == nullptr) {
        return;
    }

    LayoutToolbar();
    LayoutOptionBar();

    core::FramePlan plan;
    // 底图的源矩形是**图片自己的坐标**：capturedHandle_ 的像素 (0,0) 就是虚拟桌面左上角，
    // 和窗口原点 (windowX_, windowY_) 无关。之前这里传了屏幕坐标，多屏之后 windowY_ = -720，
    // D2D 会去采样图片外的区域——上面那块变成透明（露出实时桌面）、其余整体错位 720 px。
    plan.Add(core::Blit{ capturedHandle_,
                         Rect{ 0.0f, 0.0f, static_cast<float>(screenWidth_),
                               static_cast<float>(screenHeight_) },
                         Rect{ 0.0f, 0.0f, static_cast<float>(screenWidth_),
                               static_cast<float>(screenHeight_) },
                         1.0f });

    if (!selection_.Bounds().IsEmpty()) {
        AnnotationPlanOptions options;
        options.rasterizer = rasterizer_.get();
        options.fontFamily = options_.fontFamily;
        options.scale = 1.0f;
        options.sizeScale = uiScale_;
        options.origin = Point{ selection_.Bounds().left - windowX_,
                                selection_.Bounds().top - windowY_ };
        // 正在画的那一笔还在 document_.Active() 里，没有进 Items()（Commit 才进）。
        // 不把它一起画出来，表现就是「拖的时候什么都看不到、松手才出现」。
        if (drawing_ && !document_.Active().points.empty()) {
            std::vector<core::Annotation> preview = document_.Items();
            preview.push_back(document_.Active());
            core::AppendAnnotationPlan(preview, options, plan);
        } else {
            core::AppendAnnotationPlan(document_.Items(), options, plan);
        }
    }

    // 选区/压暗/悬停描边/把手的绘制现在统一在这里（没有选区时画整屏悬停暗度）。
    RenderSelection(plan);
    RenderCrosshair(plan);

    if (options_.mode != OverlayMode::RecordRegion) {
        if (toolbarVisible_) {
            RenderToolbar(plan);
        }
        RenderOptionBar(plan);
        RenderMagnifier(plan);
        // 提示条画在选项栏之后：它就在按钮下方，先画就会被二级工具栏盖住。
        if (toolbarVisible_) {
            RenderTooltip(plan);
        }
    }
    RenderHint(plan);
    RenderSizeBadge(plan);
    RenderToast(plan);

    backend_->WaitForFrameSlot();
    backend_->Render(plan, IntRect{});
    backend_->Present();
    dirty_ = false;
}

// ---- 马赛克与文字 ----

void ScreenshotOverlay::RunOcr() {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!ComposeSelection(bgra, width, height)) {
        errorKind_ = "compose";
        return;
    }
    if (!options_.onOcr) {
        errorKind_ = "noocr";
        return;
    }
    const core::Rect selection = selection_.Bounds();
    options_.onOcr(bgra.data(), width, height, static_cast<int>(selection.left),
                   static_cast<int>(selection.top));
}

void ScreenshotOverlay::PaintMosaicBlock(core::Point local) {
    const Rect bounds = selection_.Bounds();
    if (bounds.IsEmpty() || local.x < 0.0f || local.y < 0.0f ||
        local.x > bounds.Width() || local.y > bounds.Height()) {
        return;
    }
    const float radius = MosaicBrushRadius();
    document_.Begin(AnnotationTool::Mosaic, style_);
    document_.AddPoint(core::Point{ local.x - radius, local.y - radius });
    document_.SetEnd(core::Point{ local.x + radius, local.y + radius });
    document_.Commit();
    if (!document_.Items().empty()) {
        UpdateMosaicTile(const_cast<core::Annotation&>(document_.Items().back()));
    }
}

std::vector<core::Annotation> ScreenshotOverlay::BelowMosaics() const {
    if (document_.Items().size() <= 1) {
        return {};
    }
    return std::vector<core::Annotation>(document_.Items().begin(),
                                         document_.Items().end() - 1);
}

bool ScreenshotOverlay::ComposeRegion(const core::IntRect& region,
                                      const std::vector<core::Annotation>& annotations,
                                      std::vector<std::uint8_t>& bgra, int& width, int& height) {
    const int regionWidth = region.right - region.left;
    const int regionHeight = region.bottom - region.top;
    if (regionWidth <= 0 || regionHeight <= 0 || backend_ == nullptr) {
        return false;
    }
    core::FramePlan plan;
    plan.Add(core::Blit{
        capturedHandle_,
        Rect{ static_cast<float>(region.left), static_cast<float>(region.top),
              static_cast<float>(region.right), static_cast<float>(region.bottom) },
        Rect{ 0.0f, 0.0f, static_cast<float>(regionWidth),
              static_cast<float>(regionHeight) },
        1.0f });
    core::AnnotationPlanOptions options;
    options.rasterizer = rasterizer_.get();
    options.fontFamily = options_.fontFamily;
    options.scale = 1.0f;
    options.sizeScale = uiScale_;
    // 标注点存的是选区局部坐标，合成目标的原点 = region 左上角。
    // 之前这里写成 -region.left/-region.top，等于把标注整体推到合成画面之外，
    // 合成结果只剩底图——这就是「马赛克不含下方标注」的直接原因。
    const core::Rect selection = selection_.Bounds();
    options.origin = Point{ selection.left - windowX_ - static_cast<float>(region.left),
                            selection.top - windowY_ - static_cast<float>(region.top) };
    core::AppendAnnotationPlan(annotations, options, plan);

    // 独立离屏目标：不 Detach/Attach 主后端，窗口视觉不重建，不闪。
    if (!backend_->RenderOffscreen(plan, regionWidth, regionHeight, bgra, width, height) ||
        width != regionWidth || height != regionHeight) {
        char message[96] = {};
        std::snprintf(message, sizeof(message), "halocap: mosaic compose failed (hr=0x%08lX)\n",
                      static_cast<unsigned long>(backend_->LastHr()));
        OutputDebugStringA(message);
        errorKind_ = "mosaic-compose";
        return false;
    }
    return true;
}

void ScreenshotOverlay::UpdateMosaicTile(core::Annotation& annotation) {
    if (annotation.tool != AnnotationTool::Mosaic || annotation.points.size() < 2 ||
        baseBgra_.empty() || backend_ == nullptr) {
        return;
    }

    const Rect bounds = selection_.Bounds();
    const Rect localRect = core::NormalizeRect(annotation.points[0], annotation.points[1]);
    const IntRect region{ static_cast<int>(localRect.left + bounds.left) - windowX_,
                          static_cast<int>(localRect.top + bounds.top) - windowY_,
                          static_cast<int>(localRect.right + bounds.left) - windowX_,
                          static_cast<int>(localRect.bottom + bounds.top) - windowY_ };

    // 采样源：这一笔开头合成好的「底图 + 下方标注」（选区局部坐标），没有就退回底图。
    const core::Rect selection = selection_.Bounds();
    core::ImageRef base{ baseBgra_.data(), screenWidth_, screenHeight_, screenWidth_ * 4 };
    core::IntRect sampleRegion = region;
    if (mosaicBaseValid_ && !mosaicBase_.empty() && mosaicBaseWidth_ > 0) {
        base = core::ImageRef{ mosaicBase_.data(), mosaicBaseWidth_, mosaicBaseHeight_,
                               mosaicBaseWidth_ * 4 };
        const int originX = static_cast<int>(selection.left) - windowX_;
        const int originY = static_cast<int>(selection.top) - windowY_;
        sampleRegion = core::IntRect{ region.left - originX, region.top - originY,
                                      region.right - originX, region.bottom - originY };
    }
    // 模糊程度固定（12 px 一块）；选项栏那个数字只决定马赛克笔的大小。
    const int block = core::MosaicBlockPixels(12, 1.0f);
    const core::MosaicTile tile = core::DownsampleRegion(base, sampleRegion, block);
    if (tile.width <= 0 || tile.height <= 0) {
        return;
    }

    const core::ImageHandle handle = backend_->RegisterImage(
        core::ImageRef{ tile.bgra.data(), tile.width, tile.height, tile.width * 4 });
    if (handle != 0) {
        annotation.mosaicTile = handle;
        annotation.mosaicTileSize = core::Size{ static_cast<float>(tile.width),
                                                static_cast<float>(tile.height) };
    }
}

void ScreenshotOverlay::AddTextAnnotation(core::Point localPoint, std::string_view text) {
    if (text.empty() || rasterizer_ == nullptr) {
        return;
    }

    const core::FontSpec spec{ options_.fontFamily, style_.bold ? 700 : 400,
                               style_.textSize, style_.color };
    const core::TextCell measured = rasterizer_->LayOut(text, spec, Point{ 0.0f, 0.0f });
    const float width = measured.destination.Width();
    const float height = measured.destination.Height();

    core::Annotation& annotation = document_.Begin(AnnotationTool::Text, style_);
    annotation.points = { localPoint };
    annotation.text = std::string(text);
    annotation.textBounds = Rect{ localPoint.x, localPoint.y, localPoint.x + width,
                                  localPoint.y + height };
    document_.Commit();
    dirty_ = true;
}

void ScreenshotOverlay::OpenTextEditor(core::Point localPoint) {
    if (!EnsureEditorClass() || editorWindow_ != nullptr) {
        return;
    }

    textPoint_ = localPoint;
    const Rect bounds = selection_.Bounds();
    const int screenX = static_cast<int>(bounds.left + localPoint.x);
    const int screenY = static_cast<int>(bounds.top + localPoint.y);
    const int editorWidth = std::max(180, static_cast<int>(style_.textSize) * 12);
    const int editorHeight = static_cast<int>(style_.textSize) + 18;

    editorWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kEditorClass,
                                    L"HaloCap text", WS_POPUP, screenX, screenY, editorWidth,
                                    editorHeight, nullptr, nullptr, GetModuleHandleW(nullptr),
                                    nullptr);
    if (editorWindow_ == nullptr) {
        return;
    }

    editControl_ = CreateWindowExW(0, L"EDIT", L"",
                                   WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 4, 4,
                                   editorWidth - 8, editorHeight - 8, editorWindow_,
                                   reinterpret_cast<HMENU>(1), GetModuleHandleW(nullptr), nullptr);
    if (editControl_ == nullptr) {
        DestroyTextEditor();
        return;
    }

    HFONT font = CreateFontW(-static_cast<int>(style_.textSize), 0, 0, 0,
                             style_.bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    SendMessageW(editControl_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

    g_editorOwner = this;
    g_oldEditProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        editControl_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&ScreenshotOverlay::EditorEditProc)));

    if (editingTextIndex_ >= 0 &&
        editingTextIndex_ < static_cast<int>(document_.Items().size())) {
        const std::wstring existing = Utf8ToWide(
            document_.Items()[static_cast<std::size_t>(editingTextIndex_)].text);
        SetWindowTextW(editControl_, existing.c_str());
        SendMessageW(editControl_, EM_SETSEL, 0, -1);
    }

    ShowWindow(editorWindow_, SW_SHOW);
    SetForegroundWindow(editorWindow_);
    SetFocus(editControl_);
    dirty_ = true;
}

void ScreenshotOverlay::CommitTextEditor() {
    if (editControl_ == nullptr) {
        return;
    }

    const int length = GetWindowTextLengthW(editControl_);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(editControl_, text.data(), length + 1);
        text.resize(static_cast<std::size_t>(length));
    }

    const std::string utf8 = WideToUtf8(text);
    const int editIndex = editingTextIndex_;
    editingTextIndex_ = -1;
    DestroyTextEditor();
    if (editIndex >= 0) {
        if (!utf8.empty()) {
            document_.ReplaceText(static_cast<std::size_t>(editIndex), utf8);
        } else {
            // 清空内容 = 删掉这条。
            document_.RemoveAnnotation(static_cast<std::size_t>(editIndex));
        }
    } else if (!utf8.empty()) {
        AddTextAnnotation(textPoint_, utf8);
    }
    dirty_ = true;
}

void ScreenshotOverlay::CancelTextEditor() {
    editingTextIndex_ = -1;
    DestroyTextEditor();
    dirty_ = true;
}

void ScreenshotOverlay::DestroyTextEditor() {
    if (editControl_ != nullptr) {
        SetWindowLongPtrW(editControl_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_oldEditProc));
        editControl_ = nullptr;
    }
    if (editorWindow_ != nullptr) {
        DestroyWindow(editorWindow_);
        editorWindow_ = nullptr;
    }
    if (editorBrush_ != nullptr) {
        DeleteObject(static_cast<HBRUSH>(editorBrush_));
        editorBrush_ = nullptr;
    }
    g_editorOwner = nullptr;
    g_oldEditProc = nullptr;
}

LRESULT CALLBACK ScreenshotOverlay::EditorProc(HWND window, UINT message, WPARAM wParam,
                                               LPARAM lParam) {
    if (message == WM_CTLCOLOREDIT && g_editorOwner != nullptr) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        const Color color = g_editorOwner->style_.color;
        SetTextColor(dc, RGB(static_cast<int>(color.r * 255), static_cast<int>(color.g * 255),
                             static_cast<int>(color.b * 255)));
        SetBkColor(dc, RGB(20, 25, 34));

        if (g_editorOwner->editorBrush_ == nullptr) {
            g_editorOwner->editorBrush_ = CreateSolidBrush(RGB(20, 25, 34));
        }
        return reinterpret_cast<LRESULT>(g_editorOwner->editorBrush_);
    }

    if (message == WM_ERASEBKGND) {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect,
                 static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT CALLBACK ScreenshotOverlay::EditorEditProc(HWND window, UINT message, WPARAM wParam,
                                                   LPARAM lParam) {
    if (g_editorOwner != nullptr && message == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            g_editorOwner->CommitTextEditor();
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            g_editorOwner->CancelTextEditor();
            return 0;
        }
    }
    return CallWindowProcW(g_oldEditProc, window, message, wParam, lParam);
}

// ---- 键盘钩子 ----

LRESULT CALLBACK ScreenshotOverlay::KeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_overlay == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    // 文字弹窗开着时一律放过：那是真的 EDIT 控件，输入法与按键都归它。
    if (g_overlay->editorWindow_ != nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* keyboard = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    const DWORD key = keyboard->vkCode;
    const bool control = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool win = ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0;
    const bool bare = !control && !alt && !win;

    // 覆盖层吞掉按键，所以全局注册的「贴图」热键要在这里自己匹配一次。
    const core::HotkeySpec pinHotkey = g_overlay->options_.pinHotkey;
    if (pinHotkey.key != core::KeySymbol::None) {
        core::HotkeySpec pressed;
        pressed.key = platform::win32::FromNativeVirtualKey(key);
        if (control) {
            pressed.modifiers |= core::kModControl;
        }
        if (alt) {
            pressed.modifiers |= core::kModAlt;
        }
        if (shift) {
            pressed.modifiers |= core::kModShift;
        }
        if (win) {
            pressed.modifiers |= core::kModWin;
        }
        if (pressed.key != core::KeySymbol::None && pressed.key == pinHotkey.key &&
            (pressed.modifiers & core::kStateModifiers) ==
                (pinHotkey.modifiers & core::kStateModifiers)) {
            PostMessageW(static_cast<HWND>(g_overlay->window_->Handle()), kMsgPin, 0, 0);
            return 1;
        }
    }

    // kTools 的下标：0 笔、1 矩形、2 椭圆、3 箭头、4 文字、5 荧光、6 马赛克、7 序号、8 橡皮。
    UINT message = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
    if (key == VK_ESCAPE) {
        message = kMsgEscape;
    } else if (key == VK_RETURN) {
        message = kMsgCopy;
    } else if (control && shift && key == 'Z') {
        message = kMsgRedo;
    } else if (control && key == 'Z') {
        message = kMsgUndo;
    } else if (control && key == 'Y') {
        message = kMsgRedo;
    } else if (control && key == 'C') {
        message = kMsgCopy;
    } else if (control && key == 'S') {
        message = kMsgSave;
    } else if (control && key == 'T') {
        message = kMsgPin;
    } else if (control && key == 'B') {
        message = kMsgTextStyle;
        wparam = 1;
    } else if (control && key == 'I') {
        message = kMsgTextStyle;
        wparam = 2;
    } else if (bare && key == 'C') {
        // 1.x：裸 C 随时复制光标下的颜色，Shift 切换 hex / rgb()。
        message = kMsgCopyColor;
        wparam = shift ? 1 : 0;
    } else if (bare && key == 'V') {
        message = kMsgSelect;
    } else if (bare && key == 'F') {
        message = kMsgFill;
    } else if (bare && (key == VK_OEM_4 || key == '[' || key == VK_OEM_6 || key == ']')) {
        message = kMsgOption;
        wparam = static_cast<WPARAM>(key == VK_OEM_4 || key == '['
                                         ? static_cast<int>(Action::ThicknessDown)
                                         : static_cast<int>(Action::ThicknessUp));
    } else if (bare && (key == 'R' || key == 'E' || key == 'A' || key == 'P' || key == 'T' ||
                        key == 'H' || key == 'M' || key == 'N' || key == 'X')) {
        // 1.x 的字母快捷键；Shift+R / Shift+E 直接画实心形状。
        message = kMsgTool;
        switch (key) {
        case 'P': wparam = 0; break;
        case 'R': wparam = 1; break;
        case 'E': wparam = 2; break;
        case 'A': wparam = 3; break;
        case 'T': wparam = 4; break;
        case 'H': wparam = 5; break;
        case 'M': wparam = 6; break;
        case 'N': wparam = 7; break;
        case 'X': wparam = 8; break;
        default: break;
        }
        lparam = shift && (key == 'R' || key == 'E') ? 1 : 0;
    } else if (key == VK_DELETE) {
        message = kMsgDelete;
    } else if (key == VK_SPACE) {
        message = kMsgAnnotate;
    } else if (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN) {
        // 方向键微调：1 px，按 Shift 是 10 px（1.x 的选区微调）。
        message = kMsgNudge;
        wparam = static_cast<WPARAM>(key);
        lparam = shift ? 10 : 1;
    } else if (key >= '1' && key <= '9') {
        message = kMsgTool;
        wparam = static_cast<WPARAM>(key - '1');
    }

    if (message == 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    PostMessageW(static_cast<HWND>(g_overlay->window_->Handle()), message, wparam, lparam);
    return 1;
}

void ScreenshotOverlay::InstallKeyHook() {
    if (g_keyHook == nullptr) {
        g_keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &ScreenshotOverlay::KeyHook,
                                      GetModuleHandleW(nullptr), 0);
    }
}

void ScreenshotOverlay::UninstallKeyHook() {
    if (g_keyHook != nullptr) {
        UnhookWindowsHookEx(g_keyHook);
        g_keyHook = nullptr;
    }
}

// ---- 导出 ----

bool ScreenshotOverlay::ComposeSelection(std::vector<std::uint8_t>& bgra, int& width,
                                         int& height) {
    if (!active_ || backend_ == nullptr || selection_.Bounds().IsEmpty()) {
        return false;
    }

    const Rect selection = selection_.Bounds();
    width = static_cast<int>(selection.Width()) & ~1;
    height = static_cast<int>(selection.Height()) & ~1;
    if (width < 2 || height < 2) {
        return false;
    }

    core::FramePlan plan;
    plan.Add(core::Blit{ capturedHandle_,
                         Rect{ selection.left - windowX_, selection.top - windowY_,
                               selection.left - windowX_ + width,
                               selection.top - windowY_ + height },
                         Rect{ 0.0f, 0.0f, static_cast<float>(width),
                               static_cast<float>(height) },
                         1.0f });

    AnnotationPlanOptions options;
    options.rasterizer = rasterizer_.get();
    options.fontFamily = options_.fontFamily;
    options.scale = 1.0f;
    options.sizeScale = uiScale_;
    // 标注点是选区局部坐标，而导出目标就是从选区左上角开始的那一块 → 平移为 0。
    // 以前写成 -(selection.left - windowX_)，标注整体被推到画面外，导出的图一直没有标注。
    options.origin = Point{ 0.0f, 0.0f };
    core::AppendAnnotationPlan(document_.Items(), options, plan);

    // 同样走独立离屏目标：复制/保存/贴图/识别都不再拆掉窗口视觉（以前会闪一下）。
    const int composedWidth = width;
    const int composedHeight = height;
    return backend_->RenderOffscreen(plan, composedWidth, composedHeight, bgra, width, height) &&
           width == composedWidth && height == composedHeight;
}

bool ScreenshotOverlay::MosaicBasePixelForTest(float localX, float localY,
                                               std::uint8_t bgra[4]) const {
    if (!mosaicBaseValid_ || mosaicBaseWidth_ <= 0 || mosaicBaseHeight_ <= 0 ||
        mosaicBase_.empty()) {
        return false;
    }
    const int x = static_cast<int>(localX);
    const int y = static_cast<int>(localY);
    if (x < 0 || y < 0 || x >= mosaicBaseWidth_ || y >= mosaicBaseHeight_) {
        return false;
    }
    const std::size_t offset =
        (static_cast<std::size_t>(y) * static_cast<std::size_t>(mosaicBaseWidth_) +
         static_cast<std::size_t>(x)) * 4;
    std::memcpy(bgra, mosaicBase_.data() + offset, 4);
    return true;
}

bool ScreenshotOverlay::CopySelection() {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!ComposeSelection(bgra, width, height)) {
        errorKind_ = "compose";
        return false;
    }
    if (!platform::win32::CopyBgraToClipboard(window_ != nullptr ? window_->Handle() : nullptr,
                                              bgra.data(), width, height, width * 4)) {
        errorKind_ = "clipboard";
        return false;
    }
    return true;
}

std::wstring ScreenshotOverlay::ResolveSavePath() const {
    std::wstring directory = options_.saveDirectory;
    if (directory.empty()) {
        wchar_t pictures[MAX_PATH] = {};
        if (SHGetFolderPathW(nullptr, CSIDL_MYPICTURES, nullptr, SHGFP_TYPE_CURRENT, pictures) ==
            S_OK) {
            directory = std::wstring(pictures) + L"\\HaloCap";
        } else {
            wchar_t profile[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH) == 0) {
                return {};
            }
            directory = std::wstring(profile) + L"\\Pictures\\HaloCap";
        }
    }

    EnsureDirectory(directory);
    const std::wstring extension = options_.jpeg ? L".jpg" : L".png";
    std::wstring path = directory + L"\\" + TimestampName() + extension;

    int index = 2;
    while (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        wchar_t buffer[64] = {};
        std::swprintf(buffer, 64, L"_%d", index++);
        path = directory + L"\\" + TimestampName() + buffer + extension;
    }
    return path;
}

bool ScreenshotOverlay::SaveSelection(std::string* pathOut) {
    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!ComposeSelection(bgra, width, height)) {
        errorKind_ = "compose";
        return false;
    }

    const std::wstring path = ResolveSavePath();
    if (path.empty()) {
        errorKind_ = "path";
        return false;
    }

    const bool ok = options_.jpeg
                        ? platform::win32::SaveBgraJpeg(path, bgra.data(), width, height,
                                                        width * 4, 90)
                        : platform::win32::SaveBgraPng(path, bgra.data(), width, height,
                                                       width * 4);
    if (!ok) {
        errorKind_ = "encode";
        return false;
    }
    if (pathOut != nullptr) {
        *pathOut = WideToUtf8(path);
    }
    return true;
}

bool ScreenshotOverlay::PinSelection() {
    if (!options_.onPin) {
        errorKind_ = "nopin";
        return false;
    }

    std::vector<std::uint8_t> bgra;
    int width = 0;
    int height = 0;
    if (!ComposeSelection(bgra, width, height)) {
        errorKind_ = "compose";
        return false;
    }

    const Rect selection = selection_.Bounds();
    options_.onPin(bgra.data(), width, height, static_cast<int>(selection.left),
                   static_cast<int>(selection.top));
    return true;
}

bool ScreenshotOverlay::GrabDesktopToPng(const std::wstring& path) {
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = screenWidth_;
    info.bmiHeader.biHeight = -screenHeight_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap == nullptr || bits == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }

    HGDIOBJ oldBitmap = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, screenWidth_, screenHeight_, screen, windowX_,
                               windowY_, SRCCOPY | CAPTUREBLT);
    SelectObject(memory, oldBitmap);

    bool ok = false;
    if (copied) {
        ok = platform::win32::SaveBgraPng(path, static_cast<const std::uint8_t*>(bits), screenWidth_,
                                          screenHeight_, screenWidth_ * 4);
    }

    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    return ok;
}

void ScreenshotOverlay::DebugSelect(IntRect rect) {
    if (!active_) {
        return;
    }

    selection_.SetLimit(Rect{ static_cast<float>(windowX_), static_cast<float>(windowY_),
                              static_cast<float>(windowX_ + screenWidth_),
                              static_cast<float>(windowY_ + screenHeight_) });
    selection_.Begin(Point{ static_cast<float>(rect.left), static_cast<float>(rect.top) });
    selection_.Update(Point{ static_cast<float>(rect.right), static_cast<float>(rect.bottom) },
                      false);
    selection_.Commit();
    if (!selection_.Bounds().IsEmpty()) {
        phase_ = Phase::Annotating;
        annotating_ = false;
    }
    ClearHoverCandidate();
    dirty_ = true;
}

void* ScreenshotOverlay::WindowHandle() const {
    return window_ != nullptr ? window_->Handle() : nullptr;
}

IntRect ScreenshotOverlay::SelectionRectPx() const {
    const Rect rect = selection_.Bounds();
    return IntRect{ static_cast<int>(rect.left), static_cast<int>(rect.top),
                    static_cast<int>(rect.right), static_cast<int>(rect.bottom) };
}

}
