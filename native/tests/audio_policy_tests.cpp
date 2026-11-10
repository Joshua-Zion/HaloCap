#include "audio_source.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

using halocap::core::ConvertFloatToPcm16;
using halocap::core::FramesToUs;
using halocap::core::MixInto;
using halocap::core::PeakAbs;
using halocap::core::RemixToStereo;
using halocap::core::ResampleLinear;
using halocap::core::ResampledFrameCount;
using halocap::core::SilenceFramesToNextBlock;
using halocap::core::StereoRingBuffer;
using halocap::core::UsToFrames;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckClose(float actual, float expected, float tolerance, const char* what) {
    ++checks;
    if (!(std::fabs(actual - expected) <= tolerance)) {
        ++failures;
        std::printf("FAIL %s: got %.6f want %.6f (±%.6f)\n", what, actual, expected, tolerance);
    }
}

// 一帧一帧好认的值：L=v，R=-v，任何错位都能看出来。
std::vector<float> MakeFrames(int from, int count) {
    std::vector<float> out(static_cast<size_t>(count) * 2);
    for (int i = 0; i < count; ++i) {
        out[i * 2] = static_cast<float>(from + i);
        out[i * 2 + 1] = -static_cast<float>(from + i);
    }
    return out;
}

void TestRingBasics() {
    StereoRingBuffer ring(4);

    CheckEqual(ring.AvailableFrames(), 0, "empty: nothing available");
    std::vector<float> two(4);
    CheckEqual(ring.Pop(two.data(), 2), 0, "empty: pop gives nothing");

    ring.Push(MakeFrames(0, 4).data(), 4);
    CheckEqual(ring.AvailableFrames(), 4, "full: capacity frames");
    CheckEqual(ring.DroppedFrames(), 0, "full to the exact capacity drops nothing");
    CheckEqual(ring.PushedFrames(), 4, "pushed counter");

    std::vector<float> out(8, -999.0f);
    CheckEqual(ring.Pop(out.data(), 2), 2, "pop two");
    CheckClose(out[0], 0.0f, 0.0f, "pop order first L");
    CheckClose(out[2], 1.0f, 0.0f, "pop order second L");
    CheckEqual(ring.AvailableFrames(), 2, "two left after popping two");
}

void TestRingDropOldest() {
    StereoRingBuffer ring(4);

    ring.Push(MakeFrames(0, 6).data(), 6);
    CheckEqual(ring.AvailableFrames(), 4, "overflow: still capacity");
    CheckEqual(ring.DroppedFrames(), 2, "overflow: the two oldest are gone");

    std::vector<float> out(8, -999.0f);
    CheckEqual(ring.Pop(out.data(), 4), 4, "overflow: pop all");
    CheckClose(out[0], 2.0f, 0.0f, "overflow: newest window starts at 2");
    CheckClose(out[6], 5.0f, 0.0f, "overflow: newest window ends at 5");
    CheckClose(out[7], -5.0f, 0.0f, "overflow: right channel kept with the left");
}

void TestRingWrapsAround() {
    StereoRingBuffer ring(4);

    ring.Push(MakeFrames(0, 3).data(), 3);
    std::vector<float> drained(4);
    ring.Pop(drained.data(), 2);   // 腾出两格，写指针靠近尾部

    ring.Push(MakeFrames(3, 3).data(), 3);
    CheckEqual(ring.AvailableFrames(), 4, "wrap: full again");
    CheckEqual(ring.DroppedFrames(), 0, "wrap: nothing dropped, there was room");

    std::vector<float> out(8, -999.0f);
    CheckEqual(ring.Pop(out.data(), 4), 4, "wrap: pop across the end of the array");
    // 剩下的一帧是 2，然后新来的 3、4、5 按顺序接上。
    CheckClose(out[0], 2.0f, 0.0f, "wrap: oldest is 2");
    CheckClose(out[2], 3.0f, 0.0f, "wrap: next is 3");
    CheckClose(out[4], 4.0f, 0.0f, "wrap: next is 4");
    CheckClose(out[6], 5.0f, 0.0f, "wrap: newest is 5");
}

