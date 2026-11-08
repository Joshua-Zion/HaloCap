#pragma once

#include "capability.h"
#include "settings.h"

#include <functional>
#include <string>
#include <string>
#include <vector>
#include <windows.h>

namespace halocap::app {

// 六页设置：截图 / 录制 / OCR / 快捷键 / 外观 / 关于（含环境摘要）。
// 用 GDI 控件而不是 D2D：设置是低频界面，原生 COMBOBOX/EDIT 的选择、输入法、
// 键盘导航都是白拿的；覆盖层那套高频渲染不该被搬到这里。
class SettingsWindow {
public:
    SettingsWindow();
    ~SettingsWindow();

    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;

    using SaveHandler = std::function<bool(const core::AppSettings&)>;

    bool Create(void* owner, const core::AppSettings& settings, const core::PlatformFacts& facts,
                const std::string& modelsDirectory, SaveHandler onSave);

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

private:
    enum class Page : int {
        Screenshot = 0,
        Recording = 1,
        Ocr = 2,
        Hotkeys = 3,
        Appearance = 4,
        About = 5,
    };

    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HotkeyHook(int code, WPARAM wParam, LPARAM lParam);
    void StartHotkeyCapture(int index);
    void FinishHotkeyCapture(const core::HotkeySpec& spec);
    void RefreshHotkeyLabels();
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    HWND AddCombo(Page page, int x, int y, int width, const std::vector<std::wstring>& items,
                  int selected);
    HWND AddCheck(Page page, int x, int y, int width, const std::wstring& text, bool checked);
    HWND AddLabel(Page page, int x, int y, int width, const std::wstring& text);
    HWND AddEdit(Page page, int x, int y, int width, const std::wstring& text);
    void ShowPage(Page page);
    void Save();

    HWND window_ = nullptr;
    HWND list_ = nullptr;
    HWND saveButton_ = nullptr;
    HWND cancelButton_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    HBRUSH sidebarBrush_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH accentBrush_ = nullptr;
    HPEN cardBorderPen_ = nullptr;
    HPEN fieldBorderPen_ = nullptr;

    struct PageControl {
        HWND handle = nullptr;
        Page page = Page::Screenshot;
    };
    std::vector<PageControl> controls_;

    Page current_ = Page::Screenshot;
    core::AppSettings settings_;
    core::PlatformFacts facts_;
    std::string modelsDirectory_;
    SaveHandler onSave_;
    bool saved_ = false;

    // 控件句柄（保存时读回）。
    HWND screenshotFormat_ = nullptr;
    HWND recordingResolution_ = nullptr;
    HWND recordingFrameRate_ = nullptr;
    HWND recordingQuality_ = nullptr;
    HWND recordingRange_ = nullptr;
    HWND recordingCountdown_ = nullptr;
    HWND recordingBitrate_ = nullptr;
    HWND systemAudio_ = nullptr;
    HWND microphone_ = nullptr;
    HWND recordingDirectory_ = nullptr;
    HWND ocrEngine_ = nullptr;
    HWND ocrAutoCopy_ = nullptr;
    HWND ocrPin_ = nullptr;
    HWND menuSize_ = nullptr;
    HWND pinGlow_ = nullptr;
    HWND autoStart_ = nullptr;
    HWND hotkeyValues_[5] = {};
    HWND hotkeyButtons_[5] = {};
    int captureIndex_ = -1;
    std::wstring summaryText_;
    HWND copySummaryButton_ = nullptr;
    HWND checkUpdateButton_ = nullptr;
};

}
