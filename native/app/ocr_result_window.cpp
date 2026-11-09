#include "ocr_result_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cwchar>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapOcrResult";
constexpr int kPreviewMargin = 12;
constexpr int kTitleBar = 40;
constexpr int kTopBar = 44;

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
                             const core::ocr::OcrResult& result) {
    if (bgra == nullptr || width <= 0 || height <= 0 || !EnsureClass()) {
        return false;
    }

    image_.assign(bgra, bgra + static_cast<std::size_t>(width) * height * 4);
    imageWidth_ = width;
    imageHeight_ = height;
    result_ = result;
    wideText_ = Utf8ToWide(result.plainText);

    const std::wstring title =
        L"HaloCap OCR — " + std::to_wstring(result.lines.size()) + L" 行";

    window_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, kClassName, title.c_str(), WS_POPUP,
                              0, 0, 1080, 700, static_cast<HWND>(owner), nullptr,
                              GetModuleHandleW(nullptr), this);
    if (window_ == nullptr) {
        return false;
    }

    // 1.x 结果窗口：浅色内容底、白色卡片、#007AFF 主按钮。
    background_ = CreateSolidBrush(RGB(247, 247, 249));
    cardBrush_ = CreateSolidBrush(RGB(255, 255, 255));
    accentBrush_ = CreateSolidBrush(RGB(0, 122, 255));
    borderPen_ = CreatePen(PS_SOLID, 1, RGB(231, 231, 236));
    font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH,
                        L"Microsoft YaHei UI");

    text_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wideText_.c_str(),
                            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
                                ES_AUTOVSCROLL,
                            0, 0, 10, 10, window_, reinterpret_cast<HMENU>(1),
                            GetModuleHandleW(nullptr), nullptr);
    search_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                              WS_CHILD | WS_VISIBLE | ES_LEFT | ES_AUTOHSCROLL, 0, 0, 10, 10,
                              window_, reinterpret_cast<HMENU>(2), GetModuleHandleW(nullptr),
                              nullptr);
    findButton_ = CreateWindowExW(0, L"BUTTON", L"查找",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, window_,
                                  reinterpret_cast<HMENU>(3), GetModuleHandleW(nullptr), nullptr);
    copyButton_ = CreateWindowExW(0, L"BUTTON", L"复制全部",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, window_,
                                  reinterpret_cast<HMENU>(4), GetModuleHandleW(nullptr), nullptr);
    viewButton_ = CreateWindowExW(0, L"BUTTON", L"只看文本",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, window_,
                                  reinterpret_cast<HMENU>(7), GetModuleHandleW(nullptr), nullptr);
    previousButton_ = CreateWindowExW(0, L"BUTTON", L"上一处",
                                      WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10,
                                      window_, reinterpret_cast<HMENU>(5),
                                      GetModuleHandleW(nullptr), nullptr);
    nextButton_ = CreateWindowExW(0, L"BUTTON", L"下一处",
                                  WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, window_,
                                  reinterpret_cast<HMENU>(6), GetModuleHandleW(nullptr), nullptr);
    hint_ = CreateWindowExW(0, L"STATIC", L"左侧为识别区域，框出的每一段即为右侧的一行",
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, window_, nullptr,
                            GetModuleHandleW(nullptr), nullptr);
    closeButton_ = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0,
                                   10, 10, window_, reinterpret_cast<HMENU>(8),
                                   GetModuleHandleW(nullptr), nullptr);

    for (HWND control : { text_, search_, findButton_, copyButton_, previousButton_, nextButton_, viewButton_, hint_, closeButton_ }) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

    // 两个 EDIT 都要子类化：F3/Shift+F3 在结果框里也要能跳，回车/Esc 只在搜索框里生效。
    // 原窗口过程存在成员里，EditProc 靠 hwnd 判断该转发给哪一个。
    SetWindowLongPtrW(search_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetWindowLongPtrW(text_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    searchTextProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(search_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditProc)));
    resultTextProc_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(text_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&EditProc)));
    SendMessageW(search_, EM_SETCUEBANNER, 0, reinterpret_cast<LPARAM>(L"搜索识别结果"));

    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(window_, HWND_TOPMOST, (screenWidth - 1080) / 2, (screenHeight - 700) / 2, 1080,
                 700, SWP_NOZORDER);
    const DWORD cornerPreference = 2;
    DwmSetWindowAttribute(window_, 33, &cornerPreference, sizeof(cornerPreference));

    Layout();
    ShowWindow(window_, SW_SHOW);
    UpdateWindow(window_);
    return true;
}

