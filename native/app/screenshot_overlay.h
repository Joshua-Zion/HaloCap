#pragma once

#include "annotation.h"
#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "input_host.h"
#include "selection.h"
#include "win32_input_host.h"
#include "win32_surface_window.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace halocap::app {

enum class OverlayMode {
    Screenshot,
    Ocr,
    RecordRegion,
};

struct ScreenshotOverlayOptions {
    OverlayMode mode = OverlayMode::Screenshot;
    // 空 = 图片\HaloCap。
    std::wstring saveDirectory;
    bool jpeg = false;
    // 截图页开着的全局「贴图」热键：覆盖层吞键，注册表那条收不到，所以在这里自己匹配。
    core::HotkeySpec pinHotkey{};
    std::string fontFamily = "Microsoft YaHei UI";

    // 贴图：把合成好的选区交给调用方（由 app 持有 PinWindow 的生命周期）。
    std::function<void(const std::uint8_t* bgra, int width, int height, int screenX, int screenY)>
        onPin;

    // OCR 模式：Enter /「识」把选区合成成 BGRA 交出去，由 app 跑引擎并开结果窗口。
    std::function<void(const std::uint8_t* bgra, int width, int height, int screenX, int screenY)>
        onOcr;

    // 区域录制：拖完选区交回屏幕坐标，由 app 起录制。
    std::function<void(core::IntRect rect)> onRegion;
};

// S3 的交互层：全屏覆盖层 + 选区 + 工具栏 + 标注编辑 + 文字输入 + 导出/贴图。
//
// 一次会话一个实例：Open 抓屏建窗，Close 全部释放。鼠标与快捷键都在覆盖层窗口的消息
// 处理器里；键盘用低级钩子，因为覆盖层窗口按设计不抢焦点（WS_EX_NOACTIVATE）。
// 文字输入用一个小 EDIT 弹窗（真的输入法，不自己处理 IME 组合串）。
class ScreenshotOverlay {
public:
    ScreenshotOverlay();
    ~ScreenshotOverlay();

    ScreenshotOverlay(const ScreenshotOverlay&) = delete;
    ScreenshotOverlay& operator=(const ScreenshotOverlay&) = delete;

    bool Open(const ScreenshotOverlayOptions& options);
    void Close();
    bool Active() const { return active_; }

    // app 的消息循环每轮调用：需要时重绘并上屏。
    void Pump();

    core::AnnotationDocument& Document() { return document_; }
    core::SelectionModel& Selection() { return selection_; }

    // 直接摆一个选区（自检 / 自动化用）。
    void DebugSelect(core::IntRect rect);

    // 自检用：把覆盖层窗口交给调用方，用 SendMessage 走一遍真实的鼠标消息路径。
    void* WindowHandle() const;

    // 选区合成后的三种出口。成功之后不会自动关闭，调用方决定。
    bool CopySelection();
    bool SaveSelection(std::string* pathOut);
    bool PinSelection();

    // 文字：覆盖层自己用 EDIT 弹窗输入，这条给自检与将来的工具栏按钮用。
    void AddTextAnnotation(core::Point localPoint, std::string_view text);

    // 自检用：把当前桌面（含覆盖层 UI）抓成 PNG，用来人工核对界面。
    bool GrabDesktopToPng(const std::wstring& path);

    // 自检用：工具栏/选项栏的几何与状态（对齐 1.x 的那几条判据都读这里）。
    bool ToolbarVisibleForTest() const { return toolbarVisible_; }
    int ToolbarButtonCountForTest() const { return static_cast<int>(toolbar_.size()); }
    core::IntRect ToolbarBoundsForTest() const {
        return core::IntRect{ toolbarBounds_.left, toolbarBounds_.top, toolbarBounds_.right,
                              toolbarBounds_.bottom };
    }
    int OptionButtonCountForTest() const { return static_cast<int>(optionButtons_.size()); }
    // 每个按钮都必须落在背景里：跑到外面既看着缺一块，也因为命中区是背景矩形而点不到。
    bool OptionBarFitsForTest() const {
        for (const OptionButton& button : optionButtons_) {
            if (button.rect.left < optionBounds_.left || button.rect.right > optionBounds_.right ||
                button.rect.top < optionBounds_.top || button.rect.bottom > optionBounds_.bottom) {
                return false;
            }
        }
        return true;
    }
    float ThicknessForTest() const { return style_.thickness; }
    core::IntRect ToolbarButtonRectForTest(int index) const {
        if (index < 0 || index >= static_cast<int>(toolbar_.size())) {
            return core::IntRect{};
        }
        const RECT& rect = toolbar_[static_cast<std::size_t>(index)].rect;
        return core::IntRect{ rect.left, rect.top, rect.right, rect.bottom };
    }
    const std::wstring& ToastForTest() const { return toast_; }
    bool AnnotatingForTest() const { return annotating_; }
    int UiScalePercentForTest() const { return static_cast<int>(uiScale_ * 100.0f + 0.5f); }

