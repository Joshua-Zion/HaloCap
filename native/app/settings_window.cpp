#include "settings_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "update.h"
#include "version.h"

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapSettings";
constexpr int kListWidth = 190;
constexpr int kTitleBar = 38;
constexpr int kPageTitleShift = 26;

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

namespace {

namespace {

void CopyTextToClipboard(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

std::string ReadLocalUpdateManifest() {
    wchar_t localAppData[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH) == 0) {
        return {};
    }
    const std::wstring path = std::wstring(localAppData) + L"\\HaloCap2\\update.json";
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(stream)),
                       std::istreambuf_iterator<char>());
}

}  // namespace

SettingsWindow* g_hotkeyOwner = nullptr;
int g_hotkeyIndex = -1;
HHOOK g_hotkeyHook = nullptr;

core::KeySymbol KeyFromVirtualKey(DWORD vk) {
    if (vk >= 'A' && vk <= 'Z') {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::A) + (vk - 'A'));
    }
    if (vk >= '0' && vk <= '9') {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::Num0) + (vk - '0'));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return static_cast<core::KeySymbol>(static_cast<int>(core::KeySymbol::F1) + (vk - VK_F1));
    }
    switch (vk) {
    case VK_ESCAPE: return core::KeySymbol::Escape;
    case VK_RETURN: return core::KeySymbol::Enter;
    case VK_SPACE: return core::KeySymbol::Space;
    case VK_TAB: return core::KeySymbol::Tab;
    case VK_BACK: return core::KeySymbol::Backspace;
    case VK_INSERT: return core::KeySymbol::Insert;
    case VK_DELETE: return core::KeySymbol::Delete;
    case VK_HOME: return core::KeySymbol::Home;
    case VK_END: return core::KeySymbol::End;
    case VK_PRIOR: return core::KeySymbol::PageUp;
    case VK_NEXT: return core::KeySymbol::PageDown;
    case VK_UP: return core::KeySymbol::Up;
    case VK_DOWN: return core::KeySymbol::Down;
    case VK_LEFT: return core::KeySymbol::Left;
    case VK_RIGHT: return core::KeySymbol::Right;
    case VK_SNAPSHOT: return core::KeySymbol::PrintScreen;
    default: return core::KeySymbol::None;
    }
}

}  // namespace
SettingsWindow::SettingsWindow() = default;

SettingsWindow::~SettingsWindow() { Close(); }

