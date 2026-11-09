#include "ocr_result_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapOcrResult";

// 1.x OcrResultWindow 的配色。
constexpr COLORREF kTextPrimary = RGB(0x1D, 0x1D, 0x1F);
constexpr COLORREF kTextSecondary = RGB(0x6E, 0x6E, 0x73);
constexpr COLORREF kTextTertiary = RGB(0x8E, 0x8E, 0x93);
constexpr COLORREF kAccent = RGB(0x00, 0x7A, 0xFF);
constexpr COLORREF kDanger = RGB(0xD7, 0x00, 0x15);
constexpr COLORREF kCardBg = RGB(0xF7, 0xF7, 0xF9);
constexpr COLORREF kCardBorder = RGB(0xEC, 0xEC, 0xEF);
constexpr COLORREF kWarningBg = RGB(0xFF, 0xF8, 0xE8);
constexpr COLORREF kWarningBorder = RGB(0xF0, 0xE1, 0xB9);
constexpr COLORREF kWarningText = RGB(0x8A, 0x61, 0x16);
constexpr COLORREF kChipBg = RGB(0xED, 0xF5, 0xFF);
constexpr COLORREF kChipBorder = RGB(0xD8, 0xE9, 0xFF);
constexpr COLORREF kChipText = RGB(0x00, 0x68, 0xD9);
constexpr COLORREF kButtonBorder = RGB(0xD6, 0xD6, 0xDB);
constexpr COLORREF kHairline = RGB(0xE7, 0xE7, 0xEC);
constexpr COLORREF kSegmentBg = RGB(0xF0, 0xF0, 0xF3);
constexpr COLORREF kSegmentBorder = RGB(0xE2, 0xE2, 0xE7);
constexpr COLORREF kSegmentActiveBorder = RGB(0xDC, 0xDC, 0xE1);

constexpr int kWidthDip = 560;
constexpr int kPadDip = 20;
constexpr int kTitleDip = 58;
constexpr int kContentDip = 190;
constexpr int kFooterDip = 58;
constexpr int kRowDip = 30;
constexpr int kMaxLines = 500;

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(),
                        length);
    return wide;
}

void FillRounded(HDC dc, const RECT& rect, int radius, COLORREF fill, COLORREF border,
                 int borderWidth) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, borderWidth > 0 ? pen : GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);
}

void DrawTextIn(HDC dc, const std::wstring& text, HFONT font, COLORREF color, RECT rect,
                UINT flags) {
    if (font == nullptr) {
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
    SelectObject(dc, oldFont);
}

int MeasureTextHeight(HDC dc, const std::wstring& text, HFONT font, int width) {
    RECT rect{ 0, 0, width, 0 };
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_WORDBREAK | DT_CALCRECT);
    SelectObject(dc, oldFont);
    return rect.bottom - rect.top;
}

bool PointIn(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

RECT Inset(const RECT& rect, int dx, int dy) {
    return RECT{ rect.left + dx, rect.top + dy, rect.right - dx, rect.bottom - dy };
}

}  // namespace

OcrResultWindow::OcrResultWindow() = default;

OcrResultWindow::~OcrResultWindow() { Close(); }

bool OcrResultWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.style = CS_DROPSHADOW;
        description.lpfnWndProc = &OcrResultWindow::WndProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        description.lpszClassName = kClassName;
        return RegisterClassExW(&description) != 0 ||
               GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

bool OcrResultWindow::Create(void* owner, const std::uint8_t* bgra, int width, int height,
                             const OcrResultOptions& options, OcrRunner runner) {
    if (bgra == nullptr || width <= 0 || height <= 0 || !EnsureClass()) {
        return false;
    }

    options_ = options;
    runner_ = std::move(runner);
    engine_ = options_.engine;
    fontFamilyW_ = Utf8ToWide(options_.fontFamily);
    if (fontFamilyW_.empty()) {
        fontFamilyW_ = L"Microsoft YaHei UI";
    }
    image_.assign(bgra, bgra + static_cast<std::size_t>(width) * height * 4);
    imageWidth_ = width;
    imageHeight_ = height;
    showImage_ = options_.showImage;
    searchLabel_ = L"左侧为识别区域，框出的每一段即为右侧的一行";

    const std::wstring title = options_.fromPin ? L"HaloCap 贴图识别" : L"HaloCap 文字识别";
    window_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClassName, title.c_str(),
                              WS_POPUP, 0, 0, 100, 100, static_cast<HWND>(owner), nullptr,
                              GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    const UINT dpi = GetDpiForWindow(window_);
    uiScale_ = dpi >= 48 ? static_cast<float>(dpi) / 96.0f : 1.0f;
    if (uiScale_ < 1.0f) {
        uiScale_ = 1.0f;
    }
    BuildFonts();

    text_ = CreateWindowExW(0, L"EDIT", L"",
                            WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL,
                            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(1),
                            GetModuleHandleW(nullptr), nullptr);
    search_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10,
                              10, window_, reinterpret_cast<HMENU>(2),
                              GetModuleHandleW(nullptr), nullptr);
    SendMessageW(text_, WM_SETFONT, reinterpret_cast<WPARAM>(fontEdit_), TRUE);
    SendMessageW(search_, WM_SETFONT, reinterpret_cast<WPARAM>(fontSearch_), TRUE);
    SendMessageW(search_, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"搜索识别结果"));

    SetWindowLongPtrW(text_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(search_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    resultTextProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(text_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditProc)));
    searchTextProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(search_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditProc)));

    const DWORD cornerPreference = 2;
    DwmSetWindowAttribute(window_, 33, &cornerPreference, sizeof(cornerPreference));

    Layout();
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(window_);
    UpdateWindow(window_);
    if (!options_.skipRecognition) {
        StartRecognition();
    }
    return true;
}

void OcrResultWindow::BuildFonts() {
    const auto make = [&](float dip, int weight) {
        return CreateFontW(-ScaleInt(dip), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, fontFamilyW_.c_str());
    };
    fontTitle_ = make(14.5f, FW_SEMIBOLD);
    fontSubtitle_ = make(10.5f, FW_NORMAL);
    fontBody_ = make(12.5f, FW_NORMAL);
    fontSmall_ = make(10.5f, FW_NORMAL);
    fontButton_ = make(12.0f, FW_NORMAL);
    fontIcon_ = make(13.0f, FW_SEMIBOLD);
    fontEdit_ = make(13.0f, FW_NORMAL);
    fontSearch_ = make(11.5f, FW_NORMAL);
    whiteBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    resultBrush_ = CreateSolidBrush(kCardBg);
    borderPen_ = CreatePen(PS_SOLID, 1, kButtonBorder);
}

