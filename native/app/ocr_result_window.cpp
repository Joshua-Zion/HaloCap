#include "ocr_result_window.h"

#include <algorithm>
#include <cwchar>
#include <string>

namespace halocap::app {

namespace {

constexpr wchar_t kClassName[] = L"HaloCapOcrResult";
constexpr int kPreviewMargin = 12;
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

std::wstring Lower(std::wstring text) {
    for (wchar_t& character : text) {
        if (character >= L'A' && character <= L'Z') {
            character = static_cast<wchar_t>(character - L'A' + L'a');
        }
    }
    return text;
}

}  // namespace

OcrResultWindow::OcrResultWindow() = default;

OcrResultWindow::~OcrResultWindow() { Close(); }

bool OcrResultWindow::EnsureClass() {
    static const bool registered = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
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

    window_ = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_TOPMOST, kClassName, title.c_str(),
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 680,
                              static_cast<HWND>(owner), nullptr, GetModuleHandleW(nullptr), this);
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
    hint_ = CreateWindowExW(0, L"STATIC", L"左侧为识别区域，框出的每一段即为右侧的一行",
                            WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, window_, nullptr,
                            GetModuleHandleW(nullptr), nullptr);

    for (HWND control : { text_, search_, findButton_, copyButton_, hint_ }) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }

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
        if (LOWORD(wParam) == 3) {
            FindNext();
        } else if (LOWORD(wParam) == 4) {
            CopyAll();
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

    const int previewWidth = width * 58 / 100;
    preview_ = RECT{ kPreviewMargin, kPreviewMargin, previewWidth - kPreviewMargin,
                     height - kPreviewMargin };

    const int right = previewWidth;
    const int rightWidth = width - right - kPreviewMargin;
    const int top = kPreviewMargin;

    SetWindowPos(hint_, nullptr, right, top, rightWidth, 24, SWP_NOZORDER);
    SetWindowPos(copyButton_, nullptr, right, top + 30, 96, 28, SWP_NOZORDER);
    SetWindowPos(findButton_, nullptr, right + 100, top + 30, 64, 28, SWP_NOZORDER);
    SetWindowPos(search_, nullptr, right + 100 + 68, top + 32, rightWidth - 168, 24,
                 SWP_NOZORDER);
    SetWindowPos(text_, nullptr, right, top + 68, rightWidth, height - top - 68 - kPreviewMargin,
                 SWP_NOZORDER);
}

void OcrResultWindow::Paint() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window_, &paint);

    RECT client{};
    GetClientRect(window_, &client);
    FillRect(dc, &client, background_);

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

void OcrResultWindow::FindNext() {
    if (search_ == nullptr || text_ == nullptr) {
        return;
    }

    wchar_t buffer[256] = {};
    GetWindowTextW(search_, buffer, 256);
    const std::wstring query = Lower(buffer);
    if (query.empty() || wideText_.empty()) {
        return;
    }

    const std::wstring haystack = Lower(wideText_);
    std::size_t position = haystack.find(query, searchStart_);
    if (position == std::wstring::npos) {
        position = haystack.find(query, 0);
    }
    if (position == std::wstring::npos) {
        return;
    }

    searchStart_ = position + query.size();
    SendMessageW(text_, EM_SETSEL, position, position + query.size());
    SendMessageW(text_, EM_SCROLLCARET, 0, 0);
    SetFocus(text_);
}

}