bool SettingsWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &SettingsWindow::WndProc;
        description.style = CS_DROPSHADOW;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        description.hbrBackground = nullptr;
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

    window_ = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"HaloCap 设置", WS_POPUP, 0, 0, 840, 600,
                              static_cast<HWND>(owner), nullptr, GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    font_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Microsoft YaHei UI");
    titleFont_ = CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    // 1.x 设置页的浅色配色：内容底 #F7F7F9、侧栏 #EDEDF1、卡片白、强调 #007AFF。
    background_ = CreateSolidBrush(RGB(247, 247, 249));
    sidebarBrush_ = CreateSolidBrush(RGB(237, 237, 241));
    cardBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    accentBrush_ = CreateSolidBrush(RGB(0, 122, 255));
    cardBorderPen_ = CreatePen(PS_SOLID, 1, RGB(231, 231, 236));
    fieldBorderPen_ = CreatePen(PS_SOLID, 1, RGB(217, 217, 222));

    list_ = CreateWindowExW(0, L"LISTBOX", L"",
                            WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL |
                                LBS_OWNERDRAWFIXED | LBS_HASSTRINGS,
                            12, 12, kListWidth, 500, window_, reinterpret_cast<HMENU>(100),
                            GetModuleHandleW(nullptr), nullptr);
    SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SendMessageW(list_, LB_SETITEMHEIGHT, 0, 34);
    for (const wchar_t* item : { L"截图", L"录制", L"OCR", L"快捷键", L"外观", L"关于 / 诊断" }) {
        SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    }
    SendMessageW(list_, LB_SETCURSEL, 0, 0);

    saveButton_ = CreateWindowExW(0, L"BUTTON", L"保存",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 620, 520, 90, 34,
                                  window_, reinterpret_cast<HMENU>(200),
                                  GetModuleHandleW(nullptr), nullptr);
    cancelButton_ = CreateWindowExW(0, L"BUTTON", L"取消",
                                    WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 720, 520, 90, 34,
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
                 L"（可在「快捷键」页修改）");

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
    for (int i = 0; i < 5; ++i) {
        AddLabel(Page::Hotkeys, x, hotkeyY, 56, hotkeys[i].name);
        hotkeyValues_[i] = AddLabel(Page::Hotkeys, x + 64, hotkeyY, 210,
                                    Utf8ToWide(core::FormatHotkeyText(*hotkeys[i].spec)));
        HWND button = CreateWindowExW(0, L"BUTTON", L"设置",
                                      WS_CHILD | BS_PUSHBUTTON, x + 286, hotkeyY, 64, 24,
                                      window_, reinterpret_cast<HMENU>(300 + i),
                                      GetModuleHandleW(nullptr), nullptr);
        SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        controls_.push_back({ button, Page::Hotkeys });
        ShowWindow(button, SW_HIDE);
        hotkeyButtons_[i] = button;
        hotkeyY += 34;
    }
    AddLabel(Page::Hotkeys, x, hotkeyY + 10, 560, L"点「设置」后按下新的组合键；Esc 取消。");

    // ---- 外观 ----
    AddLabel(Page::Appearance, x, 20, labelWidth, L"托盘菜单尺寸");
    menuSize_ = AddCombo(Page::Appearance, x, 44, controlWidth, { L"小", L"中", L"大" },
                         static_cast<int>(settings_.menuSize));
    pinGlow_ = AddCheck(Page::Appearance, x, 92, 300, L"贴图窗口显示光晕", settings_.pinGlowEnabled);
    autoStart_ = AddCheck(Page::Appearance, x, 126, 300, L"开机自动启动", settings_.autoStart);

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

        summaryText_ = summary;
        HWND about = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", summary.c_str(),
                                     WS_CHILD | ES_MULTILINE | ES_READONLY | WS_VSCROLL, x, 20,
                                     560, 420, window_, nullptr, GetModuleHandleW(nullptr),
                                     nullptr);
        SendMessageW(about, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        controls_.push_back({ about, Page::About });
        ShowWindow(about, SW_HIDE);

        copySummaryButton_ = CreateWindowExW(0, L"BUTTON", L"复制环境摘要",
                                             WS_CHILD | BS_PUSHBUTTON, x, 456, 140, 30,
                                             window_, reinterpret_cast<HMENU>(7),
                                             GetModuleHandleW(nullptr), nullptr);
        checkUpdateButton_ = CreateWindowExW(0, L"BUTTON", L"检查更新",
                                             WS_CHILD | BS_PUSHBUTTON, x + 152, 456, 120, 30,
                                             window_, reinterpret_cast<HMENU>(8),
                                             GetModuleHandleW(nullptr), nullptr);
        for (HWND button : { copySummaryButton_, checkUpdateButton_ }) {
            SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            controls_.push_back({ button, Page::About });
            ShowWindow(button, SW_HIDE);
        }
    }

    // 1.x 是无边框窗：把已建好的控件整体下移一个自绘标题栏的高度。
    HWND shifted[] = { list_, saveButton_, cancelButton_ };
    for (HWND control : shifted) {
        RECT rect{};
        GetWindowRect(control, &rect);
        MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(control, nullptr, rect.left, rect.top + kTitleBar, rect.right - rect.left,
                     rect.bottom - rect.top, SWP_NOZORDER);
    }
    for (const PageControl& control : controls_) {
        RECT rect{};
        GetWindowRect(control.handle, &rect);
        MapWindowPoints(HWND_DESKTOP, window_, reinterpret_cast<POINT*>(&rect), 2);
        SetWindowPos(control.handle, nullptr, rect.left, rect.top + kTitleBar + kPageTitleShift,
                     rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER);
    }

    HWND closeButton = CreateWindowExW(0, L"BUTTON", L"",
                                       WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 840 - 44, 7, 30, 24,
                                       window_, reinterpret_cast<HMENU>(202),
                                       GetModuleHandleW(nullptr), nullptr);
    SendMessageW(closeButton, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(window_, HWND_TOPMOST, (screenWidth - 840) / 2, (screenHeight - 600) / 2, 840,
                 600, SWP_NOZORDER);
    const DWORD cornerPreference = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(window_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPreference,
                          sizeof(cornerPreference));

    ShowPage(Page::Screenshot);
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
}

