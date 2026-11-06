// ICaptureSource 的第一块验证：桌面复制走常驻纹理 + 只搬脏矩形，并且把「复制权被占用」当常态分支跑出来。
//
// 这一轮会真的动屏幕：为了产生可测的内容变化，探针在屏幕上开一个小方块（240x160，不激活、工具窗口），
// 在采集循环里每隔几帧搬它一次——spike 2 的教训是静态桌面 20 秒只送 149 帧，测不出东西。
// 默认跑 5 秒，方块只在第一个可复制输出的右下角一小块里活动。
//
// 用法：capture_probe.exe [输出序号] [时长毫秒] [几帧搬一次]
#include "capture_source.h"
#include "d2d_render_backend.h"
#include "dxgi_desktop_duplication.h"

#include <windows.h>

#include <psapi.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace core = halocap::core;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::Win32DesktopDuplication;

namespace {

constexpr wchar_t kMoverClass[] = L"HaloCapCaptureMover";
constexpr int kMoverWidth = 240;
constexpr int kMoverHeight = 160;

LRESULT CALLBACK MoverProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        HBRUSH brush = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        FillRect(dc, &paint.rcPaint, brush);
        EndPaint(hwnd, &paint);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

HWND CreateMover(int x, int y) {
    static const ATOM atom = [] {
        WNDCLASSEXW description{};
        description.cbSize = sizeof(description);
        description.lpfnWndProc = &MoverProc;
        description.hInstance = GetModuleHandleW(nullptr);
        description.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        description.lpszClassName = kMoverClass;
        return RegisterClassExW(&description);
    }();

    if (atom == 0) {
        return nullptr;
    }

    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kMoverClass, L"HaloCap capture mover",
                           WS_POPUP | WS_VISIBLE, x, y, kMoverWidth, kMoverHeight, nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
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
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / 1048576.0;
}

// E_ACCESSDENIED 有两种常见原因：别的程序正持有复制权，或者本进程的安全上下文与桌面不一致。
// 后者用「是否提升」就能给出强信号，所以只读 TokenElevation——
// 实测 TokenIntegrityLevel 那次查询在这台机器上会让进程崩掉，不值得为多一个数字冒这个险。
void ReportTokenState() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        std::printf("TOKEN_QUERY=FAILED lastError=%lu\n", static_cast<unsigned long>(GetLastError()));
        return;
    }

    TOKEN_ELEVATION elevation{};
    DWORD length = 0;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &length)) {
        std::printf("TOKEN_ELEVATED=%d\n", elevation.TokenIsElevated != 0 ? 1 : 0);
    } else {
        std::printf("TOKEN_ELEVATION=FAILED lastError=%lu\n",
                    static_cast<unsigned long>(GetLastError()));
    }

    CloseHandle(token);
}

double ThreadCpuMs(const FILETIME& creation, const FILETIME& exit, const FILETIME& kernel,
                   const FILETIME& user) {
    (void)creation;
    (void)exit;
    const auto ToUs = [](const FILETIME& value) {
        return (static_cast<std::int64_t>(value.dwHighDateTime) << 32 | value.dwLowDateTime) / 10;
    };
    return static_cast<double>(ToUs(kernel) + ToUs(user)) / 1000.0;
}

}

