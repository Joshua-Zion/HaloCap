#include "screenshot_overlay.h"

#include "image_export.h"
#include "desktop_grab.h"
#include "mosaic.h"

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
constexpr UINT kMsgFullScreen = WM_APP + 7;
constexpr UINT kMsgDelete = WM_APP + 8;
constexpr UINT kMsgSelect = WM_APP + 9;
constexpr UINT kMsgFill = WM_APP + 10;

constexpr int kToolbarToolWidth = 34;
constexpr int kToolbarColorWidth = 22;
constexpr int kToolbarHeight = 44;
constexpr int kToolbarGap = 4;
constexpr int kToolbarPadding = 8;
constexpr int kHandleSize = 8;

const char* kToolLabels[] = { "笔", "矩", "椭", "箭", "字", "荧", "马", "号", "橡" };
const AnnotationTool kTools[] = {
    AnnotationTool::Pen,       AnnotationTool::Rectangle, AnnotationTool::Ellipse,
    AnnotationTool::Arrow,     AnnotationTool::Text,      AnnotationTool::Highlighter,
    AnnotationTool::Mosaic,    AnnotationTool::Step,      AnnotationTool::Eraser,
};
const char* kActionLabels[] = { "撤", "重", "复", "存", "贴", "关" };

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
    platform::win32::DesktopImage desktop;
    if (!platform::win32::GrabPrimaryDesktop(desktop)) {
        errorKind_ = "grab";
        Close();
        return false;
    }

    windowX_ = 0;
    windowY_ = 0;
    screenWidth_ = desktop.width & ~1;
    screenHeight_ = desktop.height & ~1;
    if (screenWidth_ < 2 || screenHeight_ < 2) {
        errorKind_ = "invalid";
        Close();
        return false;
    }

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
    window_->SetMessageHandler([this](HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        return HandleMessage(window, message, wParam, lParam);
    });
    if (!backend_->AttachWindow(window_->Handle(), screenWidth_, screenHeight_)) {
        errorKind_ = "attach";
        Close();
        return false;
    }

    InstallKeyHook();
    g_overlay = this;
    active_ = true;
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

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;

    case WM_TIMER:
        if (wParam == 1) {
            StepHoverAnimation();
            return 0;
        }
        break;

    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (tool_ == AnnotationTool::Pen) {
                style_.thickness =
                    std::clamp(style_.thickness + (delta > 0 ? 1.0f : -1.0f), 1.0f, 24.0f);
            } else if (tool_ == AnnotationTool::Highlighter) {
                style_.thickness =
                    std::clamp(style_.thickness + (delta > 0 ? 2.0f : -2.0f), 6.0f, 40.0f);
            }
            dirty_ = true;
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        OnMouseMove(ToScreen(point, windowX_, windowY_));
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
        selectMode_ = true;
        selectedAnnotation_ = -1;
        dirty_ = true;
        return 0;

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
            selectMode_ = false;
            selectedAnnotation_ = -1;
            SetTool(kTools[wParam]);
        }
        return 0;

    case kMsgFullScreen:
        // 空格 = 全屏标注：没有选区时把整块屏幕当成选区（1.x 的全屏标注入口）。
        if (selection_.Bounds().IsEmpty()) {
            DebugSelect(IntRect{ windowX_, windowY_, windowX_ + screenWidth_,
                                 windowY_ + screenHeight_ });
        }
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void ScreenshotOverlay::OnMouseMove(POINT point) {
    lastMouse_ = point;
    mouseInside_ = true;

    int hovered = -1;
    for (std::size_t i = 0; i < toolbar_.size(); ++i) {
        const RECT& rect = toolbar_[i].rect;
        if (point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
            point.y < rect.bottom) {
            hovered = static_cast<int>(i);
            break;
        }
    }
    if (hovered != hoveredToolbar_) {
        hoveredToolbar_ = hovered;
        hoverAnimating_ = true;
        SetTimer(static_cast<HWND>(window_->Handle()), 1, 16, nullptr);
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

    if (drawing_) {
        const Rect bounds = selection_.Bounds();
        const Point local{ static_cast<float>(point.x) - bounds.left,
                           static_cast<float>(point.y) - bounds.top };

        if (tool_ == AnnotationTool::Eraser) {
            document_.EraseAt(local, core::annotation_defaults::kEraserSize * 0.5f);
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
    }
}

void ScreenshotOverlay::OnLeftButtonDown(POINT point) {
    if (options_.mode == OverlayMode::RecordRegion) {
        if (phase_ == Phase::Selecting) {
            selecting_ = true;
            selection_.Begin(core::Point{ static_cast<float>(point.x),
                                          static_cast<float>(point.y) });
            dirty_ = true;
        }
        return;
    }

    // 选项栏在当前工具下展开时优先命中。
    if (optionBounds_.right > optionBounds_.left && point.x >= optionBounds_.left &&
        point.x < optionBounds_.right && point.y >= optionBounds_.top &&
        point.y < optionBounds_.bottom) {
        for (const OptionButton& option : optionButtons_) {
            if (point.x >= option.rect.left && point.x < option.rect.right &&
                point.y >= option.rect.top && point.y < option.rect.bottom) {
                HandleOption(option.action);
                return;
            }
        }
        return;
    }

    // 工具栏优先：它在选区之上，点按钮不能落成标注。
    if (point.x >= toolbarBounds_.left && point.x < toolbarBounds_.right &&
        point.y >= toolbarBounds_.top && point.y < toolbarBounds_.bottom) {
        for (const ToolbarButton& button : toolbar_) {
            if (point.x >= button.rect.left && point.x < button.rect.right &&
                point.y >= button.rect.top && point.y < button.rect.bottom) {
                if (button.enabled) {
                    HandleToolbar(button);
                }
                return;
            }
        }
        return;
    }

    const Point screen{ static_cast<float>(point.x), static_cast<float>(point.y) };

    if (phase_ == Phase::Selecting) {
        selecting_ = true;
        selection_.Begin(screen);
        dirty_ = true;
        return;
    }

    const core::SelectionHandle handle = selection_.HitTest(screen);
    if (handle != core::SelectionHandle::None && handle != core::SelectionHandle::Move) {
        resizing_ = true;
        resizeHandle_ = handle;
        return;
    }

    if (selectMode_) {
        const core::Rect bounds = selection_.Bounds();
        if (!bounds.Contains(screen)) {
            selectedAnnotation_ = -1;
            dirty_ = true;
            return;
        }
        const core::Point local{ screen.x - bounds.left, screen.y - bounds.top };
        selectedAnnotation_ = -1;
        for (int index = static_cast<int>(document_.Items().size()) - 1; index >= 0; --index) {
            if (core::HitTest(document_.Items()[static_cast<std::size_t>(index)], local, 3.0f)) {
                selectedAnnotation_ = index;
                break;
            }
        }
        if (selectedAnnotation_ >= 0) {
            movingAnnotation_ = true;
            moveOrigin_ = point;
        } else {
            movingSelection_ = true;
            lastPoint_ = point;
        }
        dirty_ = true;
        return;
    }

    if (!selection_.Bounds().Contains(screen)) {
        return;
    }

    const Rect bounds = selection_.Bounds();
    const Point local{ screen.x - bounds.left, screen.y - bounds.top };

    if (tool_ == AnnotationTool::Eraser) {
        drawing_ = true;
        document_.EraseAt(local, core::annotation_defaults::kEraserSize * 0.5f);
        dirty_ = true;
        return;
    }

    if (tool_ == AnnotationTool::Text) {
        OpenTextEditor(local);
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
        if (!selection_.Bounds().IsEmpty()) {
            phase_ = Phase::Annotating;
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
}

void ScreenshotOverlay::HandleToolbar(const ToolbarButton& button) {
    const bool hasSelection = !selection_.Bounds().IsEmpty();

    switch (button.action) {
    case Action::ToolSelect:
        selectMode_ = true;
        selectedAnnotation_ = -1;
        break;

    case Action::ToolPen:
    case Action::ToolRectangle:
    case Action::ToolEllipse:
    case Action::ToolArrow:
    case Action::ToolText:
    case Action::ToolHighlighter:
    case Action::ToolMosaic:
    case Action::ToolStep:
    case Action::ToolEraser:
        if (hasSelection) {
            selectMode_ = false;
            selectedAnnotation_ = -1;
            SetTool(kTools[button.toolIndex]);
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

    if (selectMode_) {
        return;
    }

    const auto addOption = [&](Action action, const std::string& text, int width, bool active) {
        OptionButton button;
        button.action = action;
        button.text = text;
        button.active = active;
        button.rect = RECT{ 0, 0, width, 32 };
        optionButtons_.push_back(button);
    };

    switch (tool_) {
    case core::AnnotationTool::Rectangle:
    case core::AnnotationTool::Ellipse:
        addOption(Action::FillOff, "空心", 48, !style_.filled);
        addOption(Action::FillOn, "实心", 48, style_.filled);
        break;
    case core::AnnotationTool::Pen:
    case core::AnnotationTool::Highlighter:
        addOption(Action::ThicknessDown, "-", 28, false);
        addOption(Action::None, std::to_string(static_cast<int>(std::lround(style_.thickness))),
                  34, false);
        addOption(Action::ThicknessUp, "+", 28, false);
        break;
    case core::AnnotationTool::Text:
        addOption(Action::TextSizeDown, "-", 28, false);
        addOption(Action::None, std::to_string(static_cast<int>(std::lround(style_.textSize))), 34,
                  false);
        addOption(Action::TextSizeUp, "+", 28, false);
        addOption(Action::Bold, "B", 28, style_.bold);
        addOption(Action::Italic, "I", 28, style_.italic);
        break;
    case core::AnnotationTool::Mosaic:
        addOption(Action::MosaicDown, "-", 28, false);
        addOption(Action::None, std::to_string(style_.mosaicBlockPx), 34, false);
        addOption(Action::MosaicUp, "+", 28, false);
        break;
    case core::AnnotationTool::Arrow:
        addOption(Action::ArrowEnd, "→", 34, style_.arrowHead == core::ArrowHeadMode::End);
        addOption(Action::ArrowBoth, "↔", 34, style_.arrowHead == core::ArrowHeadMode::Both);
        addOption(Action::ArrowNone, "—", 34, style_.arrowHead == core::ArrowHeadMode::None);
        break;
    default:
        break;
    }

    if (optionButtons_.empty()) {
        return;
    }

    int width = kToolbarPadding * 2;
    for (const OptionButton& option : optionButtons_) {
        width += option.rect.right;
    }
    width += (static_cast<int>(optionButtons_.size()) - 1) * 4;

    int x = static_cast<std::int32_t>(toolbarBounds_.left);
    int y = static_cast<std::int32_t>(toolbarBounds_.bottom) + 4;
    if (y + 40 > screenHeight_) {
        y = static_cast<std::int32_t>(toolbarBounds_.top) - 40;
    }
    x = std::clamp(x, 4, std::max(4, screenWidth_ - width - 4));
    y = std::clamp(y, 4, std::max(4, screenHeight_ - 40));

    int cursor = x + kToolbarPadding;
    for (OptionButton& option : optionButtons_) {
        option.rect = RECT{ cursor, y + 4, cursor + option.rect.right, y + 36 };
        cursor += option.rect.right + 4;
    }
    optionBounds_ = RECT{ x, y, x + width, y + 40 };
}

void ScreenshotOverlay::RenderOptionBar(core::FramePlan& plan) {
    if (optionButtons_.empty() || rasterizer_ == nullptr) {
        return;
    }
    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);
    const Rect background{ static_cast<float>(optionBounds_.left) - ox,
                           static_cast<float>(optionBounds_.top) - oy,
                           static_cast<float>(optionBounds_.right) - ox,
                           static_cast<float>(optionBounds_.bottom) - oy };
    plan.Add(core::FillRoundedRect{ background, 10.0f, Color{ 0.98f, 0.976f, 0.984f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ background, 10.0f, Color{ 0.0f, 0.0f, 0.0f, 0.13f }, 1.0f });

    for (const OptionButton& option : optionButtons_) {
        const Rect rect{ static_cast<float>(option.rect.left) - ox,
                         static_cast<float>(option.rect.top) - oy,
                         static_cast<float>(option.rect.right) - ox,
                         static_cast<float>(option.rect.bottom) - oy };
        if (option.active) {
            plan.Add(core::FillRoundedRect{ rect, 7.0f, Color{ 0.184f, 0.486f, 0.965f, 1.0f } });
        } else if (option.action != Action::None) {
            plan.Add(core::FillRoundedRect{ rect, 7.0f, Color{ 0.0f, 0.0f, 0.0f, 0.04f } });
        }

        if (option.text.empty()) {
            continue;
        }
        const bool valueOnly = option.action == Action::None;
        const core::FontSpec spec{ options_.fontFamily, valueOnly ? 400 : 600, 12.0f,
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
    if (!selecting_ || rasterizer_ == nullptr || selection_.Bounds().IsEmpty()) {
        return;
    }
    const core::Rect selection = selection_.Bounds();
    const std::string text = std::to_string(static_cast<int>(selection.Width())) + " × " +
                             std::to_string(static_cast<int>(selection.Height()));
    const core::FontSpec spec{ options_.fontFamily, 400, 11.0f,
                               Color{ 0.235f, 0.259f, 0.314f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(text, spec, core::Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }
    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);
    const float width = measured.destination.Width() + 16.0f;
    const float left = selection.left - ox;
    float top = selection.top - oy - 28.0f;
    if (top < 4.0f) {
        top = selection.top - oy + 8.0f;
    }
    plan.Add(core::FillRoundedRect{ core::Rect{ left, top, left + width, top + 24.0f }, 6.0f,
                                    Color{ 1.0f, 1.0f, 1.0f, 0.96f } });
    plan.Add(core::StrokeRoundedRect{ core::Rect{ left, top, left + width, top + 24.0f }, 6.0f,
                                      Color{ 0.0f, 0.0f, 0.0f, 0.12f }, 1.0f });
    const core::TextCell cell =
        rasterizer_->LayOut(text, spec, core::Point{ left + 8.0f, top + 5.0f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

void ScreenshotOverlay::RenderMagnifier(core::FramePlan& plan) {
    if (!mouseInside_ || rasterizer_ == nullptr || baseBgra_.empty() ||
        selection_.Bounds().IsEmpty()) {
        return;
    }
    if (lastMouse_.x >= toolbarBounds_.left && lastMouse_.x < toolbarBounds_.right &&
        lastMouse_.y >= toolbarBounds_.top && lastMouse_.y < toolbarBounds_.bottom) {
        return;
    }
    if (optionBounds_.right > optionBounds_.left && lastMouse_.x >= optionBounds_.left &&
        lastMouse_.x < optionBounds_.right && lastMouse_.y >= optionBounds_.top &&
        lastMouse_.y < optionBounds_.bottom) {
        return;
    }

    constexpr int kImage = 128;
    constexpr int kCardWidth = 136;
    constexpr int kCardHeight = 172;
    int x = static_cast<int>(lastMouse_.x) + 20;
    int y = static_cast<int>(lastMouse_.y) + 20;
    x = std::clamp(x, 4, screenWidth_ - kCardWidth - 4);
    y = std::clamp(y, 4, screenHeight_ - kCardHeight - 4);

    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);
    const core::Rect card{ static_cast<float>(x) - ox, static_cast<float>(y) - oy,
                           static_cast<float>(x + kCardWidth) - ox,
                           static_cast<float>(y + kCardHeight) - oy };
    plan.Add(core::FillRoundedRect{ card, 10.0f, Color{ 1.0f, 1.0f, 1.0f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ card, 10.0f, Color{ 0.0f, 0.0f, 0.0f, 0.14f }, 1.0f });

    const float sampleX = lastMouse_.x - windowX_;
    const float sampleY = lastMouse_.y - windowY_;
    const core::Rect source{ sampleX - 8.0f, sampleY - 8.0f, sampleX + 8.0f, sampleY + 8.0f };
    const core::Rect destination{ static_cast<float>(x + 4) - ox, static_cast<float>(y + 4) - oy,
                                  static_cast<float>(x + 4 + kImage) - ox,
                                  static_cast<float>(y + 4 + kImage) - oy };
    plan.Add(core::Blit{ capturedHandle_, source, destination, 1.0f });

    // 中心像素的十字与颜色环。
    const float centerX = (destination.left + destination.right) * 0.5f;
    const float centerY = (destination.top + destination.bottom) * 0.5f;
    plan.Add(core::StrokeRect{ core::Rect{ centerX - 8.0f, centerY - 8.0f, centerX + 8.0f,
                                           centerY + 8.0f },
                               Color{ 1.0f, 1.0f, 1.0f, 0.9f }, 1.0f });
    plan.Add(core::StrokeRect{ core::Rect{ centerX - 9.0f, centerY - 9.0f, centerX + 9.0f,
                                           centerY + 9.0f },
                               Color{ 0.0f, 0.0f, 0.0f, 0.7f }, 1.0f });

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
    std::snprintf(coordinates, sizeof(coordinates), "%d, %d", px, py);

    const float footerY = static_cast<float>(y + 140) - oy;
    plan.Add(core::FillEllipse{ core::Rect{ static_cast<float>(x + 10) - ox, footerY + 1.0f,
                                            static_cast<float>(x + 22) - ox, footerY + 13.0f },
                                Color{ r / 255.0f, g / 255.0f, b / 255.0f, 1.0f } });
    plan.Add(core::StrokeEllipse{ core::Rect{ static_cast<float>(x + 10) - ox, footerY + 1.0f,
                                              static_cast<float>(x + 22) - ox, footerY + 13.0f },
                                  Color{ 0.0f, 0.0f, 0.0f, 0.2f }, 1.0f });

    const core::FontSpec hexSpec{ options_.fontFamily, 600, 11.0f,
                                  Color{ 0.15f, 0.17f, 0.21f, 1.0f } };
    const core::FontSpec coordSpec{ options_.fontFamily, 400, 10.0f,
                                    Color{ 0.42f, 0.45f, 0.50f, 1.0f } };
    const core::TextCell hexCell =
        rasterizer_->LayOut(hex, hexSpec, core::Point{ static_cast<float>(x + 28) - ox,
                                                       footerY });
    if (hexCell.atlas != 0) {
        plan.Add(hexCell.ToBlit());
    }
    const core::TextCell coordCell =
        rasterizer_->LayOut(coordinates, coordSpec,
                            core::Point{ static_cast<float>(x + 28) - ox, footerY + 15.0f });
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

void ScreenshotOverlay::LayoutToolbar() {
    toolbar_.clear();

    const int toolCount = static_cast<int>(std::size(kToolLabels));
    const int colorCount = core::annotation_defaults::kPaletteCount;
    const int actionCount = static_cast<int>(std::size(kActionLabels));

    int width = kToolbarPadding * 2;
    width += (toolCount + 1) * kToolbarToolWidth;
    width += colorCount * kToolbarColorWidth;
    width += actionCount * kToolbarToolWidth;
    width += (toolCount + 1 + colorCount + actionCount + 2 - 1) * kToolbarGap;

    const Rect selection = selection_.Bounds();
    int x = 0;
    int y = 0;
    if (selection.IsEmpty()) {
        x = (screenWidth_ - width) / 2;
        y = 20;
    } else {
        x = static_cast<int>(selection.left);
        y = static_cast<int>(selection.top) - kToolbarHeight - 10;
        if (y < 4) {
            y = static_cast<int>(selection.bottom) + 10;
        }
        x = std::clamp(x, 4, std::max(4, screenWidth_ - width - 4));
        y = std::clamp(y, 4, std::max(4, screenHeight_ - kToolbarHeight - 4));
    }

    int cursorX = x + kToolbarPadding;
    const int cursorY = y + 1;

    const auto addButton = [&](Action action, int payload, int toolIndex, const char* label,
                               int buttonWidth, bool enabled, bool active,
                               ToolbarIcon icon = ToolbarIcon::None) {
        ToolbarButton button;
        button.rect = RECT{ cursorX, y + 1, cursorX + buttonWidth, y + kToolbarHeight - 1 };
        button.action = action;
        button.payload = payload;
        button.toolIndex = toolIndex;
        button.label = label;
        button.icon = icon;
        button.enabled = enabled;
        button.active = active;
        toolbar_.push_back(button);
        cursorX += buttonWidth + kToolbarGap;
    };

    const ToolbarIcon toolIcons[] = {
        ToolbarIcon::Pen,       ToolbarIcon::Rectangle, ToolbarIcon::Ellipse,
        ToolbarIcon::Arrow,     ToolbarIcon::Text,      ToolbarIcon::Highlighter,
        ToolbarIcon::Mosaic,    ToolbarIcon::Step,      ToolbarIcon::Eraser,
    };
    addButton(Action::ToolSelect, 0, -1, "", kToolbarToolWidth, true, selectMode_,
              ToolbarIcon::Select);

    for (int i = 0; i < toolCount; ++i) {
        addButton(static_cast<Action>(static_cast<int>(Action::ToolPen) + i), 0, i, "",
                  kToolbarToolWidth, !selection.IsEmpty(), !selectMode_ && kTools[i] == tool_,
                  toolIcons[i]);
    }

    addButton(Action::None, 0, -1, "", 1, false, false, ToolbarIcon::Separator);

    for (int i = 0; i < colorCount; ++i) {
        const bool active = style_.color == core::annotation_defaults::PaletteColor(i);
        addButton(Action::Color, i, -1, "", kToolbarColorWidth, !selection.IsEmpty(), active);
    }

    addButton(Action::None, 0, -1, "", 1, false, false, ToolbarIcon::Separator);

    const Action actions[] = { Action::Undo,   Action::Redo, Action::Copy,
                               Action::Save,   Action::Pin,  Action::CloseOverlay };
    for (int i = 0; i < actionCount; ++i) {
        bool enabled = true;
        if (actions[i] == Action::Undo) {
            enabled = document_.CanUndo();
        } else if (actions[i] == Action::Redo) {
            enabled = document_.CanRedo();
        } else if (actions[i] != Action::CloseOverlay) {
            enabled = !selection.IsEmpty();
        }
        const ToolbarIcon actionIcons[] = { ToolbarIcon::Undo, ToolbarIcon::Redo,
                                            options_.mode == OverlayMode::Ocr ? ToolbarIcon::Text
                                                                              : ToolbarIcon::Copy,
                                            ToolbarIcon::Save, ToolbarIcon::Pin,
                                            ToolbarIcon::Close };
        addButton(actions[i], 0, -1, "", kToolbarToolWidth, enabled, false, actionIcons[i]);
    }

    toolbarBounds_ = RECT{ x, y, x + width, y + kToolbarHeight };
}

void ScreenshotOverlay::RenderToolbar(core::FramePlan& plan) {
    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);

    const Rect background{ static_cast<float>(toolbarBounds_.left) - ox,
                           static_cast<float>(toolbarBounds_.top) - oy,
                           static_cast<float>(toolbarBounds_.right) - ox,
                           static_cast<float>(toolbarBounds_.bottom) - oy };
    // 1.x 的工具栏：浅色 #FAF8F9FB、10 px 圆角、#22000000 边框。
    plan.Add(core::FillRoundedRect{ background, 10.0f, Color{ 0.98f, 0.976f, 0.984f, 0.98f } });
    plan.Add(core::StrokeRoundedRect{ background, 10.0f, Color{ 0.0f, 0.0f, 0.0f, 0.13f }, 1.0f });

    for (const ToolbarButton& button : toolbar_) {
        const Rect rect{ static_cast<float>(button.rect.left) - ox,
                         static_cast<float>(button.rect.top) - oy,
                         static_cast<float>(button.rect.right) - ox,
                         static_cast<float>(button.rect.bottom) - oy };

        if (button.active) {
            plan.Add(core::FillRoundedRect{ rect, 7.0f, Color{ 0.184f, 0.486f, 0.965f, 1.0f } });
        }

        if (button.icon == ToolbarIcon::Separator) {
            const float dividerX = rect.left + (rect.right - rect.left) * 0.5f;
            plan.Add(core::FillRect{ Rect{ dividerX, rect.top + 9.0f, dividerX + 1.0f,
                                           rect.bottom - 9.0f },
                                     Color{ 0.0f, 0.0f, 0.0f, 0.10f } });
            continue;
        }

        if (button.action == Action::Color) {
            const float inset = 4.0f;
            const Rect swatch{ rect.left + inset, rect.top + inset, rect.right - inset,
                               rect.bottom - inset };
            plan.Add(core::FillEllipse{ swatch,
                                        core::annotation_defaults::PaletteColor(button.payload) });
            if (button.active) {
                plan.Add(core::StrokeEllipse{ swatch, Color{ 0.184f, 0.486f, 0.965f, 1.0f },
                                              2.0f });
            }
            continue;
        }

        if (button.icon == ToolbarIcon::None) {
            continue;
        }

        const float alpha = button.enabled ? 1.0f : 0.4f;
        const Color ink = button.active
                              ? Color{ 1.0f, 1.0f, 1.0f, alpha }
                              : Color{ 0.235f, 0.259f, 0.314f, alpha };
        const float cx = (rect.left + rect.right) * 0.5f;
        const float cy = (rect.top + rect.bottom) * 0.5f;
        const auto line = [&](float x1, float y1, float x2, float y2, float thickness) {
            plan.Add(core::Segment{ Point{ cx + x1, cy + y1 }, Point{ cx + x2, cy + y2 }, ink,
                                    thickness });
        };
        const auto addGlyph = [&](const char* text, float size) {
            if (rasterizer_ == nullptr) {
                return;
            }
            const core::FontSpec spec{ options_.fontFamily, 700, size, ink };
            const core::TextCell measured = rasterizer_->LayOut(text, spec, Point{ 0.0f, 0.0f });
            if (measured.atlas == 0) {
                return;
            }
            const float width = measured.destination.Width();
            const float height = measured.destination.Height();
            const core::TextCell cell =
                rasterizer_->LayOut(text, spec, Point{ cx - width * 0.5f, cy - height * 0.5f });
            if (cell.atlas != 0) {
                plan.Add(cell.ToBlit());
            }
        };

        switch (button.icon) {
        case ToolbarIcon::Pen:
            line(-7.0f, 7.0f, 6.0f, -6.0f, 2.2f);
            line(6.0f, -6.0f, 9.0f, -9.0f, 2.2f);
            break;
        case ToolbarIcon::Rectangle:
            plan.Add(core::StrokeRect{
                Rect{ cx - 8.0f, cy - 6.0f, cx + 8.0f, cy + 6.0f }, ink, 1.8f });
            break;
        case ToolbarIcon::Ellipse:
            plan.Add(core::StrokeEllipse{
                Rect{ cx - 8.0f, cy - 6.0f, cx + 8.0f, cy + 6.0f }, ink, 1.8f });
            break;
        case ToolbarIcon::Arrow:
            line(-8.0f, 8.0f, 7.0f, -7.0f, 2.0f);
            line(7.0f, -7.0f, 1.0f, -7.0f, 2.0f);
            line(7.0f, -7.0f, 7.0f, -1.0f, 2.0f);
            break;
        case ToolbarIcon::Text:
            addGlyph("T", 17.0f);
            break;
        case ToolbarIcon::Highlighter: {
            Color highlight = ink;
            highlight.a = alpha * 0.55f;
            plan.Add(core::Segment{ Point{ cx - 8.0f, cy + 3.0f }, Point{ cx + 8.0f, cy - 3.0f },
                                    highlight, 6.0f });
            line(-8.0f, 8.0f, 8.0f, 2.0f, 1.4f);
            break;
        }
        case ToolbarIcon::Mosaic:
            for (int row = 0; row < 2; ++row) {
                for (int column = 0; column < 3; ++column) {
                    const float x = cx - 8.0f + column * 6.0f;
                    const float y = cy - 6.0f + row * 6.0f;
                    plan.Add(core::FillRect{ Rect{ x, y, x + 4.5f, y + 4.5f }, ink });
                }
            }
            break;
        case ToolbarIcon::Step:
            plan.Add(core::StrokeEllipse{ Rect{ cx - 8.0f, cy - 8.0f, cx + 8.0f, cy + 8.0f }, ink,
                                          1.8f });
            addGlyph("1", 10.0f);
            break;
        case ToolbarIcon::Eraser:
            line(-8.0f, 3.0f, 2.0f, -7.0f, 2.2f);
            line(-8.0f, 8.0f, 8.0f, -2.0f, 2.2f);
            line(-8.0f, 8.0f, 0.0f, 8.0f, 1.4f);
            break;
        case ToolbarIcon::Undo:
            line(7.0f, -6.0f, -3.0f, -6.0f, 2.0f);
            line(-3.0f, -6.0f, -7.0f, -2.0f, 2.0f);
            line(-7.0f, -2.0f, -3.0f, 2.0f, 2.0f);
            break;
        case ToolbarIcon::Redo:
            line(-7.0f, -6.0f, 3.0f, -6.0f, 2.0f);
            line(3.0f, -6.0f, 7.0f, -2.0f, 2.0f);
            line(7.0f, -2.0f, 3.0f, 2.0f, 2.0f);
            break;
        case ToolbarIcon::Copy:
            plan.Add(core::StrokeRect{ Rect{ cx - 8.0f, cy - 8.0f, cx + 3.0f, cy + 3.0f }, ink,
                                       1.6f });
            plan.Add(core::StrokeRect{ Rect{ cx - 3.0f, cy - 3.0f, cx + 8.0f, cy + 8.0f }, ink,
                                       1.6f });
            break;
        case ToolbarIcon::Save:
            line(0.0f, -8.0f, 0.0f, 3.0f, 2.0f);
            line(-4.0f, -1.0f, 0.0f, 3.0f, 2.0f);
            line(4.0f, -1.0f, 0.0f, 3.0f, 2.0f);
            line(-8.0f, 8.0f, -8.0f, 5.0f, 1.6f);
            line(-8.0f, 5.0f, 8.0f, 5.0f, 1.6f);
            line(8.0f, 5.0f, 8.0f, 8.0f, 1.6f);
            break;
        case ToolbarIcon::Pin:
            plan.Add(core::FillEllipse{ Rect{ cx - 4.0f, cy - 9.0f, cx + 4.0f, cy - 1.0f }, ink });
            line(0.0f, -1.0f, 0.0f, 6.0f, 2.0f);
            line(0.0f, 6.0f, 4.0f, 9.0f, 2.0f);
            break;
        case ToolbarIcon::Close:
            line(-7.0f, -7.0f, 7.0f, 7.0f, 2.0f);
            line(7.0f, -7.0f, -7.0f, 7.0f, 2.0f);
            break;
        case ToolbarIcon::Separator:
        case ToolbarIcon::None:
            break;
        }
    }
}

void ScreenshotOverlay::RenderSelection(core::FramePlan& plan) {
    const Rect selection = selection_.Bounds();
    if (selection.IsEmpty()) {
        return;
    }

    const float ox = static_cast<float>(windowX_);
    const float oy = static_cast<float>(windowY_);
    const Rect local{ selection.left - ox, selection.top - oy, selection.right - ox,
                      selection.bottom - oy };
    const Color dim{ 0.0f, 0.0f, 0.0f, 0.45f };
    const float width = static_cast<float>(screenWidth_);
    const float height = static_cast<float>(screenHeight_);

    // 选区之外压暗：四条矩形拼起来，不需要额外的遮罩纹理。
    plan.Add(core::FillRect{ Rect{ 0.0f, 0.0f, width, local.top }, dim });
    plan.Add(core::FillRect{ Rect{ 0.0f, local.bottom, width, height }, dim });
    plan.Add(core::FillRect{ Rect{ 0.0f, local.top, local.left, local.bottom }, dim });
    plan.Add(core::FillRect{ Rect{ local.right, local.top, width, local.bottom }, dim });

    // 1.x 的选区：蓝色 #2F7CF6 边框，把手是 7×7 的蓝色小方块。
    const Color accent{ 0.184f, 0.486f, 0.965f, 1.0f };
    plan.Add(core::StrokeRect{ local, accent, 1.5f });

    if (phase_ != Phase::Annotating) {
        return;
    }

    const float cx = (local.left + local.right) * 0.5f;
    const float cy = (local.top + local.bottom) * 0.5f;
    const Point handles[] = { { local.left, local.top },    { cx, local.top },
                              { local.right, local.top },   { local.right, cy },
                              { local.right, local.bottom }, { cx, local.bottom },
                              { local.left, local.bottom }, { local.left, cy } };

    for (const Point& handle : handles) {
        const Rect rect{ handle.x - 3.5f, handle.y - 3.5f, handle.x + 3.5f, handle.y + 3.5f };
        plan.Add(core::FillRect{ rect, accent });
    }
}

void ScreenshotOverlay::RenderHint(core::FramePlan& plan) {
    if (rasterizer_ == nullptr) {
        return;
    }

    const bool recordMode = options_.mode == OverlayMode::RecordRegion;
    const bool ocrMode = options_.mode == OverlayMode::Ocr;
    const char* hint = selection_.Bounds().IsEmpty()
                           ? (recordMode ? "拖动选择录制区域 · Esc 取消"
                              : ocrMode ? "拖动选择区域 · Enter 识别 · Esc 退出"
                                      : "拖动选择区域 · 空格全屏标注 · Esc 退出")
                           : (ocrMode ? "OCR · Enter 识别 · Esc 退出"
                                      : "标注中 · 1-9 选工具 · Ctrl+Z 撤销 · Ctrl+C 复制 · Ctrl+S 保存");
    const core::FontSpec spec{ options_.fontFamily, 400, 13.0f,
                               Color{ 0.235f, 0.259f, 0.314f, 1.0f } };
    const core::TextCell measured = rasterizer_->LayOut(hint, spec, Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }

    // 1.x 的提示条：顶部居中、白色圆角、左边一个蓝色圆点、深色文字。
    const float textWidth = measured.destination.Width();
    const float textHeight = measured.destination.Height();
    const float chipWidth = textWidth + 34.0f;
    const float chipHeight = textHeight + 16.0f;
    const float chipLeft = (static_cast<float>(screenWidth_) - chipWidth) * 0.5f;
    const float chipTop = 26.0f;

    plan.Add(core::FillRoundedRect{ Rect{ chipLeft, chipTop, chipLeft + chipWidth,
                                         chipTop + chipHeight },
                                    10.0f, Color{ 1.0f, 1.0f, 1.0f, 0.97f } });
    plan.Add(core::StrokeRoundedRect{ Rect{ chipLeft, chipTop, chipLeft + chipWidth,
                                            chipTop + chipHeight },
                                     10.0f, Color{ 0.0f, 0.0f, 0.0f, 0.12f }, 1.0f });
    const float dotX = chipLeft + 14.0f;
    const float dotY = chipTop + chipHeight * 0.5f;
    plan.Add(core::FillEllipse{ Rect{ dotX - 3.0f, dotY - 3.0f, dotX + 3.0f, dotY + 3.0f },
                                Color{ 0.184f, 0.486f, 0.965f, 1.0f } });
    const core::TextCell cell =
        rasterizer_->LayOut(hint, spec, Point{ dotX + 9.0f, chipTop + 8.0f });
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
    plan.Add(core::Blit{ capturedHandle_,
                         Rect{ static_cast<float>(windowX_), static_cast<float>(windowY_),
                               static_cast<float>(windowX_ + screenWidth_),
                               static_cast<float>(windowY_ + screenHeight_) },
                         Rect{ 0.0f, 0.0f, static_cast<float>(screenWidth_),
                               static_cast<float>(screenHeight_) },
                         1.0f });

    if (!selection_.Bounds().IsEmpty()) {
        AnnotationPlanOptions options;
        options.rasterizer = rasterizer_.get();
        options.fontFamily = options_.fontFamily;
        options.scale = 1.0f;
        options.origin = Point{ selection_.Bounds().left - windowX_,
                                selection_.Bounds().top - windowY_ };
        core::AppendAnnotationPlan(document_.Items(), options, plan);
        RenderSelection(plan);
    }

    if (options_.mode != OverlayMode::RecordRegion) {
        RenderToolbar(plan);
        RenderOptionBar(plan);
        RenderMagnifier(plan);
    }
    RenderHint(plan);
    RenderSizeBadge(plan);

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

    const core::ImageRef base{ baseBgra_.data(), screenWidth_, screenHeight_, screenWidth_ * 4 };
    const int block = core::MosaicBlockPixels(annotation.style.mosaicBlockPx, 1.0f);
    const core::MosaicTile tile = core::DownsampleRegion(base, region, block);
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
    DestroyTextEditor();
    if (!utf8.empty()) {
        AddTextAnnotation(textPoint_, utf8);
    }
    dirty_ = true;
}

void ScreenshotOverlay::CancelTextEditor() {
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

    UINT message = 0;
    int toolIndex = 0;
    if (key == VK_ESCAPE) {
        message = kMsgEscape;
    } else if (key == VK_RETURN) {
        message = kMsgCopy;
    } else if (control && key == 'Z') {
        message = kMsgUndo;
    } else if (control && key == 'Y') {
        message = kMsgRedo;
    } else if (control && key == 'C') {
        message = kMsgCopy;
    } else if (control && key == 'S') {
        message = kMsgSave;
    } else if (!control && key == 'V') {
        message = kMsgSelect;
    } else if (!control && key == 'F') {
        message = kMsgFill;
    } else if (key == VK_DELETE) {
        message = kMsgDelete;
    } else if (key == VK_SPACE) {
        message = kMsgFullScreen;
    } else if (key >= '1' && key <= '9') {
        message = kMsgTool;
        toolIndex = static_cast<int>(key - '1');
    }

    if (message == 0) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    PostMessageW(static_cast<HWND>(g_overlay->window_->Handle()), message, toolIndex, 0);
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

    backend_->DetachWindow();
    if (!backend_->Resize(width, height)) {
        backend_->AttachWindow(window_->Handle(), screenWidth_, screenHeight_);
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
    options.origin = Point{ -(selection.left - windowX_), -(selection.top - windowY_) };
    core::AppendAnnotationPlan(document_.Items(), options, plan);

    backend_->Render(plan, IntRect{});

    int readWidth = 0;
    int readHeight = 0;
    const bool ok = backend_->ReadPixels(bgra, readWidth, readHeight) && readWidth == width &&
                    readHeight == height;
    if (!ok) {
        backend_->AttachWindow(window_->Handle(), screenWidth_, screenHeight_);
    }
    return ok;
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
    }
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
