#pragma once

#include "annotation.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace Gdiplus {
class Graphics;
}

namespace halocap::app {

struct PinWindowOptions {
    // 外描边（1.x 的 PinGlowEnabled）：内容外圈的 1.5 DIP 蓝色描边。
    bool outline = true;
    // 设置里的「贴图 OCR」：关掉时菜单里不出现「识别文字」。
    bool ocrEnabled = true;
    bool jpeg = false;
    std::string fontFamily = "Microsoft YaHei UI";
};

// 贴图窗口：无边框、置顶、分层窗口（UpdateLayeredWindow），内容是截图 + 描边/光晕。
// 交互对齐 1.x：右键菜单（标注/识别文字/复制/另存/实际大小/外描边/关闭/全部关闭）、
// 空格进入标注（九种工具 + 撤销/重做/清空）、滚轮缩放（Ctrl+滚轮调不透明度）、
// Ctrl+0 恢复实际大小、双击关闭、Esc 退出标注或关闭。
class PinWindow {
public:
    PinWindow();
    ~PinWindow();

    PinWindow(const PinWindow&) = delete;
    PinWindow& operator=(const PinWindow&) = delete;

    // bgra: width*height*4，函数返回后调用方可以释放。screenX/Y 是图像左上角。
    bool Create(const std::uint8_t* bgra, int width, int height, int screenX, int screenY,
                const PinWindowOptions& options = PinWindowOptions{});

    // 贴图上的 OCR：把原始图像与它的屏幕位置交给调用方（1.x 同语义，标注不参与识别）。
    using OcrHandler = std::function<void(const std::uint8_t*, int, int, int, int)>;
    void SetOcrHandler(OcrHandler handler) { onOcr_ = std::move(handler); }

    // 「全部关闭」由 app 统一收口：它持有所有贴图，回调里逐个 Close，消息循环里再收尸。
    void SetCloseAllHandler(std::function<void()> handler) { onCloseAll_ = std::move(handler); }
    // 「外描边」切换后 app 要写设置并同步到其它贴图。
    void SetOutlineChangedHandler(std::function<void(bool)> handler) {
        onOutlineChanged_ = std::move(handler);
    }

    void SetOutline(bool enabled);
    void SetOcrEnabled(bool enabled);

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return static_cast<HWND>(window_); }

    // 自检用。
    bool AnnotatingForTest() const { return annotating_; }
    bool MenuOpenForTest() const { return menuWindow_ != nullptr; }
    const core::AnnotationDocument& DocumentForTest() const { return document_; }
    core::IntRect ContentRectForTest() const;
    core::IntRect ToolbarBoundsForTest() const {
        return core::IntRect{ toolbarBounds_.left, toolbarBounds_.top, toolbarBounds_.right,
                              toolbarBounds_.bottom };
    }
    core::IntRect OptionBoundsForTest() const {
        return core::IntRect{ optionBounds_.left, optionBounds_.top, optionBounds_.right,
                              optionBounds_.bottom };
    }
    // 二级栏最后一个按钮的右边缘：和面板右边缘比一下就知道有没有留空。
    int OptionContentRightForTest() const;
    int MenuRectLeftForTest() const { return menuScreenX_; }
    int MenuRectTopForTest() const { return menuScreenY_; }

private:
    enum class PinAction : int {
        None = 0,
        ToolShape, ToolArrow, ToolPen, ToolHighlighter, ToolEraser, ToolText, ToolMosaic,
        ToolStep,
        Undo, Redo, Clear, CloseEdit, Done,
        Color,
        ShapeRectangle, ShapeEllipse, FillOn, FillOff,
        ThicknessDown, ThicknessUp, TextSizeDown, TextSizeUp,
        Bold, Italic,
        MosaicDown, MosaicUp,
        ArrowEnd, ArrowBoth, ArrowNone,
        EraserDown, EraserUp,
    };

    struct PinButton {
        RECT rect{};
        PinAction action = PinAction::None;
        int payload = 0;
        const char* icon = nullptr;
        bool enabled = true;
        bool active = false;
        bool separator = false;
        bool dot = false;
        core::Color dotColor{};
        std::wstring text;
    };

    struct MenuItem {
        int id = 0;
        std::wstring label;
        std::wstring shortcut;
        bool separator = false;
        bool checkable = false;
        bool checked = false;
        bool enabled = true;
        RECT rect{};
    };