void TestRingDegenerate() {
    StereoRingBuffer zero(0);
    zero.Push(MakeFrames(0, 5).data(), 5);
    CheckEqual(zero.AvailableFrames(), 0, "capacity 0: nothing ever available");
    CheckEqual(zero.DroppedFrames(), 5, "capacity 0: everything is dropped");
    std::vector<float> out(2, 1.0f);
    CheckEqual(zero.Pop(out.data(), 1), 0, "capacity 0: pop is safe");
    CheckClose(out[0], 1.0f, 0.0f, "capacity 0: pop wrote nothing");

    StereoRingBuffer ring(4);
    ring.Push(MakeFrames(0, 10).data(), 10);   // 一次来的比整个缓冲大
    CheckEqual(ring.DroppedFrames(), 6, "one huge push: front six dropped");
    CheckEqual(ring.AvailableFrames(), 4, "one huge push: newest four kept");
    std::vector<float> hugeOut(8, -999.0f);
    ring.Pop(hugeOut.data(), 4);
    CheckClose(hugeOut[0], 6.0f, 0.0f, "one huge push: window starts at 6");

    ring.Push(nullptr, 4);
    ring.Push(MakeFrames(0, 1).data(), 0);
    CheckEqual(ring.PushedFrames(), 10, "null/empty pushes change nothing");

    ring.Reset();
    CheckEqual(ring.AvailableFrames(), 0, "reset: empty");
    CheckEqual(ring.DroppedFrames(), 0, "reset: counters cleared");
    CheckEqual(ring.PushedFrames(), 0, "reset: pushed cleared");
}

void TestMix() {
    std::vector<float> dst = { 0.5f, -0.5f, 0.0f, 0.0f };
    const std::vector<float> src = { 0.4f, 0.4f, 0.2f, 0.2f };

    MixInto(dst.data(), src.data(), 2, 1.0f);
    CheckClose(dst[0], 0.9f, 1e-6f, "mix gain 1");
    CheckClose(dst[1], -0.1f, 1e-6f, "mix gain 1 right channel");
    CheckClose(dst[2], 0.2f, 1e-6f, "mix from zero");

    std::vector<float> hot = { 0.8f, -0.8f };
    MixInto(hot.data(), src.data(), 1, 1.0f);
    CheckClose(hot[0], 1.0f, 0.0f, "mix saturates upward at +1");
    CheckClose(hot[1], -0.4f, 1e-6f, "only the channel that crossed the bound gets clamped");

    std::vector<float> negative = { -0.8f, 0.8f };
    const std::vector<float> loud = { -0.6f, 0.6f };
    MixInto(negative.data(), loud.data(), 1, 1.0f);
    CheckClose(negative[0], -1.0f, 0.0f, "mix saturates downward at -1");

    std::vector<float> quiet = { 0.0f, 0.0f };
    MixInto(quiet.data(), src.data(), 1, 0.5f);
    CheckClose(quiet[0], 0.2f, 1e-6f, "gain applies to the source, not the destination");

    // 什么都不该动，也不该崩。
    MixInto(nullptr, src.data(), 1, 1.0f);
    MixInto(quiet.data(), nullptr, 1, 1.0f);
    MixInto(quiet.data(), src.data(), 0, 1.0f);
    CheckClose(quiet[0], 0.2f, 1e-6f, "degenerate mix calls leave the buffer alone");
}

void TestRemix() {
    {
        const std::vector<float> mono = { 0.3f, -0.2f, 0.5f };
        std::vector<float> out(6, 0.0f);
        RemixToStereo(out.data(), mono.data(), 3, 1);
        CheckClose(out[0], 0.3f, 1e-6f, "mono: left gets it");
        CheckClose(out[1], 0.3f, 1e-6f, "mono: right gets the same");
        CheckClose(out[4], 0.5f, 1e-6f, "mono: third frame L");
        CheckClose(out[5], 0.5f, 1e-6f, "mono: third frame R");
    }
    {
        const std::vector<float> stereo = { 1.0f, 2.0f, 3.0f, 4.0f };
        std::vector<float> out(4, 0.0f);
        RemixToStereo(out.data(), stereo.data(), 2, 2);
        CheckEqual(out[0] == 1.0f && out[1] == 2.0f && out[2] == 3.0f && out[3] == 4.0f, 1,
                   "stereo passes through");
    }
    {
        // 5.1：L R C LFE Ls Rs —— 前两路留下，其余丢掉（1.x 就是这个行为）。
        std::vector<float> surround;
        for (int i = 0; i < 2; ++i) {
            surround.insert(surround.end(), { 10.0f + i, 20.0f + i, 30.0f, 40.0f, 50.0f, 60.0f });
        }
        std::vector<float> out(4, 0.0f);
        RemixToStereo(out.data(), surround.data(), 2, 6);
        CheckEqual(out[0] == 10.0f && out[1] == 20.0f, 1, "5.1 frame 0 takes L R");
        CheckEqual(out[2] == 11.0f && out[3] == 21.0f, 1, "5.1 frame 1 takes L R");
    }
    {
        std::vector<float> out(2, 7.0f);
        RemixToStereo(out.data(), MakeFrames(0, 1).data(), 1, 0);
        CheckEqual(out[0] == 0.0f && out[1] == 0.0f, 1, "channel count 0 goes silent");
    }
}

