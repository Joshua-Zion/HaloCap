#include "settings_window.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapSettings";
constexpr int kListWidth = 190;

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
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

std::wstring GetText(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

}  // namespace

SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() { Close(); }

bool SettingsWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &SettingsWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        description.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

HWND SettingsWindow::AddCombo(Page page, int x, int y, int width,
                              const std::vector<std::wstring>& items, int selected) {
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x, y,
                                 width, 220, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    for (const std::wstring& item : items) {
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
    }
    SendMessageW(combo, CB_SETCURSEL, std::max(0, selected), 0);
    SendMessageW(combo, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    controls_.push_back({ combo, page });
    ShowWindow(combo, SW_HIDE);
    return combo;
}

HWND SettingsWindow::AddCheck(Page page, int x, int y, int width, const std::wstring& text,
                              bool checked) {
    HWND check = CreateWindowExW(0, L"BUTTON", text.c_str(),
                                 WS_CHILD | BS_AUTOCHECKBOX | WS_TABSTOP, x, y, width, 24,
                                 window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(check, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    controls_.push_back({ check, page });
    ShowWindow(check, SW_HIDE);
    return check;
}

HWND SettingsWindow::AddLabel(Page page, int x, int y, int width, const std::wstring& text) {
    HWND label = CreateWindowExW(0, L"STATIC", text.c_str(), WS_CHILD | SS_LEFT, x, y, width, 22,
                                 window_, nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    controls_.push_back({ label, page });
    ShowWindow(label, SW_HIDE);
    return label;
}

HWND SettingsWindow::AddEdit(Page page, int x, int y, int width, const std::wstring& text) {
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text.c_str(),
                                WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, x, y, width, 24, window_,
                                nullptr, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    controls_.push_back({ edit, page });
    ShowWindow(edit, SW_HIDE);
    return edit;
}

bool SettingsWindow::Create(void* owner, const core::AppSettings& settings,
                            const core::PlatformFacts& facts, const std::string& modelsDirectory,
                            SaveHandler onSave) {
    if (!EnsureClass()) {
        return false;
    }

    settings_ = settings;
    facts_ = facts;
    modelsDirectory_ = modelsDirectory;
    onSave_ = std::move(onSave);
    saved_ = false;

    window_ = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"HaloCap 设置",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 840, 600,
                              static_cast<HWND>(owner), nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Microsoft YaHei UI");

    list_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 12, 12, kListWidth,
                            500, window_, reinterpret_cast<HMENU>(100), GetModuleHandleW(nullptr),
                            nullptr);
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    for (const wchar_t* item : { L"截图", L"录制", L"OCR", L"快捷键", L"外观", L"关于 / 诊断" }) {
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(list_, LB_SETCURSEL, 0, 0);

    saveButton_ = CreateWindowExW(0, L"BUTTON", L"保存", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                  620, 520, 90, 30, window_, reinterpret_cast<HMENU>(200),
                                  GetModuleHandleW(nullptr), nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE, 720, 520, 90, 30,
                                    window_, reinterpret_cast<HMENU>(201),
                                    GetModuleHandleW(nullptr), nullptr);
    for (HWND button : { saveButton_, cancelButton_ }) {
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    const int x = 220;
    const int controlWidth = 380;
    const int labelWidth = 140;

    // ---- 截图 ----
    AddLabel(Page::Screenshot, x, 20, labelWidth, L"截图格式");
    screenshotFormat_ = AddCombo(Page::Screenshot, x, 44, controlWidth, { L"PNG", L"JPEG" },
                                 static_cast<int>(settings_.screenshotFormat));
    AddLabel(Page::Screenshot, x, 88, 560,
             L"截图热键：" + Utf8ToWide(core::FormatHotkeyText(settings_.screenshotHotkey)) +
                 L"（热键编辑在下一轮）");

    // ---- 录制 ----
    AddLabel(Page::Recording, x, 20, labelWidth, L"分辨率");
    recordingResolution_ = AddCombo(Page::Recording, x, 44, controlWidth,
                                    { L"原始", L"2160p", L"1440p", L"1080p", L"720p" },
                                    static_cast<int>(settings_.recording.resolution));
    AddLabel(Page::Recording, x, 88, labelWidth, L"帧率");
    recordingFrameRate_ = AddCombo(Page::Recording, x, 112, controlWidth, { L"30", L"60" },
                                   settings_.recording.frameRate == core::RecordingFrameRate::Sixty
                                       ? 1
                                       : 0);
    AddLabel(Page::Recording, x, 156, labelWidth, L"质量");
    recordingQuality_ = AddCombo(Page::Recording, x, 180, controlWidth, { L"高", L"均衡", L"紧凑" },
                                 static_cast<int>(settings_.recording.quality));
    AddLabel(Page::Recording, x, 224, labelWidth, L"范围");
    recordingRange_ = AddCombo(Page::Recording, x, 248, controlWidth, { L"区域", L"窗口" },
                               static_cast<int>(settings_.recording.range));
    AddLabel(Page::Recording, x, 292, labelWidth, L"倒计时");
    recordingCountdown_ = AddCombo(Page::Recording, x, 316, 200, { L"关", L"3 秒", L"5 秒" },
                                   settings_.recording.countdown ==
                                           core::RecordingCountdown::ThreeSeconds
                                       ? 1
                                       : (settings_.recording.countdown ==
                                                  core::RecordingCountdown::FiveSeconds
                                              ? 2
                                              : 0));
    AddLabel(Page::Recording, x, 360, labelWidth, L"音频码率");
    recordingBitrate_ = AddCombo(Page::Recording, x, 384, 200, { L"128 kbps", L"192 kbps" },
                                 settings_.recording.audioBitrate ==
                                         core::RecordingAudioBitrate::K192
                                     ? 1
                                     : 0);
    systemAudio_ = AddCheck(Page::Recording, x, 420, 200, L"录制系统声音", settings_.recording.systemAudio);
    microphone_ = AddCheck(Page::Recording, x + 220, 420, 200, L"录制麦克风",
                           settings_.recording.microphone);
    AddLabel(Page::Recording, x, 452, labelWidth, L"保存目录");
    recordingDirectory_ = AddEdit(Page::Recording, x, 476, controlWidth,
                                  Utf8ToWide(settings_.recordingDirectory));

    // ---- OCR ----
    AddLabel(Page::Ocr, x, 20, labelWidth, L"识别引擎");
    ocrEngine_ = AddCombo(Page::Ocr, x, 44, controlWidth,
                          { L"系统 OCR（Windows.Media.Ocr）", L"PP-OCRv6 tiny（模型）" },
                          static_cast<int>(settings_.ocr.engine));
    ocrAutoCopy_ = AddCheck(Page::Ocr, x, 92, 300, L"识别后自动复制文本", settings_.ocr.autoCopy);
    ocrPin_ = AddCheck(Page::Ocr, x, 122, 300, L"贴图时同时识别", settings_.ocr.pinOcrEnabled);
    AddLabel(Page::Ocr, x, 164, 560,
             L"模型目录：" + (modelsDirectory_.empty() ? L"未找到（设置 HALOCAP_OCR_MODELS）"
                                                       : Utf8ToWide(modelsDirectory_)));

    // ---- 快捷键 ----
    const struct {
        const wchar_t* name;
        const core::HotkeySpec* spec;
    } hotkeys[] = {
        { L"截图", &settings_.screenshotHotkey }, { L"OCR", &settings_.ocrHotkey },
        { L"录制", &settings_.recordHotkey },     { L"贴图", &settings_.pinHotkey },
        { L"圆盘", &settings_.radialHotkey },
    };
    int hotkeyY = 20;
    for (const auto& hotkey : hotkeys) {
        AddLabel(Page::Hotkeys, x, hotkeyY, 560,
                 std::wstring(hotkey.name) + L"：" +
                     Utf8ToWide(core::FormatHotkeyText(*hotkey.spec)));
        hotkeyY += 32;
    }
    AddLabel(Page::Hotkeys, x, hotkeyY + 12, 560, L"热键捕获与冲突提示在下一轮接。");

    // ---- 外观 ----
    AddLabel(Page::Appearance, x, 20, labelWidth, L"托盘菜单尺寸");
    menuSize_ = AddCombo(Page::Appearance, x, 44, controlWidth, { L"小", L"中", L"大" },
                         static_cast<int>(settings_.menuSize));
    pinGlow_ = AddCheck(Page::Appearance, x, 92, 300, L"贴图窗口显示光晕", settings_.pinGlowEnabled);

    // ---- 关于 / 诊断 ----
    {
        std::wstring summary;
        summary += L"HaloCap 2.0（原生重构）\n\n";
        summary += L"系统版本：" + std::to_wstring(facts_.osBuild) + L"（" +
                   (core::MeetsWindowsFloor(facts_.osBuild) ? L"支持" : L"低于下限") + L"）\n";
        summary += std::wstring(L"Windows.Graphics.Capture：") +
                   (facts_.graphicsCaptureSupported ? L"可用" : L"不可用") + L"\n";
        summary += L"进程私有内存：" +
                   std::to_wstring(facts_.privateMemoryBytes / (1024 * 1024)) + L" MB\n";
        summary += L"OCR 引擎：系统 OCR" +
                   std::wstring(modelsDirectory_.empty() ? L"（模型未找到）" : L" + PP-OCRv6 tiny") +
                   L"\n";
        summary += L"模型目录：" +
                   (modelsDirectory_.empty() ? L"未找到" : Utf8ToWide(modelsDirectory_)) + L"\n";
        summary += L"设置：%APPDATA%\\HaloCap2\\settings.json\n";
        summary += L"日志：%APPDATA%\\HaloCap2\\logs\n";

        HWND about = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", summary.c_str(),
                                     WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, x, 20,
                                     560, 470, window_, nullptr, GetModuleHandleW(nullptr),
                                     nullptr);
        SendMessageW(about, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        controls_.push_back({ about, Page::About });
        ShowWindow(about, SW_HIDE);
    }

    ShowPage(Page::Screenshot);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
}

void SettingsWindow::ShowPage(Page page) {
    current_ = page;
    for (const PageControl& control : controls_) {
        ShowWindow(control.handle, control.page == page ? SW_SHOW : SW_HIDE);
    }
}

void SettingsWindow::Save() {
    if (screenshotFormat_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(screenshotFormat_, CB_GETCURSEL, 0, 0));
        settings_.screenshotFormat = index == 1 ? core::ScreenshotFormat::Jpeg
                                                : core::ScreenshotFormat::Png;
    }
    if (recordingResolution_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingResolution_, CB_GETCURSEL, 0, 0));
        settings_.recording.resolution = static_cast<core::RecordingResolution>(
            std::clamp(index, 0, 4));
    }
    if (recordingFrameRate_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingFrameRate_, CB_GETCURSEL, 0, 0));
        settings_.recording.frameRate =
            index == 1 ? core::RecordingFrameRate::Sixty : core::RecordingFrameRate::Thirty;
    }
    if (recordingQuality_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingQuality_, CB_GETCURSEL, 0, 0));
        settings_.recording.quality = static_cast<core::RecordingQuality>(std::clamp(index, 0, 2));
    }
    if (recordingRange_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingRange_, CB_GETCURSEL, 0, 0));
        settings_.recording.range = static_cast<core::RecordingRange>(std::clamp(index, 0, 1));
    }
    if (recordingCountdown_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingCountdown_, CB_GETCURSEL, 0, 0));
        settings_.recording.countdown = index == 2 ? core::RecordingCountdown::FiveSeconds
                                                   : (index == 1
                                                          ? core::RecordingCountdown::ThreeSeconds
                                                          : core::RecordingCountdown::Off);
    }
    if (recordingBitrate_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(recordingBitrate_, CB_GETCURSEL, 0, 0));
        settings_.recording.audioBitrate = index == 1 ? core::RecordingAudioBitrate::K192
                                                      : core::RecordingAudioBitrate::K128;
    }
    if (systemAudio_ != nullptr) {
        settings_.recording.systemAudio =
            SendMessageW(systemAudio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (microphone_ != nullptr) {
        settings_.recording.microphone =
            SendMessageW(microphone_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (recordingDirectory_ != nullptr) {
        settings_.recordingDirectory = WideToUtf8(GetText(recordingDirectory_));
    }
    if (ocrEngine_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(ocrEngine_, CB_GETCURSEL, 0, 0));
        settings_.ocr.engine =
            index == 1 ? core::OcrEngine::Model : core::OcrEngine::Native;
    }
    if (ocrAutoCopy_ != nullptr) {
        settings_.ocr.autoCopy = SendMessageW(ocrAutoCopy_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (ocrPin_ != nullptr) {
        settings_.ocr.pinOcrEnabled =
            SendMessageW(ocrPin_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }
    if (menuSize_ != nullptr) {
        const int index = static_cast<int>(SendMessageW(menuSize_, CB_GETCURSEL, 0, 0));
        settings_.menuSize = static_cast<core::MenuSize>(std::clamp(index, 0, 2));
    }
    if (pinGlow_ != nullptr) {
        settings_.pinGlowEnabled = SendMessageW(pinGlow_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    if (onSave_ != nullptr && onSave_(settings_)) {
        saved_ = true;
        DestroyWindow(window_);
    }
}

void SettingsWindow::Close() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    if (background_ != nullptr) {
        DeleteObject(background_);
        background_ = nullptr;
    }
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND window, UINT message, WPARAM wParam,
                                         LPARAM lParam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
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

LRESULT SettingsWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wParam) == 100 && HIWORD(wParam) == LBN_SELCHANGE) {
            const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
            ShowPage(static_cast<Page>(std::clamp(index, 0, 5)));
            return 0;
        }
        if (LOWORD(wParam) == 200) {
            Save();
            return 0;
        }
        if (LOWORD(wParam) == 201) {
            DestroyWindow(window);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        window_ = nullptr;
        list_ = nullptr;
        saveButton_ = nullptr;
        cancelButton_ = nullptr;
        controls_.clear();
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

}
