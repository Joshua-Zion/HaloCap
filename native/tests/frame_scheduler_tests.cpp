#include "frame_scheduler.h"

#include <cstdio>

namespace {

using halocap::core::FrameScheduler;

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void TestInterval() {
    CheckEqual(halocap::core::FrameIntervalUs(60), 16666, "60fps interval");
    CheckEqual(halocap::core::FrameIntervalUs(30), 33333, "30fps interval");
    CheckEqual(halocap::core::FrameIntervalUs(0), 0, "unthrottled interval is zero");
    CheckEqual(halocap::core::FrameIntervalUs(-1), 0, "negative fps is unthrottled");
}

void TestSteadyPacing() {
    FrameScheduler scheduler;
    scheduler.Start(60, 0);

    int accepted = 0;
    for (int i = 0; i < 10; ++i) {
        const long long now = static_cast<long long>(i) * 16666;
        if (scheduler.BeginFrame(now)) {
            scheduler.EndFrame(now + 5000, true);
            ++accepted;
        }
    }

    CheckEqual(accepted, 10, "steady: every slot accepted");
    CheckEqual(scheduler.Stats().expectedFrames, 10, "steady: expected counts slots");
    CheckEqual(scheduler.Stats().missedFrames, 0, "steady: nothing missed");
    CheckEqual(scheduler.Stats().overBudgetFrames, 0, "steady: 5ms inside a 16.6ms budget");
}

void TestNotDueYetIsRefused() {
    FrameScheduler scheduler;
    scheduler.Start(60, 0);
    Check(scheduler.BeginFrame(0), "first frame at t0");
    scheduler.EndFrame(1000, true);

    Check(!scheduler.BeginFrame(5000), "before the next slot it refuses");
    CheckEqual(scheduler.Stats().startedFrames, 1, "refused call starts nothing");
    CheckEqual(scheduler.Stats().expectedFrames, 1, "refused call counts nothing");
}

void TestLateCatchUp() {
    FrameScheduler scheduler;
    scheduler.Start(60, 0);
    Check(scheduler.BeginFrame(0), "first frame");
    scheduler.EndFrame(1000, true);

    // 卡了整整一秒：只画一帧，其余的槽位算丢帧，而不是连发补回来。
    Check(scheduler.BeginFrame(1000000), "a second later it accepts one frame");
    scheduler.EndFrame(1001000, true);

    CheckEqual(scheduler.Stats().startedFrames, 2, "late: two frames actually drawn");
    CheckEqual(scheduler.Stats().expectedFrames, 61, "late: expected counts the whole second");
    CheckEqual(scheduler.Stats().missedFrames, 59, "late: 59 slots missed");
    CheckEqual(scheduler.Stats().longestGapUs, 1000000, "late: longest gap recorded");
    CheckEqual(scheduler.NextDeadlineUs(), 1016626, "late: next slot pushed past now");
}

void TestOverBudgetAndDroppedPresent() {
    FrameScheduler scheduler;
    scheduler.Start(30, 0);

    Check(scheduler.BeginFrame(0), "frame 1");
    scheduler.EndFrame(40000, true);  // 33.3ms 的预算用了 40ms

    Check(scheduler.BeginFrame(50000), "frame 2");
    scheduler.EndFrame(60000, false);  // Present 失败

    CheckEqual(scheduler.Stats().overBudgetFrames, 1, "one frame over budget");
    CheckEqual(scheduler.Stats().droppedPresents, 1, "one failed present counted");
    CheckEqual(scheduler.Stats().startedFrames, 2, "two frames started");
}

void TestUnthrottled() {
    FrameScheduler scheduler;
    scheduler.Start(0, 1000);

    for (int i = 0; i < 5; ++i) {
        Check(scheduler.BeginFrame(1000 + i), "unthrottled always accepts");
        scheduler.EndFrame(1001 + i, true);
    }

    CheckEqual(scheduler.Stats().expectedFrames, 5, "unthrottled: expected == started");
    CheckEqual(scheduler.Stats().missedFrames, 0, "unthrottled has no slots to miss");
    CheckEqual(scheduler.Stats().overBudgetFrames, 0, "unthrottled never calls a frame late");
}

void TestStrayEndFrameIgnored() {
    FrameScheduler scheduler;
    scheduler.Start(60, 0);

    scheduler.EndFrame(100, true);
    scheduler.EndFrame(200, false);

    CheckEqual(scheduler.Stats().startedFrames, 0, "nothing started");
    CheckEqual(scheduler.Stats().overBudgetFrames, 0, "stray EndFrame not counted as late");
    CheckEqual(scheduler.Stats().droppedPresents, 0, "stray EndFrame not counted as dropped");
}

}

int main() {
    TestInterval();
    TestSteadyPacing();
    TestNotDueYetIsRefused();
    TestLateCatchUp();
    TestOverBudgetAndDroppedPresent();
    TestUnthrottled();
    TestStrayEndFrameIgnored();

    if (failures != 0) {
        std::printf("SCHEDULER_TESTS=FAILED %d\n", failures);
        return 1;
    }

    std::printf("SCHEDULER_TESTS=OK\n");
    return 0;
}
