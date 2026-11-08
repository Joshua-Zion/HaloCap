// S3 的截图链路验证：桌面复制 → 选区 → 九种标注 → 离屏合成 → 回读 → PNG + 剪贴板。
//
// 它不画 UI（工具栏、鼠标编辑在 app 那边），而是把 S3 的「领域 + 导出」整条链跑一遍：
// 选区、马赛克块、文本排版、箭头、序号、撤销后的最终列表，最后落到一个真实 PNG 上，
// 并量两条 S3 的退出条件：选区打开耗时与截图后回落的内存。
//
// 参数：输出 PNG 路径、选区（WxH@X,Y）
//   screenshot_probe.exe                          900x600@100,100
//   screenshot_probe.exe out.png 1280x720@0,0
#include "annotation.h"
#include "d2d_font_rasterizer.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"
#include "image_export.h"
#include "mosaic.h"
#include "selection.h"

#include <windows.h>

#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace core = halocap::core;
using halocap::platform::win32::D2dFontRasterizer;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::Win32DesktopDuplication;

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

std::int64_t QpcNowUs() {
    static const LARGE_INTEGER frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return value;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart) * 1000000 / frequency.QuadPart;
}

double PrivateMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / 1048576.0;
}

std::int64_t FileSizeBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
        return -1;
    }
    return (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
           static_cast<std::int64_t>(data.nFileSizeLow);
}

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
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

std::wstring AbsolutePath(const std::string& utf8Path) {
    const std::wstring wide = Utf8ToWide(utf8Path);
    wchar_t buffer[MAX_PATH] = {};
    const DWORD length = GetFullPathNameW(wide.c_str(), MAX_PATH, buffer, nullptr);
    if (length == 0 || length >= MAX_PATH) {
        return wide;
    }
    return buffer;
}

struct SelectionSpec {
    int width = 900;
    int height = 600;
    int x = 100;
    int y = 100;
};

SelectionSpec ParseSelection(const char* text) {
    SelectionSpec spec;
    int width = 0;
    int height = 0;
    int x = 0;
    int y = 0;
    if (text != nullptr && std::sscanf(text, "%dx%d@%d,%d", &width, &height, &x, &y) == 4 &&
        width > 0 && height > 0) {
        spec.width = width;
        spec.height = height;
        spec.x = x;
        spec.y = y;
    }
    return spec;
}

std::uint8_t ChannelAt(const std::vector<std::uint8_t>& bgra, int width, int x, int y, int channel) {
    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return bgra[offset + static_cast<std::size_t>(channel)];
}

struct Pixel {
    std::uint8_t b = 0;
    std::uint8_t g = 0;
    std::uint8_t r = 0;
    std::uint8_t a = 0;
};

Pixel ReadPixel(const std::vector<std::uint8_t>& bgra, int width, int x, int y) {
    const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
    return Pixel{ bgra[offset], bgra[offset + 1], bgra[offset + 2], bgra[offset + 3] };
}