void OcrResultWindow::DeleteResources() {
    for (HFONT* font : { &fontTitle_, &fontSubtitle_, &fontBody_, &fontSmall_, &fontButton_,
                         &fontIcon_, &fontEdit_, &fontSearch_ }) {
        if (*font != nullptr) {
            DeleteObject(*font);
            *font = nullptr;
        }
    }
    for (HBRUSH* brush : { &whiteBrush_, &resultBrush_ }) {
        if (*brush != nullptr) {
            DeleteObject(*brush);
            *brush = nullptr;
        }
    }
    if (borderPen_ != nullptr) {
        DeleteObject(borderPen_);
        borderPen_ = nullptr;
    }
}

void OcrResultWindow::Close() {
    if (window_ != nullptr) {
        KillTimer(window_, 1);
        KillTimer(window_, 2);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    DeleteResources();
}

// ---- 识别流程 ----

void OcrResultWindow::StartRecognition() {
    if (window_ == nullptr) {
        return;
    }
    if (!runner_) {
        stage_ = Stage::Failed;
        warning_ = L"没有可用的识别入口";
        Layout();
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }
    stage_ = Stage::Loading;
    warning_.clear();
    matches_.clear();
    matchIndex_ = 0;
    lastQuery_.clear();
    result_ = core::ocr::OcrResult{};
    plainText_.clear();
    elapsedMs_ = 0.0;
    lineScroll_ = 0;
    hoveredLine_ = -1;
    if (text_ != nullptr) {
        SetWindowTextW(text_, L"");
    }
    if (searchOpen_) {
        CloseSearch(false);
    }
    ++token_;
    loadingPhase_ = 0;
    SetTimer(window_, 1, 16, nullptr);
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
    runner_(engine_, token_, image_.data(), imageWidth_, imageHeight_, window_);
}

void OcrResultWindow::ApplyOutcome(const OcrOutcome& outcome) {
    KillTimer(window_, 1);
    engine_ = outcome.engine;
    elapsedMs_ = outcome.elapsedMs;
    result_ = outcome.result;
    plainText_ = Utf8ToWide(result_.plainText);
    lineScroll_ = 0;
    hoveredLine_ = -1;
    if (!outcome.error.empty()) {
        stage_ = Stage::Failed;
        warning_ = Utf8ToWide(outcome.error);
    } else if (result_.lines.empty()) {
        stage_ = Stage::Empty;
        warning_.clear();
    } else {
        stage_ = Stage::Success;
        warning_.clear();
    }
    if (text_ != nullptr) {
        SetWindowTextW(text_, plainText_.c_str());
    } else {
        stage_ = Stage::Failed;
    }
    if (stage_ == Stage::Success && options_.autoCopy && !plainText_.empty()) {
        CopyText(plainText_, L"已自动复制全部文字");
    } else {
        ResetStatus();
    }
    if (searchOpen_ && status_.empty()) {
        SetWindowTextW(search_, L"");
    }
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::PresentResultForTest(const core::ocr::OcrResult& result,
                                           double elapsedMs) {
    KillTimer(window_, 1);
    result_ = result;
    plainText_ = Utf8ToWide(result_.plainText);
    elapsedMs_ = elapsedMs;
    stage_ = result_.lines.empty() ? Stage::Empty : Stage::Success;
    warning_.clear();
    if (text_ != nullptr) {
        SetWindowTextW(text_, plainText_.c_str());
    }
    ResetStatus();
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

// ---- 状态与复制 ----

void OcrResultWindow::ShowStatus(const std::wstring& message, bool error) {
    status_ = message;
    statusError_ = error;
    if (window_ != nullptr) {
        SetTimer(window_, 2, 1800, nullptr);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void OcrResultWindow::ResetStatus() {
    status_ = stage_ == Stage::Success ? L"Esc 关闭 · 可选择文本 · Ctrl+F 搜索" : L"Esc 关闭";
    statusError_ = false;
    if (window_ != nullptr) {
        KillTimer(window_, 2);
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void OcrResultWindow::CopyText(const std::wstring& text, const std::wstring& message) {
    if (text.empty()) {
        return;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        ShowStatus(L"复制失败，剪贴板可能被占用", true);
        return;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        ShowStatus(L"复制失败，剪贴板可能被占用", true);
        return;
    }
    std::memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(window_)) {
        GlobalFree(memory);
        ShowStatus(L"复制失败，剪贴板可能被占用", true);
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        ShowStatus(L"复制失败，剪贴板可能被占用", true);
    } else {
        ShowStatus(message);
    }
    CloseClipboard();
}

void OcrResultWindow::CopyAll() {
    if (stage_ != Stage::Success || plainText_.empty()) {
        return;
    }
    CopyText(plainText_, L"已复制全部文字");
}

void OcrResultWindow::CopyLine(std::size_t index) {
    if (index >= result_.lines.size()) {
        return;
    }
    CopyText(Utf8ToWide(result_.lines[index].text), L"已复制这一行");
}

// ---- 视图 ----

void OcrResultWindow::SetLineView(bool lineView) {
    if (lineView_ == lineView) {
        return;
    }
    lineView_ = lineView;
    if (lineView_ && searchOpen_) {
        CloseSearch(false);
    }
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::SetImageVisible(bool visible) {
    if (showImage_ == visible) {
        return;
    }
    showImage_ = visible;
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::SetImageVisibleForTest(bool visible) { SetImageVisible(visible); }

core::IntRect OcrResultWindow::PreviewRectForTest() const {
    return core::IntRect{ previewImage_.left, previewImage_.top, previewImage_.right,
                          previewImage_.bottom };
}

void OcrResultWindow::OpenSearch() {
    if (stage_ != Stage::Success || searchOpen_) {
        return;
    }
    searchOpen_ = true;
    SetLineView(false);
    lastQuery_.clear();
    matches_.clear();
    matchIndex_ = 0;
    if (search_ != nullptr) {
        SetWindowTextW(search_, L"");
    }
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
    if (search_ != nullptr) {
        SetFocus(search_);
    }
}

void OcrResultWindow::CloseSearch(bool focusWindow) {
    if (!searchOpen_) {
        return;
    }
    searchOpen_ = false;
    lastQuery_.clear();
    matches_.clear();
    matchIndex_ = 0;
    if (search_ != nullptr) {
        SetWindowTextW(search_, L"");
        ShowWindow(search_, SW_HIDE);
    }
    searchLabel_ = L"左侧为识别区域，框出的每一段即为右侧的一行";
    Layout();
    InvalidateRect(window_, nullptr, FALSE);
    if (focusWindow) {
        SetFocus(window_);
    }
}

std::wstring OcrResultWindow::CurrentQuery() const {
    if (search_ == nullptr) {
        return {};
    }
    const int length = GetWindowTextLengthW(search_);
    std::wstring query(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(search_, query.data(), length + 1);
    query.resize(static_cast<std::size_t>(length));
    return query;
}

void OcrResultWindow::RefreshSearchStatus() {
    // 1.x 的状态文本就是 "3/17"；命中单位是「行」，上一处/下一处走的也是行。
    searchLabel_ = lastQuery_.empty()
                       ? L"左侧为识别区域，框出的每一段即为右侧的一行"
                       : (matches_.empty()
                              ? L"无匹配"
                              : std::to_wstring(matchIndex_ + 1) + L"/" +
                                    std::to_wstring(matches_.size()));
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::UpdateSearch(bool selectFirst) {
    if (search_ == nullptr || text_ == nullptr) {
        return;
    }
    const std::wstring query = CurrentQuery();
    const bool queryChanged = query != lastQuery_;
    lastQuery_ = query;

    if (query.empty() || result_.lines.empty()) {
        matches_.clear();
        matchIndex_ = 0;
        SendMessageW(text_, EM_SETSEL, 0, 0);
        RefreshSearchStatus();
        return;
    }

    const int utf8Length = WideCharToMultiByte(CP_UTF8, 0, query.c_str(),
                                               static_cast<int>(query.size()), nullptr, 0,
                                               nullptr, nullptr);
    std::string queryUtf8(static_cast<std::size_t>(utf8Length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, query.c_str(), static_cast<int>(query.size()),
                        queryUtf8.data(), utf8Length, nullptr, nullptr);

    matches_ = core::ocr::FindMatchingLines(result_, queryUtf8);
    if (queryChanged || matchIndex_ >= matches_.size()) {
        matchIndex_ = 0;
    }
    if (matches_.empty()) {
        SendMessageW(text_, EM_SETSEL, 0, 0);
    } else if (selectFirst) {
        SelectMatch(matchIndex_);
    }
    RefreshSearchStatus();
}

void OcrResultWindow::StepMatch(int direction) {
    if (!searchOpen_ || matches_.empty()) {
        return;
    }
    if (direction < 0) {
        matchIndex_ = matchIndex_ == 0 ? matches_.size() - 1 : matchIndex_ - 1;
    } else {
        matchIndex_ = (matchIndex_ + 1) % matches_.size();
    }
    SelectMatch(matchIndex_);
    RefreshSearchStatus();
}

void OcrResultWindow::SelectMatch(std::size_t index) {
    if (matches_.empty() || index >= matches_.size() || text_ == nullptr) {
        return;
    }
    const std::size_t lineIndex = matches_[index];
    std::size_t charOffset = 0;
    for (std::size_t i = 0; i < lineIndex && i < result_.lines.size(); ++i) {
        const std::string& line = result_.lines[i].text;
        charOffset += static_cast<std::size_t>(MultiByteToWideChar(
                          CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0)) +
                      1;
    }
    const std::string& lineText = result_.lines[lineIndex].text;
    const std::size_t charLength = static_cast<std::size_t>(MultiByteToWideChar(
        CP_UTF8, 0, lineText.c_str(), static_cast<int>(lineText.size()), nullptr, 0));
    SendMessageW(text_, EM_SETSEL, charOffset, charOffset + charLength);
    SendMessageW(text_, EM_SCROLLCARET, 0, 0);
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::SetQueryForTest(const std::wstring& query) {
    OpenSearch();
    if (search_ != nullptr) {
        SetWindowTextW(search_, query.c_str());
    }
}

std::wstring OcrResultWindow::StatusForTest() const { return searchLabel_; }

// ---- 布局 ----

void OcrResultWindow::Layout() {
    if (window_ == nullptr) {
        return;
    }
    const int pad = ScaleInt(kPadDip);
    width_ = ScaleInt(kWidthDip);

    titleRect_ = RECT{ 0, 0, width_, ScaleInt(kTitleDip) };
    closeTop_ = RECT{ width_ - pad + ScaleInt(6.0f) - ScaleInt(28.0f), ScaleInt(15.0f),
                      width_ - pad + ScaleInt(6.0f), ScaleInt(43.0f) };
    int y = ScaleInt(kTitleDip);

    const int chipHeight = ScaleInt(24.0f);
    chipRect_ = RECT{ pad, y, pad + ScaleInt(160.0f), y + chipHeight };
    y += chipHeight + ScaleInt(9.0f);

    const int groupHeight = ScaleInt(30.0f) + 2 * ScaleInt(2.0f);
    engineGroup_ = RECT{ pad, y, pad + ScaleInt(160.0f) + 2 * ScaleInt(2.0f),
                         y + groupHeight };
    engineNative_ = RECT{ pad + ScaleInt(2.0f), y + ScaleInt(2.0f),
                          pad + ScaleInt(2.0f) + ScaleInt(80.0f),
                          y + ScaleInt(2.0f) + ScaleInt(30.0f) };
    engineModel_ = RECT{ engineNative_.right, engineNative_.top,
                         engineNative_.right + ScaleInt(80.0f), engineNative_.bottom };
    const int smallWidth = ScaleInt(62.0f);
    const int smallHeight = ScaleInt(28.0f);
    const int smallTop = y + (groupHeight - smallHeight) / 2;
    searchButton_ = RECT{ width_ - pad - smallWidth * 2 - ScaleInt(6.0f), smallTop,
                          width_ - pad - smallWidth - ScaleInt(6.0f), smallTop + smallHeight };
    viewButton_ = RECT{ width_ - pad - smallWidth, smallTop, width_ - pad,
                        smallTop + smallHeight };
    y += groupHeight + ScaleInt(10.0f);

    if (searchOpen_) {
        const int searchHeight = ScaleInt(46.0f);
        searchBar_ = RECT{ pad, y, width_ - pad, y + searchHeight };
        const int editHeight = ScaleInt(28.0f);
        const int editTop = y + (searchHeight - editHeight) / 2;
        const int buttonWidth = ScaleInt(58.0f);
        const int closeWidth = ScaleInt(28.0f);
        int right = searchBar_.right - ScaleInt(10.0f);
        searchClose_ = RECT{ right - closeWidth, editTop, right, editTop + editHeight };
        right = searchClose_.left - ScaleInt(4.0f);
        searchNext_ = RECT{ right - buttonWidth, editTop, right, editTop + editHeight };
        right = searchNext_.left - ScaleInt(6.0f);
        searchPrev_ = RECT{ right - buttonWidth, editTop, right, editTop + editHeight };
        right = searchPrev_.left - ScaleInt(68.0f);
        searchEditRect_ = RECT{ searchBar_.left + ScaleInt(10.0f), editTop, right,
                                editTop + editHeight };
        y += searchHeight + ScaleInt(10.0f);
    } else {
        searchBar_ = RECT{};
        searchEditRect_ = RECT{};
        searchPrev_ = RECT{};
        searchNext_ = RECT{};
        searchClose_ = RECT{};
    }

    if (!warning_.empty()) {
        HDC dc = GetDC(window_);
        const int textHeight = MeasureTextHeight(dc, warning_, fontSmall_,
                                                width_ - 2 * pad - ScaleInt(24.0f));
        ReleaseDC(window_, dc);
        const int warningHeight = textHeight + ScaleInt(20.0f);
        warningRect_ = RECT{ pad, y, width_ - pad, y + warningHeight };
        y += warningHeight + ScaleInt(8.0f);
    } else {
        warningRect_ = RECT{};
    }

    int contentHeight = ScaleInt(kContentDip);
    const int footerHeight = ScaleInt(kFooterDip);
    int previewHeight = 0;
    if (showImage_ && imageWidth_ > 0) {
        const int available = width_ - 2 * pad - ScaleInt(12.0f);
        const int scaled = static_cast<int>(static_cast<float>(imageHeight_) * available /
                                            static_cast<float>(imageWidth_));
        const int imageHeight = std::clamp(scaled, ScaleInt(60.0f), ScaleInt(180.0f));
        previewHeight = imageHeight + ScaleInt(12.0f) + ScaleInt(8.0f);
    }
    const int maxContent = ScaleInt(660.0f) - (y + contentHeight + previewHeight +
                                               footerHeight + ScaleInt(14.0f));
    if (maxContent < 0) {
        contentHeight = std::max(ScaleInt(110.0f), contentHeight + maxContent);
    }
    contentRect_ = RECT{ pad, y, width_ - pad, y + contentHeight };
    y += contentHeight + ScaleInt(10.0f);

    if (previewHeight > 0) {
        previewCard_ = RECT{ pad, y, width_ - pad, y + previewHeight };
        previewImage_ = Inset(previewCard_, ScaleInt(6.0f), ScaleInt(6.0f));
        y += previewHeight;
    } else {
        previewCard_ = RECT{};
        previewImage_ = RECT{};
    }

    footerRect_ = RECT{ 0, y + ScaleInt(10.0f), width_, y + ScaleInt(10.0f) + footerHeight };
    height_ = footerRect_.bottom + ScaleInt(14.0f);

    const int buttonHeight = ScaleInt(34.0f);
    const int footerTop = footerRect_.top + (footerHeight - buttonHeight) / 2;
    int right = width_ - pad;
    const int copyWidth = ScaleInt(94.0f);
    copyAllButton_ = RECT{ right - copyWidth, footerTop, right, footerTop + buttonHeight };
    right = copyAllButton_.left - ScaleInt(9.0f);
    const int closeWidth = ScaleInt(72.0f);
    closeFooter_ = RECT{ right - closeWidth, footerTop, right, footerTop + buttonHeight };
    right = closeFooter_.left - ScaleInt(8.0f);
    const int imageWidth = ScaleInt(62.0f);
    imageButton_ = RECT{ right - imageWidth, footerTop, right, footerTop + buttonHeight };
    right = imageButton_.left - ScaleInt(8.0f);
    const int retryWidth = ScaleInt(62.0f);
    retryButton_ = RECT{ right - retryWidth, footerTop, right, footerTop + buttonHeight };

    const int emptyWidth = ScaleInt(132.0f);
    emptyRetry_ = RECT{ width_ / 2 - emptyWidth / 2,
                        contentRect_.top + ScaleInt(96.0f), width_ / 2 + emptyWidth / 2,
                        contentRect_.top + ScaleInt(96.0f) + ScaleInt(34.0f) };

    if (text_ != nullptr) {
        const RECT edit = Inset(contentRect_, ScaleInt(6.0f), ScaleInt(6.0f));
        MoveWindow(text_, edit.left, edit.top, edit.right - edit.left, edit.bottom - edit.top,
                   TRUE);
        RECT format{ ScaleInt(6.0f), ScaleInt(4.0f), edit.right - edit.left,
                     edit.bottom - edit.top };
        SendMessageW(text_, EM_SETRECT, 0, reinterpret_cast<LPARAM>(&format));
        ShowWindow(text_, stage_ == Stage::Success && !lineView_ ? SW_SHOW : SW_HIDE);
    }
    if (search_ != nullptr) {
        if (searchOpen_) {
            const RECT edit = Inset(searchEditRect_, ScaleInt(1.0f), ScaleInt(1.0f));
            MoveWindow(search_, edit.left, edit.top, edit.right - edit.left,
                       edit.bottom - edit.top, TRUE);
            ShowWindow(search_, SW_SHOW);
        } else {
            ShowWindow(search_, SW_HIDE);
        }
    }

    // 定位：优先贴选区下方，放不下就翻到上方。
    RECT anchor{ options_.anchor.left, options_.anchor.top, options_.anchor.right,
                 options_.anchor.bottom };
    if (anchor.right <= anchor.left || anchor.bottom <= anchor.top) {
        POINT cursor{};
        GetCursorPos(&cursor);
        anchor = RECT{ cursor.x, cursor.y, cursor.x + 1, cursor.y + 1 };
    }
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &info);
    const int margin = ScaleInt(12.0f);
    const int x = std::clamp(static_cast<int>(anchor.left),
                             static_cast<int>(info.rcWork.left) + margin,
                             std::max(static_cast<int>(info.rcWork.left) + margin,
                                      static_cast<int>(info.rcWork.right) - width_ - margin));
    int top = static_cast<int>(anchor.bottom) + margin;
    if (top + height_ > static_cast<int>(info.rcWork.bottom) - margin) {
        top = static_cast<int>(anchor.top) - margin - height_;
    }
    top = std::clamp(top, static_cast<int>(info.rcWork.top) + margin,
                     std::max(static_cast<int>(info.rcWork.top) + margin,
                              static_cast<int>(info.rcWork.bottom) - height_ - margin));
    SetWindowPos(window_, nullptr, x, top, width_, height_, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ---- 命中与交互 ----

OcrResultWindow::Hit OcrResultWindow::HitTest(POINT client) const {
    if (PointIn(closeTop_, client)) {
        return Hit::CloseTop;
    }
    if (stage_ != Stage::Loading) {
        if (PointIn(engineNative_, client)) {
            return Hit::EngineNative;
        }
        if (PointIn(engineModel_, client)) {
            return Hit::EngineModel;
        }
    }
    if (stage_ == Stage::Success) {
        if (PointIn(searchButton_, client)) {
            return Hit::SearchButton;
        }
        if (PointIn(viewButton_, client)) {
            return Hit::ViewButton;
        }
    }
    if (searchOpen_) {
        if (PointIn(searchPrev_, client)) {
            return Hit::SearchPrevious;
        }
        if (PointIn(searchNext_, client)) {
            return Hit::SearchNext;
        }
        if (PointIn(searchClose_, client)) {
            return Hit::SearchClose;
        }
    }
    if (PointIn(retryButton_, client)) {
        return Hit::Retry;
    }
    if (PointIn(imageButton_, client)) {
        return Hit::ImageToggle;
    }
    if (PointIn(closeFooter_, client)) {
        return Hit::CloseFooter;
    }
    if (PointIn(copyAllButton_, client)) {
        return Hit::CopyAll;
    }
    if ((stage_ == Stage::Empty || stage_ == Stage::Failed) &&
        PointIn(emptyRetry_, client)) {
        return Hit::EmptyRetry;
    }
    return Hit::None;
}

int OcrResultWindow::LineAt(POINT client) const {
    if (!lineView_ || stage_ != Stage::Success || !PointIn(contentRect_, client)) {
        return -1;
    }
    const int rowHeight = ScaleInt(kRowDip);
    const int row = (client.y - contentRect_.top - ScaleInt(6.0f)) / rowHeight;
    if (row < 0) {
        return -1;
    }
    const int index = lineScroll_ + row;
    if (index < 0 || index >= static_cast<int>(result_.lines.size())) {
        return -1;
    }
    return index;
}

void OcrResultWindow::HandleClick(Hit hit) {
    switch (hit) {
    case Hit::CloseTop:
    case Hit::CloseFooter:
        Close();
        return;
    case Hit::EngineNative:
        if (engine_ != core::OcrEngine::Native) {
            engine_ = core::OcrEngine::Native;
            StartRecognition();
        }
        return;
    case Hit::EngineModel:
        if (engine_ != core::OcrEngine::Model) {
            engine_ = core::OcrEngine::Model;
            StartRecognition();
        }
        return;
    case Hit::SearchButton:
        if (searchOpen_) {
            CloseSearch(true);
        } else {
            OpenSearch();
        }
        return;
    case Hit::ViewButton:
        if (stage_ == Stage::Success) {
            SetLineView(!lineView_);
        }
        return;
    case Hit::SearchPrevious:
        StepMatch(-1);
        return;
    case Hit::SearchNext:
        StepMatch(1);
        return;
    case Hit::SearchClose:
        CloseSearch(true);
        return;
    case Hit::Retry:
        StartRecognition();
        return;
    case Hit::ImageToggle:
        SetImageVisible(!showImage_);
        return;
    case Hit::CopyAll:
        CopyAll();
        return;
    case Hit::EmptyRetry:
        // 1.x：失败/空结果时用另一个引擎再试一次。
        engine_ = engine_ == core::OcrEngine::Native ? core::OcrEngine::Model
                                                     : core::OcrEngine::Native;
        StartRecognition();
        return;
    default:
        return;
    }
}

void OcrResultWindow::HandleEditKey(HWND edit, WPARAM key) {
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_ESCAPE) {
        if (searchOpen_) {
            CloseSearch(true);
        } else {
            Close();
        }
        return;
    }
    if (key == 'F' && control) {
        OpenSearch();
        return;
    }
    if (key == VK_F3) {
        if (searchOpen_) {
            StepMatch(shift ? -1 : 1);
        } else {
            OpenSearch();
        }
        return;
    }
    if (key == 'C' && control && shift) {
        CopyAll();
        return;
    }
    if (edit == search_ && key == VK_RETURN) {
        StepMatch(shift ? -1 : 1);
        return;
    }
}

LRESULT CALLBACK OcrResultWindow::EditProc(HWND window, UINT message, WPARAM wParam,
                                           LPARAM lParam) {
    auto* self = reinterpret_cast<OcrResultWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }
    const bool isSearch = window == self->search_;
    const WNDPROC original = isSearch ? self->searchTextProc_ : self->resultTextProc_;
    if (message == WM_KEYDOWN) {
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool handled = wParam == VK_ESCAPE || (wParam == 'F' && control) ||
                             wParam == VK_F3 || (wParam == 'C' && control && shift) ||
                             (isSearch && wParam == VK_RETURN);
        if (handled) {
            self->HandleEditKey(window, wParam);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return CallWindowProcW(original, window, message, wParam, lParam);
}

// ---- 消息 ----

LRESULT CALLBACK OcrResultWindow::WndProc(HWND window, UINT message, WPARAM wParam,
                                          LPARAM lParam) {
    auto* self = reinterpret_cast<OcrResultWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<OcrResultWindow*>(create->lpCreateParams);
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

LRESULT OcrResultWindow::Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;

    case WM_PRINTCLIENT:
        PaintContent(reinterpret_cast<HDC>(wParam));
        return 0;

    case WM_ERASEBKGND:
        return 1;   // Paint 自己铺底，避免闪一下默认底色

    case kOcrOutcomeMessage: {
        auto* outcome = reinterpret_cast<OcrOutcome*>(wParam);
        if (outcome != nullptr) {
            if (outcome->token == token_) {
                ApplyOutcome(*outcome);
            }
            delete outcome;
        }
        return 0;
    }

    case WM_TIMER:
        if (wParam == 1) {
            loadingPhase_ = (loadingPhase_ + 1) % 60;
            InvalidateRect(window_, &contentRect_, FALSE);
            return 0;
        }
        if (wParam == 2) {
            KillTimer(window, 2);
            ResetStatus();
            return 0;
        }
        break;

    case WM_KEYDOWN:
        HandleEditKey(window, wParam);
        return 0;

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            if (PointIn(titleRect_, point) && !PointIn(closeTop_, point)) {
                return HTCAPTION;
            }
        }
        return hit;
    }

    case WM_MOUSEMOVE: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const Hit hovered = HitTest(point);
        const int line = LineAt(point);
        if (hovered != hoveredHit_ || line != hoveredLine_) {
            hoveredHit_ = hovered;
            hoveredLine_ = line;
            InvalidateRect(window_, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, window, 0 };
        TrackMouseEvent(&track);
        SetCursor(LoadCursorW(nullptr, hovered != Hit::None || line >= 0 ? IDC_HAND
                                                                          : IDC_ARROW));
        return 0;
    }

    case WM_MOUSELEAVE:
        if (hoveredHit_ != Hit::None || hoveredLine_ >= 0) {
            hoveredHit_ = Hit::None;
            hoveredLine_ = -1;
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        const Hit hit = HitTest(point);
        if (hit != Hit::None) {
            HandleClick(hit);
            return 0;
        }
        const int line = LineAt(point);
        if (line >= 0) {
            CopyLine(static_cast<std::size_t>(line));
        }
        return 0;
    }

    case WM_MOUSEWHEEL: {
        if (lineView_ && stage_ == Stage::Success) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            if (PointIn(contentRect_, point)) {
                const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
                const int rows = (contentRect_.bottom - contentRect_.top) / ScaleInt(kRowDip) - 1;
                const int maxScroll = std::max(0, static_cast<int>(result_.lines.size()) - rows);
                lineScroll_ = std::clamp(lineScroll_ + (delta > 0 ? -3 : 3), 0, maxScroll);
                InvalidateRect(window_, &contentRect_, FALSE);
                return 0;
            }
        }
        break;
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND edit = reinterpret_cast<HWND>(lParam);
        SetTextColor(dc, kTextPrimary);
        if (edit == text_) {
            SetBkColor(dc, kCardBg);
            return reinterpret_cast<LRESULT>(resultBrush_);
        }
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(whiteBrush_);
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 2 && HIWORD(wParam) == EN_CHANGE) {
            UpdateSearch(true);
            return 0;
        }
        return 0;

    case WM_CLOSE:
        Close();
        return 0;

    case WM_DESTROY:
        if (window_ == window) {
            window_ = nullptr;
        }
        text_ = nullptr;
        search_ = nullptr;
        searchTextProc_ = nullptr;
        resultTextProc_ = nullptr;
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// ---- 绘制 ----

void OcrResultWindow::DrawButton(HDC dc, const RECT& rect, const std::wstring& text,
                                 bool primary, bool active, bool hover) {
    COLORREF fill = RGB(255, 255, 255);
    COLORREF border = kButtonBorder;
    COLORREF ink = kTextPrimary;
    if (primary) {
        fill = kAccent;
        border = kAccent;
        ink = RGB(255, 255, 255);
    } else if (active) {
        fill = kChipBg;
        border = kChipBorder;
        ink = kChipText;
    } else if (hover) {
        fill = RGB(247, 247, 250);
        border = RGB(201, 201, 207);
    }
    FillRounded(dc, rect, ScaleInt(8.0f), fill, border, 1);
    DrawTextIn(dc, text, fontButton_, ink, rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void OcrResultWindow::DrawSmallButton(HDC dc, const RECT& rect, const std::wstring& text,
                                      bool hover) {
    FillRounded(dc, rect, ScaleInt(8.0f), hover ? RGB(247, 247, 250) : RGB(255, 255, 255),
                RGB(0xE1, 0xE1, 0xE6), 1);
    DrawTextIn(dc, text, fontSmall_, kTextPrimary, rect,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void OcrResultWindow::DrawSegment(HDC dc, const RECT& rect, const std::wstring& text,
                                  bool active, bool hover) {
    if (active) {
        FillRounded(dc, rect, ScaleInt(6.0f), RGB(255, 255, 255), kSegmentActiveBorder, 1);
    } else if (hover) {
        FillRounded(dc, rect, ScaleInt(6.0f), RGB(235, 235, 239), RGB(235, 235, 239), 0);
    } else {
        FillRounded(dc, rect, ScaleInt(6.0f), kSegmentBg, kSegmentBg, 0);
    }
    DrawTextIn(dc, text, active ? fontButton_ : fontSmall_,
               active ? kTextPrimary : kTextSecondary, rect,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void OcrResultWindow::DrawPreview(HDC dc) {
    FillRounded(dc, previewCard_, ScaleInt(10.0f), kCardBg, kCardBorder, 1);
    if (image_.empty() || previewImage_.right <= previewImage_.left) {
        return;
    }
    const int availableWidth = previewImage_.right - previewImage_.left;
    const int availableHeight = previewImage_.bottom - previewImage_.top;
    if (availableWidth <= 0 || availableHeight <= 0) {
        return;
    }
    const float scale = std::min(static_cast<float>(availableWidth) / imageWidth_,
                                 static_cast<float>(availableHeight) / imageHeight_);
    const int drawWidth = std::max(1, static_cast<int>(imageWidth_ * scale));
    const int drawHeight = std::max(1, static_cast<int>(imageHeight_ * scale));
    const int offsetX = previewImage_.left + (availableWidth - drawWidth) / 2;
    const int offsetY = previewImage_.top + (availableHeight - drawHeight) / 2;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = imageWidth_;
    info.bmiHeader.biHeight = -imageHeight_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(dc, HALFTONE);
    StretchDIBits(dc, offsetX, offsetY, drawWidth, drawHeight, 0, 0, imageWidth_, imageHeight_,
                  image_.data(), &info, DIB_RGB_COLORS, SRCCOPY);

    if (stage_ == Stage::Success) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(255, 90, 90));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        for (const core::ocr::OcrLine& line : result_.lines) {
            const int left = offsetX + static_cast<int>(line.box.left * scale);
            const int top = offsetY + static_cast<int>(line.box.top * scale);
            const int right = offsetX + static_cast<int>(line.box.right * scale);
            const int bottom = offsetY + static_cast<int>(line.box.bottom * scale);
            Rectangle(dc, left, top, right, bottom);
        }
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
    }

    // 搜索命中：全部命中画绿色，当前命中画强调蓝。
    if (!matches_.empty()) {
        HPEN matchPen = CreatePen(PS_SOLID, 2, RGB(46, 204, 113));
        HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 255));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        for (std::size_t i = 0; i < matches_.size() && i < result_.lines.size(); ++i) {
            const std::size_t lineIndex = matches_[i];
            if (lineIndex >= result_.lines.size()) {
                continue;
            }
            const core::ocr::OcrLine& line = result_.lines[lineIndex];
            HGDIOBJ old = SelectObject(dc, i == matchIndex_ ? accentPen : matchPen);
            const int left = offsetX + static_cast<int>(line.box.left * scale);
            const int top = offsetY + static_cast<int>(line.box.top * scale);
            const int right = offsetX + static_cast<int>(line.box.right * scale);
            const int bottom = offsetY + static_cast<int>(line.box.bottom * scale);
            Rectangle(dc, left, top, right, bottom);
            SelectObject(dc, old);
        }
        SelectObject(dc, oldBrush);
        DeleteObject(matchPen);
        DeleteObject(accentPen);
    }
}

void OcrResultWindow::DrawLines(HDC dc) {
    const int rowHeight = ScaleInt(kRowDip);
    const int left = contentRect_.left + ScaleInt(4.0f);
    const int right = contentRect_.right - ScaleInt(4.0f);
    const int rows = (contentRect_.bottom - contentRect_.top - ScaleInt(12.0f)) / rowHeight;
    const int numberWidth = ScaleInt(26.0f);
    int y = contentRect_.top + ScaleInt(6.0f);
    for (int row = 0; row < rows; ++row) {
        const int index = lineScroll_ + row;
        if (index < 0 || index >= static_cast<int>(result_.lines.size())) {
            break;
        }
        RECT rowRect{ left, y, right, y + rowHeight };
        if (index == hoveredLine_) {
            FillRounded(dc, rowRect, ScaleInt(8.0f), RGB(255, 255, 255), RGB(231, 231, 236),
                        1);
        }
        RECT numberRect{ rowRect.left + ScaleInt(10.0f), rowRect.top,
                         rowRect.left + numberWidth, rowRect.bottom };
        DrawTextIn(dc, std::to_wstring(index + 1), fontSmall_, RGB(0xA8, 0xA8, 0xAD),
                   numberRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT textRect{ rowRect.left + numberWidth, rowRect.top, rowRect.right - ScaleInt(30.0f),
                       rowRect.bottom };
        DrawTextIn(dc, Utf8ToWide(result_.lines[index].text), fontBody_, kTextPrimary,
                   textRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        const int iconX = rowRect.right - ScaleInt(20.0f);
        const int iconY = rowRect.top + rowHeight / 2 - ScaleInt(6.0f);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xB0, 0xB0, 0xB5));
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, iconX, iconY + ScaleInt(3.0f), iconX + ScaleInt(8.0f),
                  iconY + ScaleInt(12.0f));
        Rectangle(dc, iconX + ScaleInt(3.0f), iconY, iconX + ScaleInt(11.0f),
                  iconY + ScaleInt(9.0f));
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);

        y += rowHeight;
    }
    if (static_cast<int>(result_.lines.size()) > kMaxLines) {
        RECT hint{ left, y, right, y + rowHeight };
        DrawTextIn(dc, L"还有 " + std::to_wstring(result_.lines.size() - kMaxLines) +
                             L" 行，切换回「文本」视图查看和复制。",
                   fontSmall_, kTextTertiary, hint, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
}

void OcrResultWindow::DrawLoading(HDC dc) {
    FillRounded(dc, contentRect_, ScaleInt(10.0f), kCardBg, kCardBorder, 1);
    const int centerY = (contentRect_.top + contentRect_.bottom) / 2;
    RECT titleRect{ contentRect_.left, centerY - ScaleInt(26.0f), contentRect_.right,
                    centerY - ScaleInt(4.0f) };
    DrawTextIn(dc, L"正在识别文字…", fontButton_, kTextSecondary, titleRect,
               DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    const std::wstring hint = engine_ == core::OcrEngine::Model
                                  ? L"PP-OCRv6 tiny · 首次使用会准备本地模型"
                                  : L"Windows 原生 · 系统离线引擎";
    RECT hintRect{ contentRect_.left, centerY, contentRect_.right,
                   centerY + ScaleInt(18.0f) };
    DrawTextIn(dc, hint, fontSmall_, kTextTertiary, hintRect,
               DT_CENTER | DT_TOP | DT_SINGLELINE);

    const int trackWidth = ScaleInt(180.0f);
    const int trackHeight = ScaleInt(3.0f);
    RECT track{ (contentRect_.left + contentRect_.right) / 2 - trackWidth / 2,
                centerY - ScaleInt(40.0f),
                (contentRect_.left + contentRect_.right) / 2 + trackWidth / 2,
                centerY - ScaleInt(40.0f) + trackHeight };
    FillRounded(dc, track, trackHeight, RGB(0xE8, 0xE8, 0xED), RGB(0xE8, 0xE8, 0xED), 0);
    const int segment = ScaleInt(60.0f);
    const int travel = trackWidth - segment;
    const int offset = travel * loadingPhase_ / 59;
    RECT bar{ track.left + offset, track.top, track.left + offset + segment, track.bottom };
    FillRounded(dc, bar, trackHeight, kAccent, kAccent, 0);
}

void OcrResultWindow::DrawEmpty(HDC dc) {
    const bool warning = stage_ == Stage::Failed;
    const int iconSize = ScaleInt(46.0f);
    RECT icon{ (contentRect_.left + contentRect_.right) / 2 - iconSize / 2,
               contentRect_.top + ScaleInt(14.0f),
               (contentRect_.left + contentRect_.right) / 2 + iconSize / 2,
               contentRect_.top + ScaleInt(14.0f) + iconSize };
    FillRounded(dc, icon, ScaleInt(14.0f), warning ? kWarningBg : kChipBg,
                warning ? kWarningBorder : kChipBorder, 1);
    DrawTextIn(dc, warning ? L"!" : L"文", fontTitle_, warning ? kWarningText : kChipText,
               icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    std::wstring text;
    if (stage_ == Stage::Empty) {
        text = engine_ == core::OcrEngine::Model
                   ? L"没有识别到文字。可以切回「原生」再试一次，或选择更清晰、对比度更高的区域。"
                   : L"没有识别到文字。可以切换到「模型」再试一次，或选择更清晰、对比度更高的区域。";
    } else {
        text = L"这次没有识别成功。";
        if (!warning_.empty()) {
            text += L"\n\n" + warning_;
        }
    }
    RECT textRect{ contentRect_.left + ScaleInt(24.0f), icon.bottom + ScaleInt(12.0f),
                   contentRect_.right - ScaleInt(24.0f), emptyRetry_.top - ScaleInt(8.0f) };
    DrawTextIn(dc, text, fontSmall_, kTextSecondary, textRect,
               DT_CENTER | DT_TOP | DT_WORDBREAK);
    DrawSmallButton(dc, emptyRetry_,
                    engine_ == core::OcrEngine::Native ? L"用模型识别重试"
                                                       : L"用原生识别重试",
                    hoveredHit_ == Hit::EmptyRetry);
}

void OcrResultWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);
    PaintContent(dc);
    EndPaint(window_, &paint);
}

void OcrResultWindow::PaintContent(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, whiteBrush_);
    SetBkMode(dc, TRANSPARENT);

    const int pad = ScaleInt(kPadDip);

    // 标题栏。
    RECT icon{ pad, ScaleInt(13.0f), pad + ScaleInt(32.0f), ScaleInt(45.0f) };
    FillRounded(dc, icon, ScaleInt(9.0f), RGB(0xEA, 0xF3, 0xFF), RGB(0xD4, 0xE7, 0xFF), 1);
    DrawTextIn(dc, L"文", fontIcon_, kAccent, icon, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT titleText{ icon.right + ScaleInt(11.0f), ScaleInt(11.0f), closeTop_.left,
                    ScaleInt(32.0f) };
    DrawTextIn(dc, L"文字识别结果", fontTitle_, kTextPrimary, titleText,
               DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    RECT subtitle{ titleText.left, ScaleInt(32.0f), closeTop_.left, ScaleInt(50.0f) };
    DrawTextIn(dc,
               options_.fromPin ? L"贴图原图 · 按行识别 · 不包含标注"
                                : L"截图选区 · 按行识别",
               fontSubtitle_, kTextTertiary, subtitle, DT_LEFT | DT_TOP | DT_SINGLELINE);

    FillRounded(dc, closeTop_, ScaleInt(8.0f),
                hoveredHit_ == Hit::CloseTop ? RGB(0xF0, 0xF0, 0xF3) : RGB(255, 255, 255),
                RGB(255, 255, 255), 0);
    {
        const int cx = (closeTop_.left + closeTop_.right) / 2;
        const int cy = (closeTop_.top + closeTop_.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, std::max(1, ScaleInt(1.0f)), kTextTertiary);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, cx - ScaleInt(5.0f), cy - ScaleInt(5.0f), nullptr);
        LineTo(dc, cx + ScaleInt(5.0f), cy + ScaleInt(5.0f));
        MoveToEx(dc, cx + ScaleInt(5.0f), cy - ScaleInt(5.0f), nullptr);
        LineTo(dc, cx - ScaleInt(5.0f), cy + ScaleInt(5.0f));
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    // 引擎标记 + 计数。
    if (stage_ != Stage::Loading) {
        const std::wstring chip =
            stage_ == Stage::Failed
                ? L"识别不可用"
                : (engine_ == core::OcrEngine::Model ? L"PP-OCRv6 tiny" : L"Windows 原生");
        const bool warning = stage_ == Stage::Failed;
        FillRounded(dc, chipRect_, ScaleInt(6.0f), warning ? kWarningBg : kChipBg,
                    warning ? kWarningBorder : kChipBorder, 1);
        DrawTextIn(dc, chip, fontSmall_, warning ? kWarningText : kChipText, chipRect_,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    std::wstring count;
    if (stage_ == Stage::Loading) {
        count = L"识别中…";
    } else if (stage_ == Stage::Success) {
        std::size_t characters = 0;
        for (wchar_t c : plainText_) {
            if (!iswspace(static_cast<wint_t>(c))) {
                ++characters;
            }
        }
        count = std::to_wstring(result_.lines.size()) + L" 行 · " +
                std::to_wstring(characters) + L" 字 · " +
                std::to_wstring(static_cast<int>(elapsedMs_ + 0.5)) + L" ms";
    } else if (stage_ == Stage::Empty) {
        count = L"未识别到文字";
    } else {
        count = L"识别失败";
    }
    RECT countRect{ width_ / 2, chipRect_.top, width_ - pad, chipRect_.bottom };
    DrawTextIn(dc, count, fontSmall_, kTextTertiary, countRect,
               DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    // 引擎分段控件。
    FillRounded(dc, engineGroup_, ScaleInt(8.0f), kSegmentBg, kSegmentBorder, 1);
    DrawSegment(dc, engineNative_, L"原生识别", engine_ == core::OcrEngine::Native,
                hoveredHit_ == Hit::EngineNative);
    DrawSegment(dc, engineModel_, L"模型识别", engine_ == core::OcrEngine::Model,
                hoveredHit_ == Hit::EngineModel);

    // 搜索 / 逐行。
    const bool success = stage_ == Stage::Success;
    DrawSmallButton(dc, searchButton_, searchOpen_ ? L"收起" : L"搜索",
                    success && hoveredHit_ == Hit::SearchButton);
    DrawSmallButton(dc, viewButton_, lineView_ ? L"文本" : L"逐行",
                    success && hoveredHit_ == Hit::ViewButton);
    if (!success) {
        FillRounded(dc, searchButton_, ScaleInt(8.0f), RGB(255, 255, 255), kButtonBorder, 1);
        DrawTextIn(dc, searchOpen_ ? L"收起" : L"搜索", fontSmall_, kTextTertiary,
                   searchButton_, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        FillRounded(dc, viewButton_, ScaleInt(8.0f), RGB(255, 255, 255), kButtonBorder, 1);
        DrawTextIn(dc, lineView_ ? L"文本" : L"逐行", fontSmall_, kTextTertiary, viewButton_,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // 搜索栏。
    if (searchOpen_) {
        FillRounded(dc, searchBar_, ScaleInt(9.0f), kCardBg, kCardBorder, 1);
        FillRounded(dc, searchEditRect_, ScaleInt(7.0f), RGB(255, 255, 255),
                    RGB(0xDC, 0xDC, 0xE1), 1);
        RECT statusRect{ searchPrev_.left - ScaleInt(68.0f), searchBar_.top,
                         searchPrev_.left - ScaleInt(8.0f), searchBar_.bottom };
        DrawTextIn(dc, searchLabel_, fontSmall_, kTextTertiary, statusRect,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawSmallButton(dc, searchPrev_, L"上一处", hoveredHit_ == Hit::SearchPrevious);
        DrawSmallButton(dc, searchNext_, L"下一处", hoveredHit_ == Hit::SearchNext);
        FillRounded(dc, searchClose_, ScaleInt(8.0f),
                    hoveredHit_ == Hit::SearchClose ? RGB(0xF0, 0xF0, 0xF3) : RGB(255, 255, 255),
                    RGB(255, 255, 255), 0);
        const int cx = (searchClose_.left + searchClose_.right) / 2;
        const int cy = (searchClose_.top + searchClose_.bottom) / 2;
        HPEN pen = CreatePen(PS_SOLID, 1, kTextTertiary);
        HGDIOBJ old = SelectObject(dc, pen);
        MoveToEx(dc, cx - ScaleInt(4.0f), cy - ScaleInt(4.0f), nullptr);
        LineTo(dc, cx + ScaleInt(4.0f), cy + ScaleInt(4.0f));
        MoveToEx(dc, cx + ScaleInt(4.0f), cy - ScaleInt(4.0f), nullptr);
        LineTo(dc, cx - ScaleInt(4.0f), cy + ScaleInt(4.0f));
        SelectObject(dc, old);
        DeleteObject(pen);
    }

    // 警告。
    if (!warning_.empty() && warningRect_.bottom > warningRect_.top) {
        FillRounded(dc, warningRect_, ScaleInt(9.0f), kWarningBg, kWarningBorder, 1);
        RECT text = Inset(warningRect_, ScaleInt(12.0f), ScaleInt(10.0f));
        DrawTextIn(dc, warning_, fontSmall_, kWarningText, text,
                   DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    // 内容区。
    if (stage_ == Stage::Loading) {
        DrawLoading(dc);
    } else if (stage_ == Stage::Success) {
        FillRounded(dc, contentRect_, ScaleInt(10.0f), kCardBg, kCardBorder, 1);
        if (lineView_) {
            DrawLines(dc);
        }
    } else {
        DrawEmpty(dc);
    }

    // 原图（识别框 + 搜索高亮）。
    if (showImage_ && previewCard_.bottom > previewCard_.top) {
        DrawPreview(dc);
    }

    // 页脚。
    HPEN hairline = CreatePen(PS_SOLID, 1, kHairline);
    HGDIOBJ oldPen = SelectObject(dc, hairline);
    MoveToEx(dc, 0, footerRect_.top, nullptr);
    LineTo(dc, width_, footerRect_.top);
    SelectObject(dc, oldPen);
    DeleteObject(hairline);
    RECT statusRect{ pad, footerRect_.top, retryButton_.left - ScaleInt(12.0f),
                     footerRect_.bottom };
    DrawTextIn(dc, status_, fontSmall_, statusError_ ? kDanger : kTextTertiary, statusRect,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    DrawButton(dc, retryButton_, L"重试", false, false, hoveredHit_ == Hit::Retry);
    DrawButton(dc, imageButton_, L"原图", false, showImage_, hoveredHit_ == Hit::ImageToggle);
    DrawButton(dc, closeFooter_, L"关闭", false, false, hoveredHit_ == Hit::CloseFooter);
    const bool canCopy = stage_ == Stage::Success && !plainText_.empty();
    if (canCopy) {
        DrawButton(dc, copyAllButton_, L"复制全部", true, false, false);
    } else {
        FillRounded(dc, copyAllButton_, ScaleInt(8.0f), RGB(0xEB, 0xF4, 0xFF),
                    RGB(0xEB, 0xF4, 0xFF), 0);
        DrawTextIn(dc, L"复制全部", fontButton_, RGB(0xA9, 0xC8, 0xF5), copyAllButton_,
                   DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

}
}