    const char* LastErrorKind() const { return errorKind_; }
    core::IntRect SelectionRectPx() const;

    // 工具栏图标的种类。图标路径由 App.xaml 生成（见 toolbar_icons.gen.h），
    // 命名的辅助函数在匿名命名空间里，所以这个枚举得是公开的。
    enum class ToolbarIcon : std::int32_t {
        None = 0,
        Pen, Rectangle, Ellipse, Arrow, Text, Highlighter, Mosaic, Step, Eraser,
        Undo, Redo, Clear, Copy, Save, Pin, Ocr, Close, Done, Select, Shape, Outline, Fill,
        ArrowBoth, ArrowLine, Bold, Italic, Decrease, Increase,
        Separator,
    };

private:
    // Selecting = 还在拖第一块选区；Annotating = 选区已完成（这一相位里可以移动/缩放，
    // 只有 annotating_ 为真时鼠标才是画标注）。
    enum class Phase {
        Selecting,
        Annotating,
    };

    enum class Action : std::int32_t {
        None = 0,
        ToolSelect, ToolShape, ToolPen, ToolRectangle, ToolEllipse, ToolArrow, ToolText,
        ToolHighlighter, ToolMosaic, ToolStep, ToolEraser,
        Color,
        Undo, Redo, ClearAnnotations, Save, Pin, Ocr, Copy, CancelOverlay, DoneAndCopy,
        CloseOverlay,
        FillOn, FillOff, ShapeRectangle, ShapeEllipse,
        ThicknessDown, ThicknessUp, TextSizeDown, TextSizeUp, Bold, Italic,
        MosaicDown, MosaicUp, ArrowEnd, ArrowBoth, ArrowNone, EraserDown, EraserUp,
    };

    struct ToolbarButton {
        RECT rect{};
        Action action = Action::None;
        int payload = 0;      // 颜色下标
        int toolIndex = -1;
        std::string label;
        ToolbarIcon icon = ToolbarIcon::None;
        bool enabled = true;
        bool active = false;
    };

    static bool EnsureEditorClass();
    static LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditorEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK KeyHook(int code, WPARAM wParam, LPARAM lParam);

    LRESULT HandleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    void OnMouseMove(POINT point);
    void OnLeftButtonDown(POINT point);
    void OnLeftButtonUp(POINT point);
    void OnRightButtonDown(POINT point);
    void UpdateCursor(POINT point);
    bool IsOverChrome(POINT point) const;
    void StartNewSelection(POINT point);
    void SelectWindowOrMonitor(POINT point);
    void UpdateHoverCandidate(POINT point);
    void ClearHoverCandidate();
    core::IntRect MonitorBoundsAt(POINT point) const;
    void NudgeSelection(int dx, int dy);
    void SetTool(core::AnnotationTool tool);
    void HandleToolbar(const ToolbarButton& button);
    void HandleOption(Action action);
    void LayoutToolbar();
    void LayoutOptionBar();
    void RenderOptionBar(core::FramePlan& plan);
    void RenderMagnifier(core::FramePlan& plan);
    void RenderSizeBadge(core::FramePlan& plan);
    void Render();
    void RenderToolbar(core::FramePlan& plan);
    void RenderToolbarIcon(core::FramePlan& plan, const core::Rect& rect, ToolbarIcon icon,
                           bool active, bool enabled);
    // 画 1.x 的 160×160 填充路径（数据来自 App.xaml 的 Toolbar*Geometry）。
    void AppendIconPath(core::FramePlan& plan, const core::Rect& rect, ToolbarIcon icon,
                        const core::Color& color);
    void RenderSelection(core::FramePlan& plan);
    void AddDimAround(core::FramePlan& plan, const core::Rect& hole, const core::Color& dim,
                      float width, float height);
    core::Color SelectionAccent() const;
    void RenderCrosshair(core::FramePlan& plan);
    void RenderHint(core::FramePlan& plan);
    void UpdateMosaicTile(core::Annotation& annotation);
    void RunOcr();
    void OpenTextEditor(core::Point localPoint);
    void CommitTextEditor();
    void CancelTextEditor();
    void DestroyTextEditor();

    bool ComposeSelection(std::vector<std::uint8_t>& bgra, int& width, int& height);
    std::wstring ResolveSavePath() const;
    void InstallKeyHook();
    void UninstallKeyHook();

    // 平台对象：一次会话一套，Close 时全部释放。
    std::unique_ptr<halocap::platform::win32::D2dRenderBackend> backend_;
    std::unique_ptr<halocap::platform::win32::D2dFontRasterizer> rasterizer_;
    std::unique_ptr<halocap::platform::win32::Win32SurfaceWindow> window_;

