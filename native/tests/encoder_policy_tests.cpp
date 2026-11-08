#include "encoder.h"

#include <cstdio>

namespace {

using halocap::core::DefaultBitrateBitsPerSec;
using halocap::core::FrameGate;
using halocap::core::FramePacer;
using halocap::core::RecordingClock;
using halocap::core::ToReferenceTime100ns;
using halocap::core::VideoEncoderConfig;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s: expected true\n", what);
    }
}

const char* Name(FrameGate::Decision decision) {
    switch (decision) {
    case FrameGate::Decision::Submit: return "Submit";
    case FrameGate::Decision::TooEarly: return "TooEarly";
    case FrameGate::Decision::NonMonotonic: return "NonMonotonic";
    }
    return "?";
}

void CheckDecision(FrameGate::Decision actual, FrameGate::Decision expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %s want %s\n", what, Name(actual), Name(expected));
    }
}

void TestClockBasics() {
    RecordingClock clock;
    CheckEqual(clock.MediaTimeUs(123456), 0, "clock before Start is zero");
    CheckTrue(!clock.Running(), "clock not running before Start");

    clock.Start(1000);
    CheckEqual(clock.MediaTimeUs(4000), 3000, "elapsed since Start");
    CheckEqual(clock.MediaTimeUs(1000), 0, "the Start instant itself is zero");

    // 时钟回拨不能产出负时间戳：样本时间是 100ns 刻度，负数会把写入器直接喂坏。
    CheckEqual(clock.MediaTimeUs(500), 0, "wall clock going backwards clamps at zero");
}

void TestClockPause() {
    RecordingClock clock;
    clock.Start(1000);
    clock.Pause(5000);

    // 暂停期间媒体时间冻结——这就是「暂停不能留下空洞」的全部含义。
    CheckEqual(clock.MediaTimeUs(5000), 4000, "paused at the pause instant");
    CheckEqual(clock.MediaTimeUs(7000), 4000, "frozen while paused");
    CheckEqual(clock.MediaTimeUs(9000), 4000, "still frozen 4s later");
    CheckTrue(clock.Paused(), "reports paused");

    clock.Resume(9000);
    CheckEqual(clock.MediaTimeUs(9000), 4000, "resume does not jump");
    CheckEqual(clock.MediaTimeUs(11000), 6000, "only the unpaused span counts");
    CheckTrue(!clock.Paused(), "reports running again");

    // 重复 Pause 不能再算一段；Resume 没有配对的 Pause 时不该改变任何东西。
    RecordingClock sloppy;
    sloppy.Start(0);
    sloppy.Pause(5000);
    sloppy.Pause(6000);
    sloppy.Resume(7000);
    // 若第二次 Pause 覆盖了起点，这里会变成 6000 而不是 5000。
    CheckEqual(sloppy.MediaTimeUs(7000), 5000, "double pause counts a single span");
    sloppy.Resume(8000);
    CheckEqual(sloppy.MediaTimeUs(8000), 6000, "an extra resume freezes nothing");

    // 重新 Start 就是重新开始一段录制。
    sloppy.Start(20000);
    CheckEqual(sloppy.MediaTimeUs(21000), 1000, "Restart resets the accumulated pauses");
}

void TestGate30FromSource60() {
    FrameGate gate(30);
    CheckEqual(gate.IntervalUs(), 33333, "30 fps interval truncates");

    // 60 帧每秒的来源送进 30 帧闸门：必须是干净的隔一取一，不能出现抖动。
    // 注意 33333 本身是奇数，所以要按序号而不是按数值判奇偶。
    const long long times[] = { 0, 16667, 33333, 50000, 66667, 83333 };
    for (int i = 0; i < 6; ++i) {
        const bool accept = gate.Decide(static_cast<std::int64_t>(times[i])) == FrameGate::Decision::Submit;
        if (i % 2 == 0) {
            CheckTrue(accept, "even slot submits");
        } else {
            CheckTrue(!accept, "odd slot drops");
        }
    }

    CheckEqual(gate.AcceptedCount(), 3, "three accepted out of six");
    CheckEqual(gate.DroppedCount(), 3, "three dropped out of six");
    CheckEqual(gate.AcceptedCount() + gate.DroppedCount(), 6, "every call is accounted");
}

void TestGateEdgeDecisions() {
    FrameGate gate(30);
    CheckDecision(gate.Decide(0), FrameGate::Decision::Submit, "first frame always submits");
    CheckDecision(gate.Decide(33332), FrameGate::Decision::TooEarly,
                  "one microsecond short of an interval drops");
    CheckDecision(gate.Decide(33333), FrameGate::Decision::Submit,
                  "exactly one interval later submits");
    CheckDecision(gate.Decide(33333), FrameGate::Decision::TooEarly,
                  "a repeated timestamp is too early, not a rewind");
    CheckDecision(gate.Decide(1000), FrameGate::Decision::NonMonotonic,
                  "going backwards is its own verdict");

    // 倒退的那一帧不接受，参照时间仍是上一次真正接受的时刻——所以后面接着 33333 还是 TooEarly。
    CheckDecision(gate.Decide(33333), FrameGate::Decision::TooEarly, "the rewind did not move the anchor");
    CheckEqual(gate.AcceptedCount(), 2, "two frames reached the encoder");
    CheckEqual(gate.DroppedCount(), 4, "four calls were turned down");
    CheckEqual(gate.AcceptedCount() + gate.DroppedCount(), 6, "every call is accounted");
}

