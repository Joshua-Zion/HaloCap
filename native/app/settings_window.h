#pragma once

#include "capability.h"
#include "settings.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <string_view>
#include <windows.h>

namespace halocap::app {

struct SettingsBackgroundCache;

// 1.x 设置窗：820×620 DIP，18 边距外投影、18 圆角；标题栏 50、侧栏 218、
// 内容 #F7F7F9、白卡片 12 圆角；开关 44×24、分段控件、快捷键字段、底部保存/取消。
//
// 整窗用 GDI+ 直接画到分层窗口：设置是低频界面，自绘能逐像素对齐 1.x 的
// 行距、圆角与开关形态；只有目录选择走系统对话框。
class SettingsWindow {
public:
    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    // 第二个参数是失败原因（保存被拒时显示在底部横幅里，窗口不关、编辑保留）。
    using SaveHandler = std::function<bool(const core::AppSettings&, std::wstring&)>;

    bool Create(void* owner, const core::AppSettings& settings, const core::PlatformFacts& facts,
                const std::string& modelsDirectory, SaveHandler onSave);

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

    // 自检/截图用：直接切到某一页。
    void ShowPageForTest(int index);

    // 自检用：热键捕获是低级钩子驱动的，只能注入真按键来验，验完读这三个读数。
    void BeginHotkeyCaptureForTest(int index) { StartHotkeyCapture(index); }
    int CaptureIndexForTest() const { return captureIndex_; }
    const std::wstring& ErrorForTest() const { return error_; }
    const core::AppSettings& SettingsForTest() const { return settings_; }
    // 保存闸门的确定性入口：不依赖按钮命中区与 DPI 缩放，直接走 Save()。
    void SaveForTest() { Save(); }
    int HitTestForTest(int x, int y) const { return HitTest(POINT{ x, y }); }
    int ScaleForTest() const { return static_cast<int>(scale_ * 100.0f + 0.5f); }
    bool HookInstalledForTest() const { return keyHook_ != nullptr; }
    int HookEventsForTest() const { return hookEvents_; }

private:
    enum class Page : int {
        General = 0,
        Hotkeys = 1,
        Capture = 2,
        Ocr = 3,
        Recording = 4,
        About = 5,
    };

    struct Hotspot {
        RECT rect{};
        int id = 0;
    };

    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HotkeyHook(int code, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void Render();
    void AddHotspot(const RECT& rect, int id);
    int HitTest(POINT client) const;
    void Click(int id);
    void StartHotkeyCapture(int index);
    void FinishHotkeyCapture(const core::HotkeySpec& spec);
    void CancelHotkeyCapture();
    void TrackModifier(DWORD virtualKey, bool down);
    void ShowError(const std::wstring& message);
    void ClearError();
    void Save();
    void CopySummary();
    void CheckUpdate();
    void BeginUpdateDownload(const std::wstring& version);
    void BeginOcrModelDownload();
    void OpenFolder(const std::wstring& path);
    void ChooseDirectory();
    std::wstring SummaryText() const;

    HWND window_ = nullptr;
    // 任务栏 / Alt+Tab 图标：与设置页标题栏、托盘同一个 logo，Close 时释放。
    void* iconBig_ = nullptr;
    void* iconSmall_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HDC memory_ = nullptr;
    void* bits_ = nullptr;
    int widthPx_ = 0;
    int heightPx_ = 0;
    float scale_ = 1.0f;

    core::AppSettings settings_;
    core::PlatformFacts facts_;
    std::string modelsDirectory_;
    SaveHandler onSave_;
    bool saved_ = false;

    Page page_ = Page::General;
    int scroll_ = 0;
    int contentHeight_ = 0;
    int viewHeightDip_ = 0;
    int hoverId_ = 0;
    int captureIndex_ = -1;
    std::wstring error_;
    // 页脚的状态行（检查更新 / 下载进度），与 error_（红底错误横幅）互斥。
    std::wstring status_;
    bool updateBusy_ = false;
    HHOOK keyHook_ = nullptr;
    int hookEvents_ = 0;
    bool ctrlDown_ = false;
    bool altDown_ = false;
    bool shiftDown_ = false;
    bool winDown_ = false;
    bool comInitialized_ = false;
    std::vector<Hotspot> hotspots_;
    std::unique_ptr<SettingsBackgroundCache> backgroundCache_;
    void BuildBackgroundCache();
    bool dragging_ = false;
    POINT dragOrigin_{};
    RECT dragWindow_{};
    // 目录选择线程还开着时忽略重复点击；结果消息到了再置回 false。
    bool pickerOpen_ = false;
};

}