void TestResample() {
    CheckEqual(ResampledFrameCount(1000, 1.0), 1000, "ratio 1 keeps the count");
    CheckEqual(ResampledFrameCount(1000, 2.0), 2000, "doubling");
    CheckEqual(ResampledFrameCount(1000, 0.5), 500, "halving");
    CheckEqual(ResampledFrameCount(44100, 48000.0 / 44100.0), 48000, "44.1k to 48k one second");
    CheckEqual(ResampledFrameCount(0, 2.0), 0, "no source no output");
    CheckEqual(ResampledFrameCount(100, 0.0), 0, "ratio 0 is not a resample");

    // 直流必须原样出来：任何 off-by-one 的取样位置都会让它变成别的值。
    const std::vector<float> dc(200, 0.25f);
    std::vector<float> out(ResampledFrameCount(100, 48000.0 / 44100.0) * 2, 0.0f);
    const int outFrames = ResampledFrameCount(100, 48000.0 / 44100.0);
    ResampleLinear(dc.data(), 100, 2, 48000.0 / 44100.0, out.data(), outFrames);
    bool allSame = true;
    for (const float value : out) {
        allSame = allSame && std::fabs(value - 0.25f) < 1e-6f;
    }
    CheckEqual(allSame ? 1 : 0, 1, "DC survives resampling");

    // 斜坡上取样位置对不对：源帧 0..3 = 0,1,2,3；ratio 2 → 目标 8 帧，d=1 落在 0.5。
    const std::vector<float> ramp = { 0.0f, 0.0f, 1.0f, 1.0f, 2.0f, 2.0f, 3.0f, 3.0f };
    // 8 帧 × 2 声道 = 16 个 float：上一版按帧数开了 8 个，越界写把调试堆打崩在这里。
    std::vector<float> up(16, -9.0f);
    ResampleLinear(ramp.data(), 4, 2, 2.0, up.data(), 8);
    CheckClose(up[0], 0.0f, 1e-6f, "upsample d0");
    CheckClose(up[2], 0.5f, 1e-6f, "upsample d1 interpolates halfway");
    CheckClose(up[6], 1.5f, 1e-6f, "upsample d3");
    CheckClose(up[14], 3.0f, 1e-6f, "upsample last clamps at the last source frame");

    std::vector<float> down(4, -9.0f);
    ResampleLinear(ramp.data(), 4, 2, 0.5, down.data(), 2);
    CheckClose(down[0], 0.0f, 1e-6f, "downsample d0");
    CheckClose(down[2], 2.0f, 1e-6f, "downsample d1 lands on source frame 2");

    // 不该崩也不该写坏目标。
    ResampleLinear(nullptr, 4, 2, 1.0, down.data(), 2);
    ResampleLinear(ramp.data(), 4, 0, 1.0, down.data(), 2);
    CheckClose(down[0], 0.0f, 1e-6f, "degenerate resample calls leave the buffer alone");
}

void TestPeakAbs() {
    // 峰值表最关心的是「取绝对值后的最大值」，左右声道都要扫到。
    const float quiet[] = { 0.1f, -0.3f, 0.2f, -0.15f };
    CheckClose(PeakAbs(quiet, 2), 0.3f, 1e-6f, "peak: right channel negative wins");

    const float loudRight[] = { 0.05f, -0.8f, 0.4f, 0.2f };
    CheckClose(PeakAbs(loudRight, 2), 0.8f, 1e-6f, "peak: loudest sample on the right");

    // 超过满幅的样本要夹到 1.0：电平条宽度是 sqrt(level)，>1 会画出格子外面。
    const float hot[] = { 1.5f, -2.0f, 0.3f, 0.0f };
    CheckClose(PeakAbs(hot, 2), 1.0f, 0.0f, "peak: over full scale clamps to 1");

    // 空指针/非正帧数返回 0，调用方在音频泵里直接传裸参数。
    CheckClose(PeakAbs(nullptr, 4), 0.0f, 0.0f, "peak: null is silent");
    CheckClose(PeakAbs(quiet, 0), 0.0f, 0.0f, "peak: no frames is silent");
    CheckClose(PeakAbs(quiet, -3), 0.0f, 0.0f, "peak: negative frames is silent");

    // 全零就是零，不该被任何偏置抬起来。
    const float silence[] = { 0.0f, 0.0f, 0.0f, 0.0f };
    CheckClose(PeakAbs(silence, 2), 0.0f, 0.0f, "peak: silence is zero");
}

