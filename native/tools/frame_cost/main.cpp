// S1 的覆盖层帧成本测量：与 spike 3（spikes/OverlayFrameCost）同一套配置与口径，
// 区别是这里走原生 D2D 设备上下文 + core 显示列表，spike 3 走托管手写 interop + D2D 1.0 离屏目标。
// 文本那一行没有复现：字形 run 要等 IFontRasterizer，见 docs/2.0-native-plan.md 的说明。
#include "d2d_render_backend.h"

#include <windows.h>

#include <psapi.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

// 不用 using namespace：winuser.h 里有同名的 FillRect 函数，会让名字二义。
namespace core = halocap::core;
using halocap::platform::win32::D2dRenderBackend;

namespace {

struct Config {
    int width;
    int height;
    int frames;
    int primitives;
};

// spike 3 跑过的五个配置，逐个对照。
constexpr Config kMatrix[] = {
    { 320, 240, 120, 2 },
    { 1280, 720, 120, 2 },
    { 3840, 2160, 120, 2 },
    { 3840, 2160, 120, 300 },
    { 3840, 2160, 120, 1000 },
};

class Clock {
public:
    void Start() { QueryPerformanceCounter(&begin_); }

    double ElapsedMs() const {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        static const LARGE_INTEGER frequency = [] {
            LARGE_INTEGER value{};
            QueryPerformanceFrequency(&value);
            return value;
        }();
        return static_cast<double>(now.QuadPart - begin_.QuadPart) * 1000.0 /
               static_cast<double>(frequency.QuadPart);
    }

private:
    LARGE_INTEGER begin_{};
};

double WorkingSetMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                                 sizeof(counters))) {
        return -1.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / 1048576.0;
}

// 冻结画面：与 spike 3 完全相同的图样，保证 GPU 采样的内容一致。
std::unique_ptr<unsigned char[]> MakeFrozen(int width, int height) {
    auto buffer = std::make_unique<unsigned char[]>(static_cast<std::size_t>(width) * height * 4);
    const std::size_t pitch = static_cast<std::size_t>(width) * 4;

    for (int y = 0; y < height; ++y) {
        unsigned char* row = buffer.get() + static_cast<std::size_t>(y) * pitch;
        for (int x = 0; x < width; ++x) {
            const auto value = static_cast<unsigned char>(((x >> 3) + (y >> 3)) & 0xFF);
            row[static_cast<std::size_t>(x) * 4] = value;
            row[static_cast<std::size_t>(x) * 4 + 1] = static_cast<unsigned char>(255 - value);
            row[static_cast<std::size_t>(x) * 4 + 2] = 128;
            row[static_cast<std::size_t>(x) * 4 + 3] = 255;
        }
    }
    return buffer;
}

core::Rect FullTarget(int width, int height) {
    return core::Rect{ 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height) };
}

void BuildPlan(core::FramePlan& plan,
               core::ImageHandle frozen,
               int width,
               int height,
               int primitives,
               int frame) {
    const core::Color line{ 1.0f, 0.2f, 0.2f, 1.0f };
    const core::Color fill{ 0.1f, 0.6f, 1.0f, 0.35f };
    const core::Rect full = FullTarget(width, height);

    plan.Clear();
    plan.Add(core::Blit{ frozen, full, full, 1.0f });
    // 放大镜：源 200x200 放大到 600x600。
    plan.Add(core::Blit{ frozen,
                         core::Rect{ 400.0f, 400.0f, 600.0f, 600.0f },
                         core::Rect{ 80.0f, 80.0f, 680.0f, 680.0f },
                         1.0f });

    const int shift = frame % 64;
    const int spanX = width > 200 ? width - 200 : 1;
    const int spanY = height > 200 ? height - 200 : 1;

    for (int i = 0; i < primitives; ++i) {
        const float left = static_cast<float>((i * 37 + shift) % spanX);
        const float top = static_cast<float>((i * 61) % spanY);

        plan.Add(core::Segment{ core::Point{ left, top },
                                core::Point{ left + 140.0f, top + 90.0f },
                                line,
                                2.5f });

        const core::Rect rect{ left + 20.0f, top + 20.0f, left + 120.0f, top + 80.0f };
        if ((i & 1) == 0) {
            plan.Add(core::FillRect{ rect, fill });
        } else {
            plan.Add(core::StrokeRect{ rect, line, 1.5f });
        }
    }
}