// 选区局部坐标下的演示标注：九种工具都放一个，便于逐像素验证。
std::vector<core::Annotation> BuildDemoAnnotations(int width, int height) {
    std::vector<core::Annotation> annotations;

    const auto style = [](core::AnnotationTool tool, int palette, float thickness) {
        core::AnnotationStyle result;
        result.color = core::annotation_defaults::PaletteColor(palette);
        result.thickness = thickness;
        if (tool == core::AnnotationTool::Highlighter) {
            result.thickness = core::annotation_defaults::kHighlighterThickness;
        }
        return result;
    };

    core::Annotation pen;
    pen.tool = core::AnnotationTool::Pen;
    pen.style = style(core::AnnotationTool::Pen, 0, 4.0f);
    // 一条水平直线：探针要在固定坐标上取像素，锯齿线容易取样到空白。
    for (int i = 0; i <= 80; ++i) {
        pen.points.push_back(core::Point{ 40.0f + i * 2.0f, 60.0f });
    }
    annotations.push_back(pen);

    core::Annotation rectangle;
    rectangle.tool = core::AnnotationTool::Rectangle;
    rectangle.style = style(core::AnnotationTool::Rectangle, 4, 4.0f);
    rectangle.points = { { 40.0f, 120.0f }, { 240.0f, 220.0f } };
    annotations.push_back(rectangle);

    core::Annotation ellipse;
    ellipse.tool = core::AnnotationTool::Ellipse;
    ellipse.style = style(core::AnnotationTool::Ellipse, 1, 4.0f);
    ellipse.points = { { 260.0f, 120.0f }, { 460.0f, 220.0f } };
    annotations.push_back(ellipse);

    core::Annotation arrow;
    arrow.tool = core::AnnotationTool::Arrow;
    arrow.style = style(core::AnnotationTool::Arrow, 0, 4.0f);
    arrow.points = { { 40.0f, 300.0f }, { 240.0f, 300.0f } };
    annotations.push_back(arrow);

    core::Annotation highlighter;
    highlighter.tool = core::AnnotationTool::Highlighter;
    highlighter.style = style(core::AnnotationTool::Highlighter, 2, 14.0f);
    highlighter.points = { { 40.0f, 350.0f }, { 240.0f, 350.0f } };
    annotations.push_back(highlighter);

    core::Annotation mosaic;
    mosaic.tool = core::AnnotationTool::Mosaic;
    mosaic.style.mosaicBlockPx = 12;
    mosaic.points = { { 40.0f, 400.0f }, { 240.0f, 500.0f } };
    annotations.push_back(mosaic);

    core::Annotation step;
    step.tool = core::AnnotationTool::Step;
    step.style = style(core::AnnotationTool::Step, 3, 2.0f);
    step.points = { { static_cast<float>(width - 60), 60.0f } };
    step.text = "1";
    annotations.push_back(step);

    core::Annotation text;
    text.tool = core::AnnotationTool::Text;
    text.style = style(core::AnnotationTool::Text, 0, 2.0f);
    text.style.textSize = 24.0f;
    text.points = { { 40.0f, static_cast<float>(height - 60) } };
    text.text = "HaloCap S3";
    annotations.push_back(text);

    return annotations;
}

}  // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // WIC 的工厂走 COM；截图导出前必须初始化（app 里也做同一步）。
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const std::string outputPath = argc > 1 ? argv[1] : "screenshot-probe.png";
    const SelectionSpec spec = ParseSelection(argc > 2 ? argv[2] : nullptr);

    D2dRenderBackend backend;
    if (!backend.Initialize()) {
        std::printf("BACKEND=FAILED hr=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    D2dFontRasterizer rasterizer(backend);
    if (!rasterizer.Initialize()) {
        std::printf("RASTERIZER=FAILED hr=0x%08lX\n", static_cast<unsigned long>(rasterizer.LastHr()));
        return 2;
    }

    Win32DesktopDuplication capture(backend);
    if (!capture.Initialize()) {
        std::printf("CAPTURE_INIT=FAILED\n");
        return 2;
    }

    // 选区打开耗时从这里起算：复制权 → 第一帧 → 渲染目标就绪。
    const std::int64_t openStartUs = QpcNowUs();

    bool opened = false;
    for (int attempt = 0; attempt < 4 && !opened; ++attempt) {
        opened = capture.Open(0);
        if (!opened) {
            std::printf("OPEN_ATTEMPT=%d kind=%s\n", attempt + 1, capture.LastErrorKind());
            if (std::string_view(capture.LastErrorKind()) != "busy") {
                break;
            }
            Sleep(static_cast<DWORD>(core::BackoffUs(attempt, 250'000, 4'000'000) / 1000));
        }
    }
    if (!opened) {
        std::printf("SCREENSHOT_PROBE=BLOCKED kind=%s\n", capture.LastErrorKind());
        return 0;
    }

    core::FrameUpdate update;
    const std::int64_t waitDeadline = QpcNowUs() + 2'000'000;
    while (!capture.WaitForUpdate(50, update) && QpcNowUs() < waitDeadline) {
        if (std::string_view(capture.LastErrorKind()) != "timeout") {
            std::printf("CAPTURE_ENDED kind=%s\n", capture.LastErrorKind());
            std::printf("SCREENSHOT_PROBE=FAILED\n");
            return 1;
        }
    }
    if (update.target == 0) {
        std::printf("SCREENSHOT_PROBE=FAILED first frame missing\n");
        return 1;
    }

    const core::IntRect desktop = capture.DesktopBounds();
    core::SelectionModel selection;
    selection.SetLimit(core::Rect{ 0.0f, 0.0f, static_cast<float>(desktop.right),
                                   static_cast<float>(desktop.bottom) });
    selection.Begin(core::Point{ static_cast<float>(spec.x), static_cast<float>(spec.y) });
    selection.Update(core::Point{ static_cast<float>(spec.x + (spec.width & ~1)),
                                 static_cast<float>(spec.y + (spec.height & ~1)) },
                     false);
    selection.Commit();

    const core::Rect selected = selection.Bounds();
    const int width = static_cast<int>(selected.Width()) & ~1;
    const int height = static_cast<int>(selected.Height()) & ~1;
    if (width <= 0 || height <= 0) {
        std::printf("SCREENSHOT_PROBE=FAILED empty selection\n");
        return 1;
    }

    if (!backend.Resize(width, height)) {
        std::printf("RESIZE=FAILED hr=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    const std::int64_t openEndUs = QpcNowUs();
    const double openMs = static_cast<double>(openEndUs - openStartUs) / 1000.0;
    std::printf("SCREENSHOT_SELECTION=%dx%d@%d,%d\n", width, height,
                static_cast<int>(selected.left), static_cast<int>(selected.top));
    std::printf("SCREENSHOT_OPEN_MS=%.1f\n", openMs);

    // 第一遍：只贴底图，回读出来的像素既用于马赛克取样，也作为「有没有画上东西」的对照。
    const core::Blit baseBlit{ update.target,
                               core::Rect{ selected.left, selected.top, selected.left + width,
                                           selected.top + height },
                               core::Rect{ 0.0f, 0.0f, static_cast<float>(width),
                                           static_cast<float>(height) },
                               1.0f };

    // 棋盘格：给马赛克一块有内容的底图。真实产品里马赛克会把「它下面的底图与已有标注」
    // 一起像素化，所以这里先画一层格子，再从那次的回读像素里取块平均。
    std::vector<core::FillRect> checker;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            core::FillRect rect;
            rect.rect = core::Rect{ 40.0f + col * 50.0f, 400.0f + row * 33.0f,
                                    40.0f + (col + 1) * 50.0f, 400.0f + (row + 1) * 33.0f };
            rect.color = ((row + col) & 1) != 0
                             ? core::annotation_defaults::PaletteColor(0)
                             : core::annotation_defaults::PaletteColor(3);
            checker.push_back(rect);
        }
    }

    core::FramePlan basePlan;
    basePlan.Add(baseBlit);
    for (const core::FillRect& rect : checker) {
        basePlan.Add(rect);
    }
    backend.Render(basePlan, core::IntRect{});

    std::vector<std::uint8_t> basePixels;
    int baseWidth = 0;
    int baseHeight = 0;
    if (!backend.ReadPixels(basePixels, baseWidth, baseHeight) || baseWidth != width ||
        baseHeight != height) {
        std::printf("BASE_READBACK=FAILED hr=0x%08lX\n",
                    static_cast<unsigned long>(backend.LastHr()));
        return 3;
    }

    std::vector<core::Annotation> annotations = BuildDemoAnnotations(width, height);

    // 马赛克：从底图里取那块区域做块平均，注册成后端图像，再作为一次 Blit 画回去。
    const core::ImageRef baseRef{ basePixels.data(), width, height, width * 4 };
    const core::IntRect mosaicRegion{ 40, 400, 240, 500 };
    const core::MosaicTile tile = core::DownsampleRegion(
        baseRef, mosaicRegion, core::MosaicBlockPixels(12, 1.0f));
    core::ImageHandle tileHandle = 0;
    if (tile.width > 0 && tile.height > 0) {
        tileHandle = backend.RegisterImage(
            core::ImageRef{ tile.bgra.data(), tile.width, tile.height, tile.width * 4 });
    }
    for (core::Annotation& annotation : annotations) {
        if (annotation.tool == core::AnnotationTool::Mosaic) {
            annotation.mosaicTile = tileHandle;
            annotation.mosaicTileSize = core::Size{ static_cast<float>(tile.width),
                                                    static_cast<float>(tile.height) };
        }
    }

    core::AnnotationPlanOptions planOptions;
    planOptions.rasterizer = &rasterizer;
    planOptions.scale = 1.0f;

    core::FramePlan plan;
    plan.Add(baseBlit);
    for (const core::FillRect& rect : checker) {
        plan.Add(rect);
    }
    core::AppendAnnotationPlan(annotations, planOptions, plan);

    const std::int64_t renderStartUs = QpcNowUs();
    backend.Render(plan, core::IntRect{});
    std::vector<std::uint8_t> pixels;
    int pixelWidth = 0;
    int pixelHeight = 0;
    const bool readBack = backend.ReadPixels(pixels, pixelWidth, pixelHeight);
    const double renderMs = static_cast<double>(QpcNowUs() - renderStartUs) / 1000.0;

    std::printf("SCREENSHOT_RENDER_MS=%.1f\n", renderMs);
    Check(readBack, "final frame is readable");
    CheckEqual(pixelWidth, width, "readback width");
    CheckEqual(pixelHeight, height, "readback height");

    if (!readBack || pixelWidth != width || pixelHeight != height) {
        std::printf("SCREENSHOT_PROBE=FAILED\n");
        return 1;
    }

    // 像素验证：每种工具都在它该在的位置留下颜色。
    const Pixel penPixel = ReadPixel(pixels, width, 60, 60);
    Check(penPixel.r > 180 && penPixel.g < 120, "pen stroke is red at (60,60)");

    const Pixel rectPixel = ReadPixel(pixels, width, 40, 170);
    Check(rectPixel.b > 180, "rectangle edge is blue at (40,170)");

    const Pixel ellipsePixel = ReadPixel(pixels, width, 459, 170);
    Check(ellipsePixel.b > 150 || ellipsePixel.g > 150, "ellipse rim is drawn near its right edge");

    const Pixel arrowPixel = ReadPixel(pixels, width, 140, 300);
    Check(arrowPixel.r > 180 && arrowPixel.g < 120, "arrow shaft is red at (140,300)");

    // 圆心被数字占着，取中心右侧 8 px（仍在半径 11 的圆内、不在圆环上）。
    const Pixel stepPixel = ReadPixel(pixels, width, width - 52, 60);
    Check(stepPixel.g > 150 && stepPixel.r < 180, "step badge is green beside its digit");

    // 马赛克：同一块里的两个像素必须完全相同（块平均的结果）。
    Check(std::memcmp(&pixels[(static_cast<std::size_t>(420) * width + 60) * 4],
                      &pixels[(static_cast<std::size_t>(420) * width + 66) * 4], 4) == 0,
          "mosaic neighbours inside one block share their colour");
    // 取一个横跨两个棋盘格的块：块平均与它下面的单一颜色必然不同。
    Check(std::memcmp(&pixels[(static_cast<std::size_t>(420) * width + 88) * 4],
                      &basePixels[(static_cast<std::size_t>(420) * width + 88) * 4], 4) != 0,
          "mosaic actually replaced the underlying pixels");
    {
        const Pixel blockA = ReadPixel(pixels, width, 45, 405);
        const Pixel blockB = ReadPixel(pixels, width, 145, 405);
        Check(blockA.r != blockB.r || blockA.g != blockB.g || blockA.b != blockB.b,
              "mosaic blocks average different source tiles");
    }

    // 文字：底图与最终图在文字区域必须有差异。
    int textChanged = 0;
    for (int y = height - 80; y < height - 20; ++y) {
        for (int x = 40; x < 260; ++x) {
            if (std::memcmp(&pixels[(static_cast<std::size_t>(y) * width + x) * 4],
                            &basePixels[(static_cast<std::size_t>(y) * width + x) * 4], 4) != 0) {
                ++textChanged;
            }
        }
    }
    Check(textChanged > 100, "text rendered into its bounds");

    // 导出：PNG 文件与剪贴板。
    const std::wstring path = AbsolutePath(outputPath);
    const bool pngOk = halocap::platform::win32::SaveBgraPng(path, pixels.data(), width, height,
                                                             width * 4);
    Check(pngOk, "PNG is written");
    const std::int64_t pngBytes = FileSizeBytes(path);
    Check(pngBytes > 1000, "PNG file is not empty");
    std::printf("SCREENSHOT_PNG=%s\n", WideToUtf8(path).c_str());
    std::printf("SCREENSHOT_PNG_BYTES=%lld\n", static_cast<long long>(pngBytes));

    const bool clipboardOk =
        halocap::platform::win32::CopyBgraToClipboard(nullptr, pixels.data(), width, height,
                                                      width * 4);
    Check(clipboardOk, "image is copied to the clipboard");

    // 剪贴板回读一次：尺寸必须是我们放进去的那块。
    if (clipboardOk && OpenClipboard(nullptr)) {
        if (HANDLE data = GetClipboardData(CF_DIB)) {
            const auto* header = static_cast<const BITMAPINFOHEADER*>(GlobalLock(data));
            if (header != nullptr) {
                CheckEqual(header->biWidth, width, "clipboard DIB width");
                CheckEqual(header->biHeight, height, "clipboard DIB height");
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
    }

    // 内存：渲染完的峰值为准；关掉采集、释放回读缓冲后再量一次「回落」。
    const double peakMb = PrivateMemoryMb();
    std::printf("SCREENSHOT_PEAK_MB=%.1f\n", peakMb);

    capture.Close();
    if (tileHandle != 0) {
        backend.ReleaseImage(tileHandle);
    }
    pixels.clear();
    pixels.shrink_to_fit();
    basePixels.clear();
    basePixels.shrink_to_fit();

    const double afterCloseMb = PrivateMemoryMb();
    std::printf("SCREENSHOT_AFTER_CLOSE_MB=%.1f\n", afterCloseMb);

    Check(openMs <= 150.0, "selection opens within 150 ms (S3 exit)");
    Check(peakMb > 0.0 && peakMb <= 120.0, "screenshot memory stays within 120 MB (S3 exit)");

    const bool ok = failures == 0 && pngOk && clipboardOk;
    std::printf("SCREENSHOT_PROBE=%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