void SettingsWindow::ShowPage(Page page) {
    current_ = page;
    currentPage_ = page;
    for (const PageControl& control : controls_) {
        ShowWindow(control.handle, control.page == page ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(window_, nullptr, TRUE);
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
    if (autoStart_ != nullptr) {
        settings_.autoStart = SendMessageW(autoStart_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    if (onSave_ != nullptr && onSave_(settings_)) {
        saved_ = true;
        DestroyWindow(window_);
    }
}

void SettingsWindow::StartHotkeyCapture(int index) {
    if (index < 0 || index >= 5) {
        return;
    }
    if (g_hotkeyHook != nullptr) {
        UnhookWindowsHookEx(g_hotkeyHook);
        g_hotkeyHook = nullptr;
    }
    captureIndex_ = index;
    g_hotkeyOwner = this;
    g_hotkeyIndex = index;
    if (hotkeyValues_[index] != nullptr) {
        SetWindowTextW(hotkeyValues_[index], L"按下新的组合键…");
    }
    g_hotkeyHook = SetWindowsHookExW(WH_KEYBOARD_LL, &SettingsWindow::HotkeyHook,
                                     GetModuleHandleW(nullptr), 0);
}

void SettingsWindow::FinishHotkeyCapture(const core::HotkeySpec& spec) {
    if (captureIndex_ >= 0 && captureIndex_ < 5) {
        switch (captureIndex_) {
        case 0: settings_.screenshotHotkey = spec; break;
        case 1: settings_.ocrHotkey = spec; break;
        case 2: settings_.recordHotkey = spec; break;
        case 3: settings_.pinHotkey = spec; break;
        case 4: settings_.radialHotkey = spec; break;
        default: break;
        }
    }
    captureIndex_ = -1;
    g_hotkeyOwner = nullptr;
    g_hotkeyIndex = -1;
    if (g_hotkeyHook != nullptr) {
        UnhookWindowsHookEx(g_hotkeyHook);
        g_hotkeyHook = nullptr;
    }
    RefreshHotkeyLabels();
}

void SettingsWindow::RefreshHotkeyLabels() {
    const core::HotkeySpec* specs[5] = { &settings_.screenshotHotkey, &settings_.ocrHotkey,
                                         &settings_.recordHotkey, &settings_.pinHotkey,
                                         &settings_.radialHotkey };
    for (int i = 0; i < 5; ++i) {
        if (hotkeyValues_[i] != nullptr) {
            SetWindowTextW(hotkeyValues_[i],
                           Utf8ToWide(core::FormatHotkeyText(*specs[i])).c_str());
        }
    }
}

LRESULT CALLBACK SettingsWindow::HotkeyHook(int code, WPARAM wParam, LPARAM lParam) {
    if (code < 0 || g_hotkeyOwner == nullptr) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    if (wParam != WM_KEYDOWN && wParam != WM_SYSKEYDOWN) {
        return CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const DWORD vk = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam)->vkCode;
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) {
        return 1;
    }
    if (vk == VK_ESCAPE) {
        g_hotkeyOwner->captureIndex_ = -1;
        g_hotkeyOwner->RefreshHotkeyLabels();
        g_hotkeyOwner = nullptr;
        if (g_hotkeyHook != nullptr) {
            UnhookWindowsHookEx(g_hotkeyHook);
            g_hotkeyHook = nullptr;
        }
        return 1;
    }

    core::HotkeySpec spec;
    spec.key = KeyFromVirtualKey(vk);
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        spec.modifiers |= core::kModControl;
    }
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        spec.modifiers |= core::kModAlt;
    }
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        spec.modifiers |= core::kModShift;
    }
    if (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) != 0) {
        spec.modifiers |= core::kModWin;
    }

    if (spec.key == core::KeySymbol::None || !core::CheckHotkey(spec).valid) {
        Beep(800, 60);
        return 1;
    }

    g_hotkeyOwner->FinishHotkeyCapture(spec);
    return 1;
}
void SettingsWindow::Close() {
    if (g_hotkeyHook != nullptr) {
        UnhookWindowsHookEx(g_hotkeyHook);
        g_hotkeyHook = nullptr;
    }
    g_hotkeyOwner = nullptr;
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    for (HBRUSH* brush : { &background_, &sidebarBrush_, &cardBrush_, &accentBrush_ }) {
        if (*brush != nullptr) {
            DeleteObject(*brush);
            *brush = nullptr;
        }
    }
    for (HPEN* pen : { &cardBorderPen_, &fieldBorderPen_ }) {
        if (*pen != nullptr) {
            DeleteObject(*pen);
            *pen = nullptr;
        }
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
    case WM_ERASEBKGND: {
        // 1.x：整窗 #F7F7F9，右侧内容区是一张 12 px 圆角的白卡。
        HDC dc = reinterpret_cast<HDC>(wParam);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(dc, &client, background_);

        // 自绘标题栏：标题 + 右侧由 owner-draw 按钮画的关闭叉。
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(29, 29, 31));
        HGDIOBJ oldFont = SelectObject(dc, font_);
        RECT titleRect{ 16, 9, client.right - 60, 9 + 24 };
        DrawTextW(dc, L"HaloCap 设置", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldFont);

        static const wchar_t* const kPageTitles[] = { L"截图", L"录制", L"OCR",
                                                      L"快捷键", L"外观",
                                                      L"关于 / 诊断" };
        const int pageIndex = std::clamp(static_cast<int>(currentPage_), 0, 5);
        HGDIOBJ oldTitleFont = SelectObject(dc, titleFont_);
        SetTextColor(dc, RGB(29, 29, 31));
        RECT pageTitle{ 224, 12 + kTitleBar + 6, client.right - 24, 12 + kTitleBar + 32 };
        DrawTextW(dc, kPageTitles[pageIndex], -1, &pageTitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldTitleFont);

        const RECT card{ 212, 12 + kTitleBar, client.right - 12,
                     client.bottom - 64 + kTitleBar };
        HGDIOBJ oldPen = SelectObject(dc, cardBorderPen_);
        HGDIOBJ oldBrush = SelectObject(dc, cardBrush_);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, 24, 24);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(cardBrush_);
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(cardBrush_);
    }

    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(110, 110, 115));
        SetBkColor(dc, RGB(237, 237, 241));
        return reinterpret_cast<LRESULT>(sidebarBrush_);
    }

    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item == nullptr) {
            return 0;
        }
        HDC dc = item->hDC;
        SetBkMode(dc, TRANSPARENT);

        if (item->CtlID == 100) {
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            FillRect(dc, &item->rcItem, sidebarBrush_);
            RECT textRect = item->rcItem;
            textRect.left += 14;
            if (selected) {
                HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
                HGDIOBJ oldBrush = SelectObject(dc, CreateSolidBrush(RGB(224, 224, 230)));
                RECT pill{ item->rcItem.left + 8, item->rcItem.top + 3,
                           item->rcItem.right - 8, item->rcItem.bottom - 3 };
                RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, 16, 16);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                SetTextColor(dc, RGB(29, 29, 31));
            } else {
                SetTextColor(dc, RGB(110, 110, 115));
            }
            wchar_t text[128] = {};
            const LRESULT length = SendMessageW(list_, LB_GETTEXT, item->itemID,
                                                reinterpret_cast<LPARAM>(text));
            if (length > 0) {
                DrawTextW(dc, text, static_cast<int>(length), &textRect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
            return 1;
        }

        if (item->CtlID == 202) {
            const RECT& r = item->rcItem;
            const int centerX = (r.left + r.right) / 2;
            const int centerY = (r.top + r.bottom) / 2;
            HGDIOBJ oldPen =
                SelectObject(dc, CreatePen(PS_SOLID, 1, RGB(90, 90, 96)));
            MoveToEx(dc, centerX - 5, centerY - 5, nullptr);
            LineTo(dc, centerX + 5, centerY + 5);
            MoveToEx(dc, centerX + 5, centerY - 5, nullptr);
            LineTo(dc, centerX - 5, centerY + 5);
            DeleteObject(SelectObject(dc, oldPen));
            return 1;
        }

        if (item->CtlID == 200 || item->CtlID == 201) {
            const bool primary = item->CtlID == 200;
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            HGDIOBJ oldPen = SelectObject(dc, primary ? GetStockObject(NULL_PEN) : fieldBorderPen_);
            HGDIOBJ oldBrush = SelectObject(
                dc, primary ? accentBrush_ : cardBrush_);
            const RECT& r = item->rcItem;
            RoundRect(dc, r.left, r.top, r.right, r.bottom, 20, 20);
            SelectObject(dc, oldBrush);
            SelectObject(dc, oldPen);

            SetTextColor(dc, primary ? RGB(255, 255, 255) : RGB(29, 29, 31));
            if (pressed) {
                RECT shade = r;
                FrameRect(dc, &shade, static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));
            }
            wchar_t text[32] = {};
            const LRESULT length = GetWindowTextW(item->hwndItem, text, 32);
            DrawTextW(dc, text, static_cast<int>(length), const_cast<RECT*>(&r),
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            return 1;
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 100 && HIWORD(wParam) == LBN_SELCHANGE) {
            const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
            ShowPage(static_cast<Page>(std::clamp(index, 0, 5)));
            return 0;
        }
        if (LOWORD(wParam) == 202) {
            DestroyWindow(window);
            return 0;
        }
        if (LOWORD(wParam) == 7) {
            CopyTextToClipboard(summaryText_);
            MessageBoxW(window, L"环境摘要已复制到剪贴板。", L"HaloCap", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (LOWORD(wParam) == 8) {
            const std::string json = ReadLocalUpdateManifest();
            if (json.empty()) {
                MessageBoxW(window, L"未找到更新清单（%LOCALAPPDATA%\\HaloCap2\\update.json）。",
                            L"HaloCap 更新", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            core::UpdateManifest manifest;
            std::string error;
            if (!core::ParseUpdateManifest(json, &manifest, &error)) {
                MessageBoxW(window, L"更新清单解析失败，详见日志。", L"HaloCap 更新",
                            MB_OK | MB_ICONWARNING);
                return 0;
            }
            const bool newer = core::IsNewerVersion(manifest.version, halocap::app::kHaloCapVersion);
            std::wstring message = L"当前版本：" +
                                   Utf8ToWide(halocap::app::kHaloCapVersion) +
                                   L"\n最新版本：" + Utf8ToWide(manifest.version) + L"\n\n";
            message += newer ? L"发现新版本，可从清单中的地址下载。" : L"已经是最新版本。";
            if (newer) {
                message += L"\n" + Utf8ToWide(manifest.url);
            }
            MessageBoxW(window, message.c_str(), L"HaloCap 更新", MB_OK | MB_ICONINFORMATION);
            return 0;
        }

        if (LOWORD(wParam) >= 300 && LOWORD(wParam) <= 304) {
            StartHotkeyCapture(static_cast<int>(LOWORD(wParam)) - 300);
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