void TestBlockPadding() {
    CheckEqual(SilenceFramesToNextBlock(0), 0, "block aligned needs no padding");
    CheckEqual(SilenceFramesToNextBlock(1024), 0, "exactly one block");
    CheckEqual(SilenceFramesToNextBlock(1000), 24, "1000 pads 24");
    CheckEqual(SilenceFramesToNextBlock(2049), 1023, "one block plus one frame");
    CheckEqual(SilenceFramesToNextBlock(1024 * 47, 1024), 0, "2 seconds of 48k is block aligned");
    CheckEqual(SilenceFramesToNextBlock(5, 8), 3, "arbitrary block size");
    CheckEqual(SilenceFramesToNextBlock(-5), 0, "negative total is nothing");
    CheckEqual(SilenceFramesToNextBlock(100, 0), 0, "block size 0 is nothing");
}

void TestTimeConversion() {
    CheckEqual(FramesToUs(48000, 48000), 1000000, "one second of 48k is a second");
    CheckEqual(UsToFrames(1000000, 48000), 48000, "and back");
    CheckEqual(FramesToUs(1440, 48000), 30000, "one AAC block at 48k is 30 ms");
    CheckEqual(UsToFrames(20000000, 44100), 882000, "20 seconds at 44.1k");
    CheckEqual(FramesToUs(0, 48000), 0, "zero frames");
    CheckEqual(UsToFrames(1, 48000), 0, "one microsecond is not a whole frame");
    CheckEqual(FramesToUs(48000, 0), 0, "rate 0 does not divide");
    CheckEqual(UsToFrames(1000000, -1), 0, "negative rate does not multiply");

    // 一小时不换算是 §4「两条流共用一个录制时钟」的前提。
    CheckEqual(FramesToUs(static_cast<long long>(48000) * 3600, 48000), 3600000000LL,
               "one hour round trips");

    // 来回换算允许差一帧，但不许差出第二帧——否则长录制的漂移就是这里来的。
    int worstError = 0;
    for (long long frames = 0; frames < 48000 * 60; frames += 480) {
        const long long back = UsToFrames(FramesToUs(frames, 48000), 48000);
        const long long error = back > frames ? back - frames : frames - back;
        if (error > worstError) {
            worstError = static_cast<int>(error);
        }
    }
    CheckEqual(worstError, 0, "frames->us->frames stays exact at 48k because 1e6/48k truncates down");
}

void TestPcmConversion() {
    // 16 位 PCM 的满幅是 32767：用 ±1.0 → ±32767 才是对称的，-32768 会听成爆音。
    const float samples[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 2.0f, -2.0f, 0.000001f };
    std::int16_t converted[8] = {};
    ConvertFloatToPcm16(samples, 8, converted);

    CheckEqual(converted[0], 0, "zero stays zero");
    CheckEqual(converted[1], 32767, "full scale positive clamps to 32767");
    CheckEqual(converted[2], -32767, "full scale negative clamps to -32767");
    CheckEqual(converted[3], 16384, "half scale rounds to 16384");
    CheckEqual(converted[4], -16384, "negative half scale");
    CheckEqual(converted[5], 32767, "over range positive is clamped");
    CheckEqual(converted[6], -32767, "over range negative is clamped");
    CheckEqual(converted[7], 0, "a tiny positive sample rounds to zero");

    // 空指针与 0 样本都不该崩：调用方在音频泵里会直接用这两个参数。
    ConvertFloatToPcm16(nullptr, 8, converted);
    ConvertFloatToPcm16(samples, 0, nullptr);
    CheckEqual(converted[0], 0, "null destination leaves the buffer alone");
}

}

int main() {
    // 关掉缓冲：这个测试真会崩（第一次就是这样），行缓冲的日志会连同现场一起丢。
    setvbuf(stdout, nullptr, _IONBF, 0);

    TestRingBasics();
    TestRingDropOldest();
    TestRingWrapsAround();
    TestRingDegenerate();
    TestMix();
    TestRemix();
    TestResample();
    TestPeakAbs();
    TestBlockPadding();
    TestTimeConversion();
    TestPcmConversion();

    if (failures != 0) {
        std::printf("AUDIO_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("AUDIO_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