void OcrResultWindow::Close() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        DeleteObject(font_);
        font_ = nullptr;
    }
    for (HBRUSH* brush : { &background_, &cardBrush_, &accentBrush_ }) {
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
    case WM_SIZE:
        Layout();
        return 0;

    case WM_NCHITTEST: {
        const LRESULT hit = DefWindowProcW(window, message, wParam, lParam);
        if (hit == HTCLIENT) {
            POINT point{ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            RECT client{};
            GetClientRect(window, &client);
            if (point.y < kTitleBar && point.x < client.right - 56) {
                return HTCAPTION;
            }
        }
        return hit;
    }

    case WM_PAINT:
        Paint();
        return 0;

    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(window, &rect);
        FillRect(reinterpret_cast<HDC>(wParam), &rect, background_);
        return 1;
    }

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(247, 247, 249));
        return reinterpret_cast<LRESULT>(background_);
    }

    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, RGB(29, 29, 31));
        SetBkColor(dc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(cardBrush_);
    }

    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (item == nullptr) {
            return 0;
        }
        HDC dc = item->hDC;
        SetBkMode(dc, TRANSPARENT);
        if (item->CtlID == 8) {
            const RECT& r = item->rcItem;
            const int centerX = (r.left + r.right) / 2;
            const int centerY = (r.top + r.bottom) / 2;
            HGDIOBJ oldPen = SelectObject(dc, CreatePen(PS_SOLID, 1, RGB(90, 90, 96)));
            MoveToEx(dc, centerX - 5, centerY - 5, nullptr);
            LineTo(dc, centerX + 5, centerY + 5);
            MoveToEx(dc, centerX + 5, centerY - 5, nullptr);
            LineTo(dc, centerX - 5, centerY + 5);
            DeleteObject(SelectObject(dc, oldPen));
            return 1;
        }
        const bool primary = item->CtlID == 4;
        HGDIOBJ oldPen = SelectObject(dc, primary ? GetStockObject(NULL_PEN) : borderPen_);
        HGDIOBJ oldBrush = SelectObject(dc, primary ? accentBrush_ : cardBrush_);
        const RECT& r = item->rcItem;
        RoundRect(dc, r.left, r.top, r.right, r.bottom, 16, 16);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        SetTextColor(dc, primary ? RGB(255, 255, 255) : RGB(29, 29, 31));
        wchar_t text[32] = {};
        const LRESULT length = GetWindowTextW(item->hwndItem, text, 32);
        DrawTextW(dc, text, static_cast<int>(length), const_cast<RECT*>(&r),
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        return 1;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 2 && HIWORD(wParam) == EN_CHANGE) {
            UpdateSearch(true);
            return 0;
        }
        if (LOWORD(wParam) == 3) {
            FindNext();
        } else if (LOWORD(wParam) == 4) {
            CopyAll();
        } else if (LOWORD(wParam) == 5) {
            StepMatch(-1);
        } else if (LOWORD(wParam) == 6) {
            StepMatch(1);
        } else if (LOWORD(wParam) == 8) {
            DestroyWindow(window);
            return 0;
        } else if (LOWORD(wParam) == 7) {
            showImage_ = !showImage_;
            SetWindowTextW(viewButton_, showImage_ ? L"只看文本" : L"显示图片");
            Layout();
            InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;


    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        window_ = nullptr;
        text_ = nullptr;
        search_ = nullptr;
        findButton_ = nullptr;
        copyButton_ = nullptr;
        hint_ = nullptr;
        previousButton_ = nullptr;
        nextButton_ = nullptr;
        viewButton_ = nullptr;
        closeButton_ = nullptr;
        searchTextProc_ = nullptr;
        resultTextProc_ = nullptr;
        return 0;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

void OcrResultWindow::Layout() {
    if (window_ == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        return;
    }

    const int previewWidth = showImage_ ? width * 58 / 100 : 0;
    preview_ = showImage_
                   ? RECT{ kPreviewMargin, kPreviewMargin + kTitleBar,
                           previewWidth - kPreviewMargin, height - kPreviewMargin }
                   : RECT{};
    const int right = showImage_ ? previewWidth : kPreviewMargin;
    const int rightWidth = width - right - kPreviewMargin;
    const int top = kPreviewMargin + kTitleBar;

    SetWindowPos(hint_, nullptr, right, top, rightWidth, 24, SWP_NOZORDER);
    SetWindowPos(viewButton_, nullptr, right + rightWidth - 92, top + 30, 92, 28, SWP_NOZORDER);
    SetWindowPos(copyButton_, nullptr, right, top + 30, 92, 28, SWP_NOZORDER);
    SetWindowPos(findButton_, nullptr, right + 98, top + 30, 60, 28, SWP_NOZORDER);
    SetWindowPos(previousButton_, nullptr, right + 162, top + 30, 64, 28, SWP_NOZORDER);
    SetWindowPos(nextButton_, nullptr, right + 230, top + 30, 64, 28, SWP_NOZORDER);
    SetWindowPos(search_, nullptr, right + 300, top + 32, std::max(80, rightWidth - 300), 24,
                 SWP_NOZORDER);
    SetWindowPos(text_, nullptr, right, top + 68, rightWidth, height - top - 68 - kPreviewMargin,
                 SWP_NOZORDER);
    SetWindowPos(closeButton_, nullptr, width - 48, 7, 32, 26, SWP_NOZORDER);
}

void OcrResultWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);

    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, background_);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(29, 29, 31));
    HGDIOBJ oldFont = SelectObject(dc, font_);
    RECT titleRect{ 16, 9, client.right - 60, 9 + 24 };
    DrawTextW(dc, L"HaloCap OCR", -1, &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldFont);

    // 预览区是白色卡片（1.x 的 #FFFFFF 卡片 + #E7E7EC 边框）。
    if (preview_.right > preview_.left && preview_.bottom > preview_.top) {
        HGDIOBJ oldPen = SelectObject(dc, borderPen_);
        HGDIOBJ oldBrush = SelectObject(dc, cardBrush_);
        RoundRect(dc, preview_.left, preview_.top, preview_.right, preview_.bottom, 24, 24);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
    }

    if (image_.empty() || preview_.right <= preview_.left || preview_.bottom <= preview_.top) {
        EndPaint(window_, &paint);
        return;
    }

    const int availableWidth = preview_.right - preview_.left;
    const int availableHeight = preview_.bottom - preview_.top;
    const float scale = std::min(static_cast<float>(availableWidth) / imageWidth_,
                                 static_cast<float>(availableHeight) / imageHeight_);
    const int drawWidth = std::max(1, static_cast<int>(imageWidth_ * scale));
    const int drawHeight = std::max(1, static_cast<int>(imageHeight_ * scale));
    const int offsetX = preview_.left + (availableWidth - drawWidth) / 2;
    const int offsetY = preview_.top + (availableHeight - drawHeight) / 2;

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

    // 识别框：在原图上画红框，缩放后贴到预览区。
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

    // 搜索命中：全部命中画绿色，当前命中画强调蓝。
    if (!matches_.empty()) {
        HPEN matchPen = CreatePen(PS_SOLID, 2, RGB(46, 204, 113));
        HPEN accentPen = CreatePen(PS_SOLID, 2, RGB(0, 122, 255));
        for (std::size_t i = 0; i < matches_.size() && i < result_.lines.size(); ++i) {
            const std::size_t lineIndex = matches_[i];
            if (lineIndex >= result_.lines.size()) {
                continue;
            }
            const core::ocr::OcrLine& line = result_.lines[lineIndex];
            HGDIOBJ old = SelectObject(dc, i == matchIndex_ ? accentPen : matchPen);
            const int left = offsetX + static_cast<int>(line.box.left * scale);
            const int top2 = offsetY + static_cast<int>(line.box.top * scale);
            const int right = offsetX + static_cast<int>(line.box.right * scale);
            const int bottom = offsetY + static_cast<int>(line.box.bottom * scale);
            Rectangle(dc, left, top2, right, bottom);
            SelectObject(dc, old);
        }
        DeleteObject(matchPen);
        DeleteObject(accentPen);
    }

    EndPaint(window_, &paint);
}

