#pragma once

#include "ocr.h"
#include "settings.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <windows.h>

namespace halocap::app {

// 一次识别的结果（worker 线程 → 结果窗口）。
struct OcrOutcome {
    int token = 0;
    core::OcrEngine engine = core::OcrEngine::Native;
    core::ocr::OcrResult result;
    double elapsedMs = 0.0;
    std::string error;   // 非空 = 识别失败
};

// 识别完成消息：wParam = OcrOutcome*，窗口接管所有权并 delete。
inline constexpr UINT kOcrOutcomeMessage = WM_APP + 40;

// app 提供的异步识别入口：拿到图像与目标窗口，完成后 PostMessage(kOcrOutcomeMessage)。
// 图像指针只在调用期间有效；实现方要自己拷贝（识别跑在后台线程）。
using OcrRunner = std::function<void(core::OcrEngine engine, int token,
                                     const std::uint8_t* bgra, int width, int height,
                                     HWND notify)>;

struct OcrResultOptions {
    core::OcrEngine engine = core::OcrEngine::Native;
    bool autoCopy = false;
    bool fromPin = false;
    bool showImage = true;
    bool skipRecognition = false;   // 自检用：直接喂结果，不走识别
    core::IntRect anchor{};         // 选区/贴图在屏幕上的矩形（窗口定位用）
    std::string fontFamily = "Microsoft YaHei UI";
};

// OCR 结果窗口，交互对齐 1.x 的 OcrResultWindow：
// 打开即显示「正在识别」，识别在 worker 里跑；可切换原生/模型、重试、逐行复制、
// 搜索（Ctrl+F / F3）、原图开关（带识别框与搜索高亮）、自动复制、Esc 关闭。
// 整窗自绘（只有文本与搜索框是真 EDIT），因此圆角卡片、分段控件、行视图都是 1.x 的样子。
class OcrResultWindow {
public:
    OcrResultWindow();
    ~OcrResultWindow();

    OcrResultWindow(const OcrResultWindow&) = delete;
    OcrResultWindow& operator=(const OcrResultWindow&) = delete;

    bool Create(void* owner, const std::uint8_t* bgra, int width, int height,
                const OcrResultOptions& options, OcrRunner runner);

    void Close();
    bool Valid() const { return window_ != nullptr; }
    HWND Handle() const { return window_; }

    // 自检用：直接喂一份结果（跳过识别），或读回识别状态。
    void PresentResultForTest(const core::ocr::OcrResult& result, double elapsedMs);
    bool ReadyForTest() const { return stage_ != Stage::Loading; }
    int LineCountForTest() const { return static_cast<int>(result_.lines.size()); }
    double ElapsedMsForTest() const { return elapsedMs_; }
    core::IntRect PreviewRectForTest() const;
    void SetImageVisibleForTest(bool visible);
    void SetLineViewForTest(bool lineView);
    bool LineViewForTest() const { return lineView_; }

    // 搜索自检（原有签名保持不变）。
    void SetQueryForTest(const std::wstring& query);
    const std::vector<std::size_t>& MatchesForTest() const { return matches_; }
    std::size_t MatchIndexForTest() const { return matchIndex_; }
    std::wstring StatusForTest() const;
    void StepMatchForTest(int direction) { StepMatch(direction); }

private:
    enum class Stage { Loading, Success, Empty, Failed };

    enum class Hit {
        None = 0,
        CloseTop,
        EngineNative,
        EngineModel,
        SearchButton,
        ViewButton,
        SearchPrevious,
        SearchNext,
        SearchClose,
        Retry,
        ImageToggle,
        CloseFooter,
        CopyAll,
        EmptyRetry,
    };