void TestGateUnlimited() {
    FrameGate gate(0);
    CheckEqual(gate.IntervalUs(), 0, "fps<=0 means no interval");
    CheckDecision(gate.Decide(0), FrameGate::Decision::Submit, "first");
    CheckDecision(gate.Decide(1), FrameGate::Decision::Submit, "1us later still submits when unlimited");
    CheckDecision(gate.Decide(2), FrameGate::Decision::Submit, "and again");
    CheckEqual(gate.DroppedCount(), 0, "unlimited never drops for being early");
}

void TestBitrate() {
    // 0.11 bpp × 帧率，然后夹住。4K30 落在区间内，4K60 顶到上限，小选区托到下限。
    CheckEqual(DefaultBitrateBitsPerSec(3840, 2160, 30), 27371520, "4K30 = 0.11bpp");
    CheckEqual(DefaultBitrateBitsPerSec(3840, 2160, 60), 45000000, "4K60 clamps at the ceiling");
    CheckEqual(DefaultBitrateBitsPerSec(640, 360, 10), 1000000, "tiny selection clamps at the floor");
    CheckEqual(DefaultBitrateBitsPerSec(0, 0, 30), 1000000, "degenerate size uses the floor");
    CheckEqual(DefaultBitrateBitsPerSec(2560, 1440, 30), 12165120, "1440p30");

    VideoEncoderConfig config;
    config.widthPx = 3840;
    config.heightPx = 2160;
    config.targetFps = 30;
    CheckEqual(config.Bitrate(), 27371520, "config with no explicit bitrate derives one");
    config.bitrateBitsPerSec = 8000000;
    CheckEqual(config.Bitrate(), 8000000, "an explicit bitrate wins");
    CheckEqual(config.FrameUs(), 33333, "config frame interval comes from the same rule");
}

void TestFramePacer() {
    FramePacer pacer(30);
    CheckEqual(pacer.IntervalUs(), 33333, "30 fps pacer interval");

    // 采集每 10 ms 一帧（远快于目标）：网格取样应收敛到每秒约 30 帧，
    // 而不是像严格最小间隔那样钉在 40 ms（那是 S2 实测 23.7 fps 的来源）。
    int accepted = 0;
    for (int i = 0; i < 100; ++i) {
        if (pacer.Decide(static_cast<std::int64_t>(i) * 10'000) == FramePacer::Decision::Submit) {
            ++accepted;
        }
    }
    CheckTrue(accepted >= 29 && accepted <= 32, "10 ms cadence converges to about 30 fps");
    CheckEqual(pacer.AcceptedCount(), accepted, "accepted counter matches");
    CheckEqual(pacer.AcceptedCount() + pacer.SkippedCount(), 100,
               "every frame is either accepted or skipped");

    // 60 fps 的内容（16.7 ms 一帧）同样要被取成 30。
    FramePacer sixty(30);
    int acceptedSixty = 0;
    for (int i = 0; i < 60; ++i) {
        if (sixty.Decide(static_cast<std::int64_t>(i) * 16'667) == FramePacer::Decision::Submit) {
            ++acceptedSixty;
        }
    }
    CheckTrue(acceptedSixty >= 28 && acceptedSixty <= 32, "60 fps content is sampled to 30 fps");

    // 采集比目标慢：每一帧都接受（内容就这么多，不能凭空变快）。
    FramePacer slow(30);
    int acceptedSlow = 0;
    for (int i = 0; i < 10; ++i) {
        if (slow.Decide(static_cast<std::int64_t>(i) * 100'000) == FramePacer::Decision::Submit) {
            ++acceptedSlow;
        }
    }
    CheckEqual(acceptedSlow, 10, "slower-than-target content is never skipped");

    // 倒退一律拒。
    FramePacer backwards(30);
    CheckTrue(backwards.Decide(100'000) == FramePacer::Decision::Submit, "first frame accepted");
    CheckTrue(backwards.Decide(50'000) == FramePacer::Decision::NonMonotonic,
              "a backwards timestamp is rejected");

    // targetFps 0 = 不限帧。
    FramePacer unlimited(0);
    int acceptedAll = 0;
    for (int i = 0; i < 10; ++i) {
        if (unlimited.Decide(static_cast<std::int64_t>(i) * 1'000) == FramePacer::Decision::Submit) {
            ++acceptedAll;
        }
    }
    CheckEqual(acceptedAll, 10, "unlimited accepts every frame");
}

void TestReferenceTime() {    CheckEqual(ToReferenceTime100ns(0), 0, "zero");
    CheckEqual(ToReferenceTime100ns(1), 10, "one microsecond is ten ticks");
    CheckEqual(ToReferenceTime100ns(33333), 333330, "a 30fps frame");
    CheckEqual(ToReferenceTime100ns(3600000000), 36000000000, "one hour in ticks");
}

}

int main() {
    TestClockBasics();
    TestClockPause();
    TestGate30FromSource60();
    TestGateEdgeDecisions();
    TestGateUnlimited();
    TestFramePacer();
    TestBitrate();
    TestReferenceTime();

    if (failures != 0) {
        std::printf("ENCODER_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("ENCODER_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