void OcrResultWindow::CopyAll() {
    if (wideText_.empty()) {
        return;
    }
    const std::size_t bytes = (wideText_.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) {
        return;
    }
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return;
    }
    std::memcpy(target, wideText_.c_str(), bytes);
    GlobalUnlock(memory);

    if (!OpenClipboard(window_)) {
        GlobalFree(memory);
        return;
    }
    EmptyClipboard();
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

void OcrResultWindow::SelectMatch(std::size_t index) {
    if (matches_.empty() || index >= matches_.size() || text_ == nullptr) {
        return;
    }
    const std::size_t lineIndex = matches_[index];
    // UTF-8 → UTF-16：逐行累加换算，行间补一个换行。
    std::size_t charOffset = 0;
    for (std::size_t i = 0; i < lineIndex && i < result_.lines.size(); ++i) {
        const std::string& text = result_.lines[i].text;
        charOffset += static_cast<std::size_t>(MultiByteToWideChar(
                          CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0)) +
                      1;
    }
    const std::string& lineText = result_.lines[lineIndex].text;
    const std::size_t charLength = static_cast<std::size_t>(MultiByteToWideChar(
        CP_UTF8, 0, lineText.c_str(), static_cast<int>(lineText.size()), nullptr, 0));
    SendMessageW(text_, EM_SETSEL, charOffset, charOffset + charLength);
    SendMessageW(text_, EM_SCROLLCARET, 0, 0);
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::StepMatch(int direction) {
    if (matches_.empty()) {
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
    if (hint_ == nullptr) {
        return;
    }
    // 1.x 的状态文本就是 "3/17"；2.0 的命中单位是「行」（FindMatchingLines 每行只报一次），
    // 上一处/下一处走的也是行，所以计数与导航同源。
    std::wstring status;
    if (!lastQuery_.empty()) {
        status = matches_.empty()
                     ? L"无匹配"
                     : std::to_wstring(matchIndex_ + 1) + L"/" + std::to_wstring(matches_.size());
    }
    SetWindowTextW(hint_, status.empty() ? L"左侧为识别区域，框出的每一段即为右侧的一行"
                                         : status.c_str());
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
        // 高亮是画在父窗口上的：清了命中必须重绘，否则绿框/蓝框会留在图上。
        InvalidateRect(window_, nullptr, FALSE);
        return;
    }

    const int utf8Length =
        WideCharToMultiByte(CP_UTF8, 0, query.c_str(), static_cast<int>(query.size()), nullptr, 0,
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
    InvalidateRect(window_, nullptr, FALSE);
}

void OcrResultWindow::FindNext() {
    const bool queryChanged = CurrentQuery() != lastQuery_;
    UpdateSearch(true);
    // 查询没变时「查找」就是下一处；变了则停在第一处（与即时搜索一致）。
    if (!queryChanged && !matches_.empty()) {
        StepMatch(1);
    }
}

void OcrResultWindow::SetQueryForTest(const std::wstring& query) {
    if (search_ != nullptr) {
        SetWindowTextW(search_, query.c_str());
    }
}

std::wstring OcrResultWindow::StatusForTest() const {
    if (hint_ == nullptr) {
        return {};
    }
    const int length = GetWindowTextLengthW(hint_);
    std::wstring status(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(hint_, status.data(), length + 1);
    status.resize(static_cast<std::size_t>(length));
    return status;
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
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        if (wParam == VK_F3) {
            self->StepMatch(shift ? -1 : 1);
            return 0;
        }
        if (isSearch && wParam == VK_RETURN) {
            self->StepMatch(shift ? -1 : 1);
            return 0;
        }
        if (isSearch && wParam == VK_ESCAPE) {
            SetWindowTextW(window, L"");
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
    }
    return CallWindowProcW(original, window, message, wParam, lParam);
}

}