    static bool EnsureClass();
    static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK EditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT Handle(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    float Scale(float dip) const { return dip * uiScale_; }
    int ScaleInt(float dip) const { return static_cast<int>(dip * uiScale_ + 0.5f); }

    void BuildFonts();
    void DeleteResources();
    void Layout();
    void Paint();
    void PaintContent(HDC dc);

    void StartRecognition();
    void ApplyOutcome(const OcrOutcome& outcome);
    void ShowStatus(const std::wstring& message, bool error = false);
    void ResetStatus();

    void CopyText(const std::wstring& text, const std::wstring& message);
    void CopyAll();
    void CopyLine(std::size_t index);

    void SetLineView(bool lineView);
    void SetImageVisible(bool visible);
    void OpenSearch();
    void CloseSearch(bool focusWindow);
    void UpdateSearch(bool selectFirst);
    void StepMatch(int direction);
    void SelectMatch(std::size_t index);
    void RefreshSearchStatus();

    std::wstring CurrentQuery() const;
    Hit HitTest(POINT client) const;
    int LineAt(POINT client) const;
    void HandleClick(Hit hit);
    void HandleEditKey(HWND edit, WPARAM key);

    void DrawButton(HDC dc, const RECT& rect, const std::wstring& text, bool primary,
                    bool active, bool hover);
    void DrawSmallButton(HDC dc, const RECT& rect, const std::wstring& text, bool hover);
    void DrawSegment(HDC dc, const RECT& rect, const std::wstring& text, bool active,
                     bool hover);
    void DrawPreview(HDC dc);
    void DrawLines(HDC dc);
    void DrawLoading(HDC dc);
    void DrawEmpty(HDC dc);

    HWND window_ = nullptr;
    HWND text_ = nullptr;
    HWND search_ = nullptr;
    WNDPROC searchTextProc_ = nullptr;
    WNDPROC resultTextProc_ = nullptr;

    OcrResultOptions options_;
    OcrRunner runner_;
    core::OcrEngine engine_ = core::OcrEngine::Native;
    Stage stage_ = Stage::Loading;
    core::ocr::OcrResult result_;
    std::wstring plainText_;
    double elapsedMs_ = 0.0;
    std::wstring warning_;
    int token_ = 0;

    bool lineView_ = false;
    bool showImage_ = true;
    bool searchOpen_ = false;
    // 逐行视图按「换行后的实际高度」排：长行不会被省略号截断。
    int lineScrollY_ = 0;
    std::vector<int> lineHeights_;
    int lineTotalHeight_ = 0;
    int hoveredLine_ = -1;
    Hit hoveredHit_ = Hit::None;

    // 搜索
    std::wstring lastQuery_;
    std::vector<std::size_t> matches_;
    std::size_t matchIndex_ = 0;
    std::wstring searchLabel_;

    // 状态栏
    std::wstring status_;
    bool statusError_ = false;

    // 加载动画
    int loadingPhase_ = 0;

    // 布局（客户区像素）
    int width_ = 0;
    int height_ = 0;
    RECT titleRect_{};
    RECT closeTop_{};
    RECT chipRect_{};
    RECT engineGroup_{};
    RECT engineNative_{};
    RECT engineModel_{};
    RECT searchButton_{};
    RECT viewButton_{};
    RECT searchBar_{};
    RECT searchEditRect_{};
    RECT searchPrev_{};
    RECT searchNext_{};
    RECT searchClose_{};
    RECT warningRect_{};
    RECT contentRect_{};
    RECT previewCard_{};
    RECT previewImage_{};
    RECT footerRect_{};
    RECT retryButton_{};
    RECT imageButton_{};
    RECT closeFooter_{};
    RECT copyAllButton_{};
    RECT emptyRetry_{};

    std::wstring fontFamilyW_;

    HFONT fontTitle_ = nullptr;
    HFONT fontSubtitle_ = nullptr;
    HFONT fontBody_ = nullptr;
    HFONT fontSmall_ = nullptr;
    HFONT fontButton_ = nullptr;
    HFONT fontIcon_ = nullptr;
    HFONT fontEdit_ = nullptr;
    HFONT fontSearch_ = nullptr;

    HBRUSH whiteBrush_ = nullptr;
    HBRUSH resultBrush_ = nullptr;
    HPEN borderPen_ = nullptr;

    float uiScale_ = 1.0f;
    std::vector<std::uint8_t> image_;
    int imageWidth_ = 0;
    int imageHeight_ = 0;
};

}
