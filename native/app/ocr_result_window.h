#pragma once

#include "ocr.h"

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace halocap::app {

// OCR 结果窗口：左侧原图 + 识别框，右侧按行文本 + 搜索 + 复制。
// 用 GDI 控件而不是再写一套 D2D 控件：结果窗口是低频界面，原生 EDIT 的
// 选择/滚动/输入法都是白拿的，与覆盖层的高频渲染不是一类东西。
class OcrResultWindow {
public:
    OcrResultWindow();
    ~OcrResultWindow();

    OcrResultWindow(const OcrResultWindow&) = delete;
    OcrResultWindow& operator=(const OcrResultWindow&) = delete;

    bool Create(void* owner, const std::uint8_t* bgra, int width, int height,
                const core::ocr::OcrResult& result);

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

private:
    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    void Layout();
    void Paint();
    void CopyAll();
    void FindNext();
    void StepMatch(int direction);
    void SelectMatch(std::size_t index);

    HWND window_ = nullptr;
    HWND text_ = nullptr;
    HWND search_ = nullptr;
    HWND findButton_ = nullptr;
    HWND copyButton_ = nullptr;
    HWND hint_ = nullptr;
    HWND previousButton_ = nullptr;
    HWND nextButton_ = nullptr;
    HWND viewButton_ = nullptr;
    bool showImage_ = true;
    HFONT font_ = nullptr;
    HBRUSH background_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH accentBrush_ = nullptr;
    HPEN borderPen_ = nullptr;

    std::vector<std::uint8_t> image_;
    int imageWidth_ = 0;
    int imageHeight_ = 0;

    core::ocr::OcrResult result_;
    std::wstring wideText_;
    std::size_t searchStart_ = 0;
    std::vector<std::size_t> matches_;
    std::size_t matchIndex_ = 0;
    std::wstring lastQuery_;

    RECT preview_{};
};

}