int main(int argc, char** argv) {
    // 关掉缓冲：这个工具会在屏幕上动东西、也可能崩，日志必须留得下来。
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int outputIndex = argc > 1 ? std::atoi(argv[1]) : 0;
    const int durationMs = argc > 2 ? std::atoi(argv[2]) : 5000;
    const int moveEvery = argc > 3 ? std::atoi(argv[3]) : 3;

    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        std::printf("DPI_AWARE=FAILED\n");
    }

    D2dRenderBackend backend;
    if (!backend.Initialize()) {
        std::printf("BACKEND_INITIALIZE_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    Win32DesktopDuplication capture(backend);
    if (!capture.Initialize()) {
        std::printf("CAPTURE_INITIALIZE=FAILED kind=%s\n", capture.LastErrorKind());
        return 2;
    }

    std::printf("OUTPUTS_SYSTEM=%d OUTPUTS_CAPTURABLE=%d\n", capture.TotalOutputCount(),
                capture.OutputCount());
    for (int i = 0; i < capture.OutputCount(); ++i) {
        const core::IntRect bounds = capture.OutputDesktopBounds(i);
        std::printf("  output %d desktop=%d,%d..%d,%d (%dx%d)\n", i, bounds.left, bounds.top,
                    bounds.right, bounds.bottom, bounds.right - bounds.left,
                    bounds.bottom - bounds.top);
    }
    if (capture.OutputCount() < capture.TotalOutputCount()) {
        std::printf("  差值就是「设备与适配器必须配对」：另外 %d 个输出挂在不属于本机设备的适配器上\n",
                    capture.TotalOutputCount() - capture.OutputCount());
    }

    // 复制权被占用是常态分支：按 core 的退避序列重试，等不到就把它当结论报出来，而不是当崩溃。
    bool opened = capture.Open(outputIndex);
    int attempts = 0;
    const int maxAttempts = argc > 4 ? std::atoi(argv[4]) : 6;
    while (!opened && std::string_view(capture.LastErrorKind()) == "busy" && attempts < maxAttempts) {
        const std::int64_t waitUs = core::BackoffUs(attempts, 250000, 4000000);
        std::printf("OPEN_BUSY attempt=%d backoffMs=%lld\n", attempts,
                    static_cast<long long>(waitUs / 1000));
        Sleep(static_cast<DWORD>(waitUs / 1000));
        PumpMessages();
        ++attempts;
        opened = capture.Open(outputIndex);
    }

    if (!opened) {
        const std::string_view kind = capture.LastErrorKind();
        std::printf("OPEN=FAILED kind=%s attempts=%d\n", kind.data(), attempts);
        if (kind == "busy") {
            ReportTokenState();
            std::printf("HINT=别的程序正持有桌面复制，或本进程的完整性级别与桌面不一致。"
                        "用非提升的终端跑一次，或关掉可能截屏的程序再试。\n");
            std::printf("OUTCOME=BUSY\n");
            return 4;
        }
        return 3;
    }
    const core::IntRect desktop = capture.DesktopBounds();
    std::printf("OPEN=OK output=%d bounds=%d,%d..%d,%d\n", outputIndex, desktop.left, desktop.top,
                desktop.right, desktop.bottom);

    // 「复制权被占用」这一条不再是偶发观察：同一个输出开第二份复制必然被拒，
    // 所以能确定性地验证 LastErrorKind 把 busy 分出来了。
    {
        Win32DesktopDuplication competitor(backend);
        competitor.Initialize();
        const bool secondOpened = competitor.Open(outputIndex);
        std::printf("SECOND_DUPLICATION_OPENED=%d kind=%s EXPECT_BUSY=%d\n", secondOpened ? 1 : 0,
                    competitor.LastErrorKind(),
                    (!secondOpened && std::string_view(competitor.LastErrorKind()) == "busy") ? 1 : 0);
    }

    // 变化源：一个小方块在输出的右下角来回搬。
    const int areaLeft = desktop.right - 900;
    const int areaTop = desktop.bottom - 400;
    HWND mover = CreateMover(areaLeft, areaTop);
    if (mover == nullptr) {
        std::printf("MOVER_WINDOW=FAILED lastError=%lu\n", static_cast<unsigned long>(GetLastError()));
        return 4;
    }

    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernelStart{};
    FILETIME userStart{};
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernelStart, &userStart);

    std::int64_t acquired = 0;
    std::int64_t changed = 0;
    std::int64_t cursorOnly = 0;
    std::int64_t timeouts = 0;
    std::int64_t busy = 0;
    std::int64_t lost = 0;
    std::int64_t dirtyPixels = 0;
    std::int64_t boxPixels = 0;
    std::int64_t maxRects = 0;
    std::int64_t maxDirtyInOneFrame = 0;
    std::int64_t step = 0;
    std::int64_t lastMoveUs = 0;

    const std::int64_t beginUs = QpcNowUs();
    const std::int64_t stopUs = beginUs + static_cast<std::int64_t>(durationMs) * 1000;

    while (QpcNowUs() < stopUs) {
        const std::int64_t nowUs = QpcNowUs();
        if (moveEvery > 0 && (acquired % static_cast<std::int64_t>(moveEvery)) == 0 &&
            nowUs - lastMoveUs > 8000) {
            const int offset = static_cast<int>(step % 300);
            SetWindowPos(mover, nullptr, areaLeft + offset, areaTop + (offset / 3), kMoverWidth,
                         kMoverHeight, SWP_NOACTIVATE | SWP_NOZORDER);
            ++step;
            lastMoveUs = nowUs;
        }
        PumpMessages();

        core::FrameUpdate update{};
        if (!capture.WaitForUpdate(200, update)) {
            const std::string_view kind = capture.LastErrorKind();
            if (kind == "timeout") {
                ++timeouts;
            } else if (kind == "busy") {
                ++busy;
            } else if (kind == "lost") {
                ++lost;
            }
            continue;
        }

        ++acquired;
        if (!update.contentChanged) {
            ++cursorOnly;
            continue;
        }

        ++changed;
        const core::DirtySummary summary = core::SummarizeDirtyRects(update.dirtyRects);
        dirtyPixels += summary.sumAreaPx;
        boxPixels += summary.boundingBoxAreaPx;
        if (summary.rectCount > maxRects) {
            maxRects = summary.rectCount;
        }
        if (summary.sumAreaPx > maxDirtyInOneFrame) {
            maxDirtyInOneFrame = summary.sumAreaPx;
        }
    }

    FILETIME kernelEnd{};
    FILETIME userEnd{};
    GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernelEnd, &userEnd);

    const double elapsedMs = static_cast<double>(QpcNowUs() - beginUs) / 1000.0;
    const double cpuMs =
        ThreadCpuMs(creation, exit, kernelEnd, userEnd) -
        ThreadCpuMs(creation, exit, kernelStart, userStart);
    const double fullFramePixels =
        static_cast<double>(desktop.right - desktop.left) * (desktop.bottom - desktop.top);
    const double totalFrameArea = static_cast<double>(changed) * fullFramePixels;

    std::printf("ELAPSED_MS=%.0f ACQUIRED=%lld CHANGED=%lld CURSOR_ONLY=%lld TIMEOUT=%lld BUSY=%lld LOST=%lld\n",
                elapsedMs, static_cast<long long>(acquired), static_cast<long long>(changed),
                static_cast<long long>(cursorOnly), static_cast<long long>(timeouts),
                static_cast<long long>(busy), static_cast<long long>(lost));
    std::printf("CONTENT_RATE_FPS=%.2f WAKE_RATE_FPS=%.2f\n",
                changed * 1000.0 / elapsedMs, acquired * 1000.0 / elapsedMs);
    std::printf("DIRTY_PIXELS_TOTAL=%lld FULL_FRAME_TOTAL=%.0f DIRTY_VS_FULL=%.4f\n",
                static_cast<long long>(dirtyPixels), totalFrameArea,
                totalFrameArea > 0.0 ? static_cast<double>(dirtyPixels) / totalFrameArea : 0.0);
    std::printf("DIRTY_BOX_TOTAL=%lld BOX_VS_SUM=%.2f MAX_RECTS=%lld MAX_DIRTY_ONE_FRAME=%lld\n",
                static_cast<long long>(boxPixels),
                dirtyPixels > 0 ? static_cast<double>(boxPixels) / static_cast<double>(dirtyPixels) : 0.0,
                static_cast<long long>(maxRects), static_cast<long long>(maxDirtyInOneFrame));
    std::printf("CPU_MS=%.0f CPU_PER_CHANGED_FRAME_MS=%.3f CPU_LOAD=%.3f\n", cpuMs,
                changed > 0 ? cpuMs / static_cast<double>(changed) : 0.0,
                elapsedMs > 0 ? cpuMs / elapsedMs : 0.0);
    std::printf("RESIDENT_TEXTURE_MB=%.0f PRIVATE_MEMORY_MB=%.0f\n",
                fullFramePixels * 4.0 / 1048576.0, PrivateMemoryMb());

    const bool usedFallback = capture.UsedFullFrameFallback();
    std::printf("USED_FULL_FRAME_FALLBACK=%d\n", usedFallback ? 1 : 0);

    capture.Close();
    std::printf("PRIVATE_MEMORY_AFTER_CLOSE_MB=%.0f\n", PrivateMemoryMb());

    DestroyWindow(mover);

    const bool sane = changed > 0 && acquired >= changed;
    std::printf("OUTCOME=%s\n", sane ? "MEASURED" : "FAILED");
    return sane ? 0 : 2;
}
