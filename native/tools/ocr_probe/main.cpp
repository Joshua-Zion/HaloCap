// S4 的 OCR 验证：用 GDI 画一张已知文字的图，跑 PP-OCR，断言识别结果里包含每一行。
//
// 为什么不拿现成截图当基准：合成图有确定的 ground truth，能给出「准确率」而不是
// 「看起来像」。三行覆盖英文/数字/中文，中文字形来自系统字体，和真实截图同一路径。
#include "image_export.h"
#include "ocr.h"
#include "ocr_engine.h"
#include "ort_ocr_engine.h"
#include "winrt_ocr_engine.h"

#include <windows.h>

#include <objbase.h>

#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace core = halocap::core;
namespace ocr = halocap::core::ocr;
using halocap::platform::win32::OrtOcrEngine;
using halocap::platform::win32::WinrtOcrEngine;

namespace {

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

struct Bitmap {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

bool RenderTextBitmap(const std::vector<std::wstring>& lines, int fontSize, Bitmap& bitmap) {
    const int lineHeight = fontSize * 2;
    bitmap.width = 900;
    bitmap.height = 40 + lineHeight * static_cast<int>(lines.size());
    bitmap.bgra.assign(static_cast<std::size_t>(bitmap.width) * bitmap.height * 4, 255);

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = bitmap.width;
    info.bmiHeader.biHeight = -bitmap.height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(memory, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (dib == nullptr || bits == nullptr) {
        if (dib != nullptr) {
            DeleteObject(dib);
        }
        DeleteDC(memory);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(memory, dib);

    RECT full{ 0, 0, bitmap.width, bitmap.height };
    FillRect(memory, &full, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    HFONT font = CreateFontW(-fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             DEFAULT_PITCH, L"Microsoft YaHei UI");
    HGDIOBJ oldFont = SelectObject(memory, font);
    SetBkMode(memory, TRANSPARENT);
    SetTextColor(memory, RGB(0, 0, 0));

    for (std::size_t i = 0; i < lines.size(); ++i) {
        RECT row{ 30, 20 + static_cast<int>(i) * lineHeight, bitmap.width - 30,
                  20 + static_cast<int>(i + 1) * lineHeight };
        DrawTextW(memory, lines[i].c_str(), static_cast<int>(lines[i].size()), &row,
                  DT_LEFT | DT_TOP | DT_SINGLELINE);
    }

    SelectObject(memory, oldFont);
    DeleteObject(font);
    SelectObject(memory, old);
    std::memcpy(bitmap.bgra.data(), bits, bitmap.bgra.size());
    DeleteObject(dib);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);

    for (std::size_t i = 3; i < bitmap.bgra.size(); i += 4) {
        bitmap.bgra[i] = 255;
    }
    return true;
}

std::string Normalize(std::string text) {
    std::string result;
    for (const char character : text) {
        if (character != ' ' && character != '\t' && character != '\r' && character != '\n') {
            result.push_back(character);
        }
    }
    return result;
}

std::string ResolveModelDirectory(const std::string& explicitPath) {
    if (!explicitPath.empty()) {
        return explicitPath;
    }
    if (const char* environment = std::getenv("HALOCAP_OCR_MODELS")) {
        return environment;
    }

    const char* candidates[] = {
        "src\\HaloCap\\Assets\\OcrModel",
        "..\\src\\HaloCap\\Assets\\OcrModel",
        "..\\..\\src\\HaloCap\\Assets\\OcrModel",
        "..\\..\\..\\src\\HaloCap\\Assets\\OcrModel",
    };
    for (const char* candidate : candidates) {
        const std::wstring wide = Utf8ToWide(candidate);
        const std::wstring probe = wide + L"\\ppocrv6_tiny_det.onnx";
        if (GetFileAttributesW(probe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return {};
}

bool SaveInputPng(const Bitmap& bitmap, const std::string& path) {
    if (path.empty()) {
        return false;
    }
    return halocap::platform::win32::SaveBgraPng(Utf8ToWide(path), bitmap.bgra.data(),
                                                 bitmap.width, bitmap.height, bitmap.width * 4);
}

}

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    std::string modelDirectory;
    std::string pngPath = "ocr-input.png";
    std::string engineName = "pp";
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--models" && i + 1 < argc) {
            modelDirectory = argv[++i];
        } else if (argument == "--png" && i + 1 < argc) {
            pngPath = argv[++i];
        } else if (argument == "--engine" && i + 1 < argc) {
            engineName = argv[++i];
        }
    }

    const std::vector<std::wstring> expected = { L"HaloCap 2.0 OCR", L"重构 2.0 测试",
                                                 L"1234567890" };
    Bitmap bitmap;
    if (!RenderTextBitmap(expected, 40, bitmap)) {
        std::printf("OCR_PROBE=FAILED bitmap\n");
        return 2;
    }
    std::printf("OCR_INPUT=%dx%d\n", bitmap.width, bitmap.height);
    std::printf("OCR_INPUT_PNG=%s\n", SaveInputPng(bitmap, pngPath) ? pngPath.c_str() : "FAILED");

    if (engineName != "winrt") {
        modelDirectory = ResolveModelDirectory(modelDirectory);
        if (modelDirectory.empty()) {
            std::printf("OCR_MODELS=NOT_FOUND\n");
            std::printf("OCR_PROBE=BLOCKED\n");
            return 0;
        }
        std::printf("OCR_MODELS=%s\n", modelDirectory.c_str());
    }

    const double beforeMb = PrivateMemoryMb();

    core::OcrEngineConfig config;
    config.modelDirectory = modelDirectory;
    config.threads = 2;

    std::unique_ptr<core::IOcrEngine> engine;
    if (engineName == "winrt") {
        engine = std::make_unique<WinrtOcrEngine>();
    } else {
        engine = std::make_unique<OrtOcrEngine>();
    }
    std::printf("OCR_ENGINE=%s\n", engine->Name());
    std::string error;
    const std::int64_t initStart = QpcNowUs();
    const bool initialized = engine->Initialize(config, &error);
    const double initMs = static_cast<double>(QpcNowUs() - initStart) / 1000.0;
    std::printf("OCR_INIT_MS=%.1f\n", initMs);
    if (!initialized) {
        std::printf("OCR_INIT=FAILED error=%s\n", error.c_str());
        std::printf("OCR_PROBE=FAILED\n");
        return 2;
    }
    const double afterInitMb = PrivateMemoryMb();

    const ocr::OcrImage image{ bitmap.bgra.data(), bitmap.width, bitmap.height, bitmap.width * 4 };
    const std::int64_t recognizeStart = QpcNowUs();
    const ocr::OcrResult result = engine->Recognize(image, ocr::OcrOptions{}, &error);
    const double recognizeMs = static_cast<double>(QpcNowUs() - recognizeStart) / 1000.0;

    std::printf("OCR_RECOGNIZE_MS=%.1f\n", recognizeMs);
    std::printf("OCR_LINE_COUNT=%zu\n", result.lines.size());
    for (const ocr::OcrLine& line : result.lines) {
        std::printf("OCR_LINE=%s|conf=%.3f|rect=%.0f,%.0f,%.0f,%.0f\n", line.text.c_str(),
                    line.confidence, line.box.left, line.box.top, line.box.Width(),
                    line.box.Height());
    }

    const std::string normalizedText = Normalize(result.plainText);
    int matched = 0;
    for (const std::wstring& line : expected) {
        const std::string needle = Normalize(WideToUtf8(line));
        const bool found = normalizedText.find(needle) != std::string::npos;
        std::printf("OCR_MATCH=%s|%s\n", found ? "1" : "0", WideToUtf8(line).c_str());
        matched += found ? 1 : 0;
    }
    std::printf("OCR_MATCH_COUNT=%d/%zu\n", matched, expected.size());

    const double afterMb = PrivateMemoryMb();
    engine.reset();
    const double afterReleaseMb = PrivateMemoryMb();

    std::printf("OCR_PRIVATE_BEFORE_MB=%.1f\n", beforeMb);
    std::printf("OCR_PRIVATE_AFTER_INIT_MB=%.1f\n", afterInitMb);
    std::printf("OCR_PRIVATE_AFTER_RECOGNIZE_MB=%.1f\n", afterMb);
    std::printf("OCR_PRIVATE_AFTER_RELEASE_MB=%.1f\n", afterReleaseMb);

    const bool ok = matched == static_cast<int>(expected.size()) && !result.lines.empty();
    std::printf("OCR_PROBE=%s\n", ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}