    static bool EnsureClass();
    static bool EnsureMenuClass();
    static bool EnsureEditorClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK MenuProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditorProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditorEditProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMenu(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    float Scale(float dip) const { return dip * uiScale_; }
    int ScaleInt(float dip) const { return static_cast<int>(dip * uiScale_ + 0.5f); }

    void UpdateLayout();
    void Render();
    void RenderMenu();
    void RenderLayered(const std::vector<std::uint8_t>& pixels, int width, int height);

    void LayoutToolbar(int originX, int originY);
    void LayoutOptionBar(int originX, int originY);
    void DrawChrome(Gdiplus::Graphics& graphics) const;
    void DrawHud(Gdiplus::Graphics& graphics) const;

    bool IsOverChrome(POINT client) const;
    int ToolbarButtonAt(POINT client) const;
    int OptionButtonAt(POINT client) const;

    void SetAnnotationMode(bool enabled);
    void SetTool(core::AnnotationTool tool);
    void HandleToolbar(const PinButton& button);
    void HandleOption(PinAction action);
    void AdjustBrush(int direction);
    void UpdateHover(int index, bool optionBar);

    void OnMouseMove(HWND window, POINT client);
    void OnLeftButtonDown(HWND window, POINT client);
    void OnLeftButtonUp(HWND window);
    void OnMouseWheel(int delta);
    void OnKeyDown(WPARAM key);
    void OnSetCursor();

    void StartWindowDrag(HWND window);
    void ContinueWindowDrag(HWND window);
    void EndWindowDrag(HWND window);

    void PaintMosaicAt(core::Point local);
    void FinishStroke();
    void CommitLiveStroke();
    void UpdateMosaicTile(core::Annotation& annotation);

    void BeginTextEditor(core::Point local);
    void CommitTextEditor();
    void CancelTextEditor();
    void DestroyTextEditor();

    void ShowHud(const std::wstring& text);
    void HideHud();

    void ShowMenu(POINT screen);
    void HideMenu();
    void ExecuteMenu(int id);
    void BuildMenuItems();

    void CopyImage();
    void SaveImage();
    void RequestOcr();
    void ResetSizeAndOpacity();
    std::vector<std::uint8_t> ComposeImage(int* width, int* height);
    void RenderAnnotations(Gdiplus::Graphics& graphics, float scale, float offsetX,
                           float offsetY, const std::vector<core::Annotation>& items) const;

    static PinWindow* g_menuOwner;
    static PinWindow* g_editorOwner;
    static WNDPROC g_oldEditProc;
    static HFONT g_editorFont;
    static HBRUSH g_editorBrush;

    std::wstring fontFamilyW_;

    void* window_ = nullptr;
    void* menuWindow_ = nullptr;
    void* editorWindow_ = nullptr;
    void* editControl_ = nullptr;
    core::Point textPoint_{};
    bool editorCommitPosted_ = false;

    int contentWidth_ = 0;
    int contentHeight_ = 0;
    int contentScreenX_ = 0;
    int contentScreenY_ = 0;
    std::vector<std::uint8_t> content_;

    PinWindowOptions options_;
    float uiScale_ = 1.0f;
    float zoom_ = 1.0f;
    float opacity_ = 1.0f;

    int glow_ = 12;
    int border_ = 1;
    int contentLeft_ = 0;
    int contentTop_ = 0;
    int displayWidth_ = 0;
    int displayHeight_ = 0;
    int windowWidth_ = 0;
    int windowHeight_ = 0;
    bool chromeAbove_ = false;
    int chromeHeight_ = 0;

    bool trackingLeave_ = false;

    bool annotating_ = false;
    bool drawing_ = false;
    bool mosaicPainting_ = false;
    core::Point lastMosaicPoint_{};
    core::AnnotationDocument document_;
    core::AnnotationStyle style_;
    core::AnnotationTool tool_ = core::AnnotationTool::Pen;
    core::AnnotationTool shapeTool_ = core::AnnotationTool::Rectangle;
    float eraserSize_ = core::annotation_defaults::kEraserSize;

    std::vector<PinButton> toolbar_;
    std::vector<PinButton> optionButtons_;
    RECT toolbarBounds_{};
    RECT optionBounds_{};
    int toolbarWidth_ = 0;
    int toolbarHeight_ = 0;
    int optionWidth_ = 0;
    int optionHeight_ = 0;
    int hoveredToolbar_ = -1;
    int hoveredOption_ = -1;

    bool menuOpen_ = false;
    std::vector<MenuItem> menuItems_;
    int hoveredMenuIndex_ = -1;
    int menuWidth_ = 0;
    int menuHeight_ = 0;
    int menuScreenX_ = 0;
    int menuScreenY_ = 0;

    // SetFocus(EDIT) 会让贴图自己收到一次 WM_KILLFOCUS，别把它当成「切走了」。
    bool startingEditor_ = false;

    std::wstring hud_;
    bool hudVisible_ = false;

    bool dragging_ = false;
    POINT dragOrigin_{};
    POINT dragWindow_{};

    OcrHandler onOcr_;
    std::function<void()> onCloseAll_;
    std::function<void(bool)> onOutlineChanged_;
};

}
