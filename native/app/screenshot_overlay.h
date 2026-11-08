#pragma once

#include "annotation.h"
#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "selection.h"
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

    const char* LastErrorKind() const { return errorKind_; }
    core::IntRect SelectionRectPx() const;

private:
    enum class Phase {
        Selecting,
        Annotating,
    };

    enum class Action : std::int32_t {
        None = 0,
        ToolSelect, ToolPen, ToolRectangle, ToolEllipse, ToolArrow, ToolText, ToolHighlighter,
        ToolMosaic, ToolStep, ToolEraser,
        Color,
        Undo, Redo, Copy, Save, Pin, CloseOverlay,
        FillOn, FillOff, ThicknessDown, ThicknessUp, TextSizeDown, TextSizeUp, Bold, Italic,
        MosaicDown, MosaicUp, ArrowEnd, ArrowBoth, ArrowNone,
    };

    enum class ToolbarIcon : std::int32_t {
        None = 0,
        Pen, Rectangle, Ellipse, Arrow, Text, Highlighter, Mosaic, Step, Eraser,
        Undo, Redo, Copy, Save, Pin, Close, Select,
        Separator,
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
    void RenderSelection(core::FramePlan& plan);
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

    // 工具栏
    std::vector<ToolbarButton> toolbar_;
    RECT toolbarBounds_{};

    struct OptionButton {
        RECT rect{};
        Action action = Action::None;
        bool active = false;
        std::string text;
    };
    std::vector<OptionButton> optionButtons_;
    RECT optionBounds_{};

    bool selectMode_ = false;
    int selectedAnnotation_ = -1;
    bool movingAnnotation_ = false;
    POINT lastMouse_{};
    bool mouseInside_ = false;
    int hoveredToolbar_ = -1;
    float hoverAlpha_[32] = {};
    bool hoverAnimating_ = false;
    void StepHoverAnimation();

    // 文字输入弹窗
    HWND editorWindow_ = nullptr;
    HWND editControl_ = nullptr;
    void* editorBrush_ = nullptr;
    core::Point textPoint_{};
};

}