    ScreenshotOverlayOptions options_;
    core::AnnotationDocument document_;
    core::SelectionModel selection_;
    core::AnnotationStyle style_;
    float eraserSize_ = core::annotation_defaults::kEraserSize;
    core::AnnotationTool tool_ = core::AnnotationTool::Pen;

    std::vector<std::uint8_t> baseBgra_;   // 整屏底图，马赛克取样用
    core::ImageHandle capturedHandle_ = 0;
    int screenWidth_ = 0;
    int screenHeight_ = 0;
    int windowX_ = 0;
    int windowY_ = 0;

    Phase phase_ = Phase::Selecting;
    bool active_ = false;
    bool dirty_ = true;
    bool pendingClose_ = false;
    const char* errorKind_ = "none";

    // 鼠标状态
    bool selecting_ = false;
    bool resizing_ = false;
    core::SelectionHandle resizeHandle_ = core::SelectionHandle::None;
    bool movingSelection_ = false;
    bool drawing_ = false;
    POINT lastPoint_{};
    POINT moveOrigin_{};

    // 整块覆盖层是按物理像素画的，而 1.x 的尺寸全是 DIP；在 150% 的屏上必须乘 1.5，
    // 否则工具栏、文字、放大镜都会比 1.x 小三分之一（「整体偏小」「提示字太小」就是这条）。
    float uiScale_ = 1.0f;
    int Scale(int dip) const { return static_cast<int>(dip * uiScale_ + 0.5f); }
    float Scale(float dip) const { return dip * uiScale_; }

    // 工具栏
    std::vector<ToolbarButton> toolbar_;
    RECT toolbarBounds_{};

    struct OptionButton {
        RECT rect{};
        Action action = Action::None;
        bool active = false;
        std::string text;
        int colorIndex = -1;                     // >= 0 时画色板圆点
        ToolbarIcon icon = ToolbarIcon::None;    // 有图标就画图标
    };
    std::vector<OptionButton> optionButtons_;
    RECT optionBounds_{};

    // annotating_ = 工具激活（鼠标在选区内是画标注）。1.x 里刚拖完选区不是标注态：
    // 那时拖选区内是「移动整块」，只有按了空格/工具键/工具按钮才进标注。
    bool annotating_ = false;
    // 「形状」按钮记住上一次用的矩形/椭圆（1.x 的 ToolShape 就是这两个的代理）。
    core::AnnotationTool shapeTool_ = core::AnnotationTool::Rectangle;
    int selectedAnnotation_ = -1;
    bool movingAnnotation_ = false;
    POINT lastMouse_{};
    bool mouseInside_ = false;
    int hoveredToolbar_ = -1;
    std::uint64_t hoverSinceMs_ = 0;
    float hoverAlpha_[32] = {};
    bool hoverAnimating_ = false;
    void StepHoverAnimation();
    void RenderTooltip(core::FramePlan& plan);

    // 颜色复制（1.x 的 C）：一条 1.2 秒的提示，Shift 切换 hex / rgb()。
    std::wstring toast_;
    std::uint64_t toastUntilMs_ = 0;
    bool colorAsRgb_ = false;
    void CopyColorUnderCursor(bool asRgb);
    void RenderToast(core::FramePlan& plan);

    // 工具栏入场：1.x 是 opacity 0→1 + 从下往上 6 px，这里只做位移那一段。
    float toolbarAnim_ = 1.0f;
    void StepToolbarAnimation();

    // 未成选区时，光标下窗口的高亮候选（1.x 的 ElementBorder）。
    HWND hoverWindow_ = nullptr;
    core::IntRect hoverRect_{};
    POINT lastProbePoint_{-100000, -100000};
    std::uint64_t lastProbeMs_ = 0;
    bool hoverProbed_ = false;

    // 双击判定自己做：窗口类没开 CS_DBLCLKS，也不值得为此改窗口创建（1.x 双击复制）。
    POINT lastClickPoint_{-100000, -100000};
    std::uint64_t lastClickMs_ = 0;

    // 按下那一刻光标下的窗口/所在显示器：短于一拖的「单击」用它兜底（1.x 的 _clickElementPx）。
    core::IntRect pressWindowRect_{};
    core::IntRect pressMonitorRect_{};
    bool toolbarVisible_ = false;

    // 文字输入弹窗
    HWND editorWindow_ = nullptr;
    HWND editControl_ = nullptr;
    void* editorBrush_ = nullptr;
    core::Point textPoint_{};
    // >= 0 表示这次编辑是「改已有文字」，提交时替换而不是新增。
    int editingTextIndex_ = -1;
};

}
