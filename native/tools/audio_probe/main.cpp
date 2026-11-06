// 音频 seam 的一次跑通：端点给什么形状、静音时回环出不出包、环形缓冲对不对、
// 以及「借来的包不还就想再等」这条契约会不会真的被拒。全程不碰屏幕、不出声。
#include "wasapi_audio_source.h"

#include "audio_source.h"

#include <psapi.h>
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;
using halocap::platform::win32::WasapiAudioSource;

std::int64_t NowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart * 1000000 / freq.QuadPart);
}

double ProcessCpuMs(const FILETIME& kernel, const FILETIME& user) {
    FILETIME creation{}, exitTime{}, kernelNow{}, userNow{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelNow, &userNow);
    const auto toUs = [](const FILETIME& v) {
        return (static_cast<std::int64_t>(v.dwHighDateTime) << 32 | v.dwLowDateTime) / 10;
    };
    return static_cast<double>(toUs(kernelNow) - toUs(kernel) + toUs(userNow) - toUs(user)) /
           1000.0;
}

void CaptureCpuBaseline(FILETIME& kernel, FILETIME& user) {
    FILETIME creation{}, exitTime{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
}

double PrivateMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

struct CaptureRun {
    std::int64_t packets = 0;
    std::int64_t timeouts = 0;
    std::int64_t silentPackets = 0;
    std::int64_t discontinuities = 0;
    std::int64_t frames = 0;
    std::int64_t maxPacketFrames = 0;
    std::int64_t firstDevicePosition = -1;
    std::int64_t lastDevicePosition = -1;
    std::int32_t firstPacketFrames = 0;
    // QPC 位置与取包时刻的差：这一条决定「音频的时间戳能不能直接用来对齐视频」。
    std::int64_t qpcDeltaAbsMaxUs = 0;
    float peak = 0.0f;
    double sumSquares = 0.0;
    std::int64_t sampleCount = 0;
    double wallMs = 0.0;
    double cpuMs = 0.0;
    const char* failKind = "ok";
};

// 一次捕获：既量 seam 的行为，也把每一包走一遍 core 的折立体声 + 环形缓冲，
// 这样「管线接上以后数字对不对」是同一次跑出来的，不是另写一遍假数据。
CaptureRun RunCapture(WasapiAudioSource& source, std::int64_t durationMs, int sampleRateHz,
                      int& failures) {
    CaptureRun run{};

    // 环形缓冲按「一秒」建：容量本身就是 §4 那条队列深度的口径。
    core::StereoRingBuffer ring(static_cast<std::size_t>(sampleRateHz));
    std::vector<float> remix;
    std::vector<float> pulled;

    const std::int64_t beginUs = NowUs();
    const std::int64_t stopUs = beginUs + durationMs * 1000;

    FILETIME kernel{};
    FILETIME user{};
    CaptureCpuBaseline(kernel, user);

    int channelCount = static_cast<int>(source.Format().channelCount);
    remix.resize(static_cast<size_t>(4800) * 2);
    pulled.resize(static_cast<size_t>(4800) * 2);

    while (NowUs() < stopUs) {
        core::AudioChunk chunk{};
        if (!source.WaitForPacket(50, chunk)) {
            const std::string_view kind = source.LastErrorKind();
            if (kind == "timeout") {
                ++run.timeouts;
            } else {
                run.failKind = source.LastErrorKind();
                std::printf("  WAIT_FAILED kind=%s code=0x%08lX\n", run.failKind,
                            static_cast<unsigned long>(source.LastErrorCode()));
                ++failures;
                break;
            }
            continue;
        }

        ++run.packets;
        if (chunk.silent) {
            ++run.silentPackets;
        }
        if (chunk.discontinuity) {
            ++run.discontinuities;
        }
        run.frames += chunk.frameCount;
        if (chunk.frameCount > run.maxPacketFrames) {
            run.maxPacketFrames = chunk.frameCount;
        }
        if (run.firstDevicePosition < 0) {
            run.firstDevicePosition = chunk.devicePositionFrames;
            run.firstPacketFrames = chunk.frameCount;
        }
        run.lastDevicePosition = chunk.devicePositionFrames;

        const std::int64_t readUs = NowUs();
        if (chunk.timestampUs > 0) {
            const std::int64_t delta = readUs - chunk.timestampUs;
            const std::int64_t absolute = delta < 0 ? -delta : delta;
            if (absolute > run.qpcDeltaAbsMaxUs) {
                run.qpcDeltaAbsMaxUs = absolute;
            }
        }

        // 折成立体声再进环形缓冲：声道数不是 2 的时候这条一定会被用到。
        if (static_cast<std::size_t>(chunk.frameCount) * 2 > remix.size()) {
            remix.resize(static_cast<size_t>(chunk.frameCount) * 2);
        }
        core::RemixToStereo(remix.data(), chunk.samples, chunk.frameCount, channelCount);

        for (std::int32_t i = 0; i < chunk.frameCount * 2; ++i) {
            const float magnitude = std::fabs(remix[static_cast<size_t>(i)]);
            if (magnitude > run.peak) {
                run.peak = magnitude;
            }
            run.sumSquares += static_cast<double>(remix[static_cast<size_t>(i)]) *
                              remix[static_cast<size_t>(i)];
            ++run.sampleCount;
        }

        ring.Push(remix.data(), chunk.frameCount);

        // 取走一半，模拟「按块编码」的消费，让缓冲不会只是写满。
        const std::int32_t consume = chunk.frameCount / 2;
        if (consume > 0) {
            ring.Pop(pulled.data(), consume);
        }

        source.ReleasePacket();
    }

    run.wallMs = static_cast<double>(NowUs() - beginUs) / 1000.0;
    run.cpuMs = ProcessCpuMs(kernel, user);

    // 缓冲的账必须对得上：推进去的总数 = 取走的 + 还在里面的 + 丢掉的最旧帧。
    const std::int64_t accounted = ring.PushedFrames();
    if (accounted != run.frames) {
        std::printf("  FAIL ring pushed=%lld but packets gave=%lld\n",
                    static_cast<long long>(accounted), static_cast<long long>(run.frames));
        ++failures;
    }
    if (ring.AvailableFrames() < 0 ||
        ring.AvailableFrames() > ring.CapacityFrames()) {
        std::printf("  FAIL ring available=%d out of capacity=%d\n", ring.AvailableFrames(),
                    ring.CapacityFrames());
        ++failures;
    }

    std::printf("  RING pushed=%lld available=%lld dropped=%lld rms=%.5f peak=%.3f\n",
                static_cast<long long>(ring.PushedFrames()),
                static_cast<long long>(ring.AvailableFrames()),
                static_cast<long long>(ring.DroppedFrames()),
                run.sampleCount > 0 ? std::sqrt(run.sumSquares / static_cast<double>(run.sampleCount))
                                    : 0.0,
                run.peak);

    return run;
}

void ReportRun(const char* tag, const CaptureRun& run, int sampleRateHz, int& failures) {
    const double deliveredFps =
        run.wallMs > 0.0 ? static_cast<double>(run.frames) / (run.wallMs / 1000.0) : 0.0;
    std::printf("%s packets=%lld timeouts=%lld silent=%lld discont=%lld frames=%lld "
                "max_packet_frames=%lld delivered_fps=%.1f engine_fps=%d\n",
                tag, static_cast<long long>(run.packets), static_cast<long long>(run.timeouts),
                static_cast<long long>(run.silentPackets),
                static_cast<long long>(run.discontinuities), static_cast<long long>(run.frames),
                static_cast<long long>(run.maxPacketFrames), deliveredFps, sampleRateHz);
    std::printf("%s device_position %lld -> %lld (advance %lld) qpc_vs_read_max_us=%lld "
                "cpu_ms=%.0f wall_ms=%.0f\n",
                tag, static_cast<long long>(run.firstDevicePosition),
                static_cast<long long>(run.lastDevicePosition),
                run.firstDevicePosition >= 0 ? run.lastDevicePosition - run.firstDevicePosition : 0,
                static_cast<long long>(run.qpcDeltaAbsMaxUs), run.cpuMs, run.wallMs);

    if (run.packets == 0) {
        // 一条包都没有：可能是设备静音（回环的正常状态），也可能是设备没了。
        // 数量对不对这件事留给出包的那一路证明，这里不假装通过。
        std::printf("%s NO_PACKETS（回环在参考设备静音时本来就不出包）\n", tag);
        return;
    }

    // 设备位置是「到本包末尾一共过了多少帧」，所以 最后一包位置 - 第一包位置 + 第一包帧数
    // 必须正好等于收到的总帧数。对不上就是位置的含义记错了，或者中间真的丢了数据。
    const std::int64_t expectedFrames =
        run.lastDevicePosition - run.firstDevicePosition + run.firstPacketFrames;
    if (expectedFrames != run.frames) {
        std::printf("%s FAIL 设备位置算出的帧数=%lld 与实际收到的=%lld 不一致\n", tag,
                    static_cast<long long>(expectedFrames), static_cast<long long>(run.frames));
        ++failures;
    }

    // 出包的那一路必须按引擎速率供帧，偏差 2% 以内；这是「音频会不会慢慢漂」的硬指标。
    const double ratio = deliveredFps / static_cast<double>(sampleRateHz);
    if (!(ratio > 0.98 && ratio < 1.02)) {
        std::printf("%s FAIL 供帧速率偏离引擎速率：%.1f / %d\n", tag, deliveredFps, sampleRateHz);
        ++failures;
    }
}

void ProbeContract(WasapiAudioSource& source, int& failures) {
    // 借来的包不还就想再等：seam 必须拒掉，而不是静默地把上一包挤掉。
    core::AudioChunk first{};
    if (!source.WaitForPacket(200, first)) {
        std::printf("CONTRACT=SKIPPED（这台机器现在连一个包都拿不到，kind=%s）\n",
                    source.LastErrorKind());
        return;
    }

    core::AudioChunk second{};
    const bool secondAllowed = source.WaitForPacket(50, second);
    std::printf("CONTRACT_DOUBLE_WAIT allowed=%d kind=%s EXPECT=0\n", secondAllowed ? 1 : 0,
                source.LastErrorKind());
    if (secondAllowed || std::string_view(source.LastErrorKind()) != "invalid") {
        ++failures;
    }

    source.ReleasePacket();

    // 还掉之后必须能继续拿到包。
    core::AudioChunk third{};
    const bool resumed = source.WaitForPacket(500, third);
    std::printf("CONTRACT_AFTER_RELEASE waited=%d kind=%s\n", resumed ? 1 : 0,
                source.LastErrorKind());
    if (resumed) {
        source.ReleasePacket();
    }
}

}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    const std::int64_t durationMs = argc > 1 ? std::atoi(argv[1]) : 3000;

    int failures = 0;
    const double memoryAtStart = PrivateMemoryMb();

    // 先只看两端点的形状；这一步不开流，所以设备被别的程序占着也能拿到格式。
    for (core::AudioSourceKind kind :
         { core::AudioSourceKind::SystemLoopback, core::AudioSourceKind::Microphone }) {
        WasapiAudioSource probe(kind);
        const bool opened = probe.Open();
        std::printf("ENDPOINT[%s] open=%d kind=%s code=0x%08lX name=\"%s\" rate=%d channels=%d "
                    "float=%d subtype=%s\n",
                    kind == core::AudioSourceKind::SystemLoopback ? "loopback" : "mic",
                    opened ? 1 : 0, probe.LastErrorKind(),
                    static_cast<unsigned long>(probe.LastErrorCode()),
                    probe.EndpointNameUtf8().c_str(), probe.Format().sampleRateHz,
                    probe.Format().channelCount, probe.Format().floatSamples ? 1 : 0,
                    probe.SubtypeGuidUtf8().c_str());
        if (opened) {
            std::printf("  buffer_frames=%lld default_period_us=%lld min_period_us=%lld\n",
                        static_cast<long long>(probe.BufferFrames()),
                        static_cast<long long>(probe.DefaultDevicePeriodUs()),
                        static_cast<long long>(probe.MinimumDevicePeriodUs()));
        }
        probe.Close();
    }

    // Initialize 的时序参数：哪种组合被接受、缓冲区被定成多大，全部打出来。
    int loopbackCases = 0;
    {
        int count = 0;
        const WasapiAudioSource::TimingCase* cases = WasapiAudioSource::TimingCases(&count);
        for (int i = 0; i < count; ++i) {
            WasapiAudioSource source(core::AudioSourceKind::SystemLoopback);
            const bool opened = source.OpenWithTimings(cases[i].bufferHns, cases[i].periodicityHns);
            std::printf("TIMING[loopback] %-28s opened=%d kind=%s code=0x%08lX buffer_frames=%lld "
                        "default_period_us=%lld\n",
                        cases[i].name, opened ? 1 : 0, source.LastErrorKind(),
                        static_cast<unsigned long>(source.LastErrorCode()),
                        static_cast<long long>(source.BufferFrames()),
                        static_cast<long long>(source.DefaultDevicePeriodUs()));
            if (opened) {
                ++loopbackCases;
            }
            source.Close();
        }
    }
    if (loopbackCases == 0) {
        std::printf("FAIL 四种时序组合没有一个能打开回环\n");
        ++failures;
    }

    // 回环：桌面此刻没声音是常态，这里的数字就是为了把「静音时到底出不出包」量清楚。
    {
        WasapiAudioSource source(core::AudioSourceKind::SystemLoopback);
        if (source.Open()) {
            const CaptureRun run = RunCapture(source, durationMs, source.Format().sampleRateHz,
                                              failures);
            ReportRun("LOOPBACK", run, source.Format().sampleRateHz, failures);
            std::printf("LOOPBACK next_packet_frames_now=%lld\n",
                        static_cast<long long>(source.NextPacketFrames()));
        } else {
            std::printf("LOOPBACK_OPEN=FAILED kind=%s code=0x%08lX\n", source.LastErrorKind(),
                        static_cast<unsigned long>(source.LastErrorCode()));
            ++failures;
        }
    }

    {
        WasapiAudioSource source(core::AudioSourceKind::Microphone);
        if (source.Open()) {
            // 借还契约只有在真能出包的一路上才测得到：回环静音时第一个包都拿不到，
            // 「没还就想再等」这一条根本没机会执行。
            ProbeContract(source, failures);

            const CaptureRun run = RunCapture(source, durationMs, source.Format().sampleRateHz,
                                              failures);
            ReportRun("MIC", run, source.Format().sampleRateHz, failures);
            // 麦克风的判据只有形状：房间本来就该是安静的，peak 高不等于坏，
            // 但一个包都没有就是设备问题。
            if (run.packets == 0) {
                std::printf("FAIL 麦克风开了流却一个包都没有\n");
                ++failures;
            }
        } else {
            std::printf("MIC_OPEN=FAILED kind=%s code=0x%08lX（没有默认录音设备也是合法结果）\n",
                        source.LastErrorKind(),
                        static_cast<unsigned long>(source.LastErrorCode()));
        }
    }

    // 没打开任何流的时候，包账与缓冲已经证明过；这里只补内存。
    std::printf("PRIVATE_MEMORY_MB start=%.0f end=%.0f\n", memoryAtStart, PrivateMemoryMb());

    if (failures != 0) {
        std::printf("OUTCOME=FAILED checks=%d\n", failures);
        return 1;
    }

    std::printf("OUTCOME=OK\n");
    return 0;
}