// 先证明「真画出来了」：只放整面 blit，再回读中心像素，应当等于冻结画面的图样。
bool VerifyPixels(D2dRenderBackend& backend, core::ImageHandle frozen, const Config& config) {
    core::FramePlan probe;
    probe.Add(core::Blit{ frozen, FullTarget(config.width, config.height),
                          FullTarget(config.width, config.height), 1.0f });

    backend.Render(probe, core::IntRect{});
    backend.Flush();

    unsigned char pixel[4] = { 0, 0, 0, 0 };
    if (!backend.SyncToCpu(pixel)) {
        std::printf("PROBE=SYNC-FAILED hr=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return false;
    }

    const int centerX = config.width / 2;
    const int centerY = config.height / 2;
    const unsigned char expected = static_cast<unsigned char>(((centerX >> 3) + (centerY >> 3)) & 0xFF);
    const bool exact = pixel[0] == expected && pixel[1] == static_cast<unsigned char>(255 - expected) &&
                       pixel[2] == 128 && pixel[3] == 255;

    std::printf("PROBE_PIXEL=%u,%u,%u,%u expected=%u,%u,128,255 PROBE=%s\n",
                static_cast<unsigned>(pixel[0]), static_cast<unsigned>(pixel[1]),
                static_cast<unsigned>(pixel[2]), static_cast<unsigned>(pixel[3]),
                static_cast<unsigned>(expected), static_cast<unsigned>(255 - expected),
                exact ? "EXACT" : "MISMATCH");
    return exact;
}

bool RunConfig(D2dRenderBackend& backend, const Config& config) {
    std::printf("CONFIG %dx%d frames=%d primitives=%d\n",
                config.width, config.height, config.frames, config.primitives);

    if (!backend.Resize(config.width, config.height)) {
        std::printf("RESIZE_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return false;
    }
    if (!backend.EnableCpuReadback()) {
        std::printf("READBACK_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return false;
    }

    const auto frozenBuffer = MakeFrozen(config.width, config.height);
    const core::ImageRef imageRef{ frozenBuffer.get(), config.width, config.height, config.width * 4 };
    const core::ImageHandle frozen = backend.RegisterImage(imageRef);
    if (frozen == 0) {
        std::printf("REGISTER_IMAGE_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return false;
    }

    if (!VerifyPixels(backend, frozen, config)) {
        return false;
    }

    core::FramePlan plan;
    Clock clock;
    const double workingSetBefore = WorkingSetMb();

    // 冷一趟：和 spike 3 一样从目标刚建好开始计时。
    clock.Start();
    for (int frame = 0; frame < config.frames; ++frame) {
        BuildPlan(plan, frozen, config.width, config.height, config.primitives, frame);
        backend.Render(plan, core::IntRect{});
    }
    backend.Flush();
    backend.SyncToCpu();
    const double coldMs = clock.ElapsedMs() / config.frames;

    // 热一趟：同样的活再来一遍，把首帧的建资源开销分出去。
    clock.Start();
    for (int frame = 0; frame < config.frames; ++frame) {
        BuildPlan(plan, frozen, config.width, config.height, config.primitives, frame);
        backend.Render(plan, core::IntRect{});
    }
    backend.Flush();
    backend.SyncToCpu();
    const double warmMs = clock.ElapsedMs() / config.frames;

    // 每帧都回读：spike 3 里 8-11 ms 那一档，2.0 明令禁止的写法。
    const int readbackFrames = config.frames < 60 ? config.frames : 60;
    clock.Start();
    for (int frame = 0; frame < readbackFrames; ++frame) {
        BuildPlan(plan, frozen, config.width, config.height, config.primitives, frame);
        backend.Render(plan, core::IntRect{});
        backend.SyncToCpu();
    }
    const double readbackMs = clock.ElapsedMs() / readbackFrames;

    std::printf("ITEMS_PER_FRAME=%d\n", 2 + config.primitives * 2);
    std::printf("SUBMIT_MS_PER_FRAME_COLD=%.3f\n", coldMs);
    std::printf("SUBMIT_MS_PER_FRAME_WARM=%.3f\n", warmMs);
    std::printf("READBACK_MS_PER_FRAME=%.3f\n", readbackMs);

    // 只重绘变化区：整张列表照旧，脏区缩到放大镜那一块（4K 的 4.1%）。
    if (config.width >= 680 && config.height >= 680) {
        const core::IntRect dirty{ 80, 80, 680, 680 };
        clock.Start();
        for (int frame = 0; frame < config.frames; ++frame) {
            BuildPlan(plan, frozen, config.width, config.height, config.primitives, frame);
            backend.Render(plan, dirty);
        }
        backend.Flush();
        backend.SyncToCpu();
        const double partialMs = clock.ElapsedMs() / config.frames;
        std::printf("PARTIAL_CLIP=80,80,680,680 PARTIAL_MS_PER_FRAME_WARM=%.3f\n", partialMs);
        std::printf("PARTIAL_RATIO=%.3f\n", warmMs > 0.0 ? partialMs / warmMs : 0.0);
    }

    std::printf("ENDDRAW_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
    std::printf("WORKING_SET_MB=%.0f->%.0f\n", workingSetBefore, WorkingSetMb());

    backend.ReleaseImage(frozen);
    return backend.LastHr() == S_OK;
}

}

int main(int argc, char** argv) {
    D2dRenderBackend backend;
    if (!backend.Initialize()) {
        std::printf("INITIALIZE_HR=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    std::printf("BACKEND=D2D1.1-device-context\n");

    if (argc >= 5) {
        const Config single{ std::atoi(argv[1]), std::atoi(argv[2]), std::atoi(argv[3]), std::atoi(argv[4]) };
        const bool ok = RunConfig(backend, single);
        std::printf("OUTCOME=%s\n", ok ? "MEASURED" : "FAILED");
        return ok ? 0 : 2;
    }

    bool allOk = true;
    for (const Config& config : kMatrix) {
        allOk = RunConfig(backend, config) && allOk;
    }

    std::printf("OUTCOME=%s\n", allOk ? "MEASURED" : "FAILED");
    return allOk ? 0 : 2;
}
