#include "frame_scheduler.h"

namespace halocap::core {

std::int64_t FrameIntervalUs(int targetFps) {
    if (targetFps <= 0) {
        return 0;
    }
    return 1000000 / targetFps;
}

void FrameScheduler::Start(int targetFps, std::int64_t nowUs) {
    targetFps_ = targetFps;
    intervalUs_ = FrameIntervalUs(targetFps);
    nextDeadlineUs_ = nowUs;
    currentDeadlineUs_ = nowUs;
    lastStartedUs_ = -1;
    frameOpen_ = false;
    stats_ = FrameStats{};
}

bool FrameScheduler::BeginFrame(std::int64_t nowUs) {
    if (intervalUs_ > 0) {
        if (nowUs < nextDeadlineUs_) {
            return false;
        }

        // 一次领一帧：落后的若干个槽直接记成 missed，不靠连发把画面追回来——
        // 覆盖层连发只会让屏幕上看到的一次画面更旧。
        const std::int64_t slots = (nowUs - nextDeadlineUs_) / intervalUs_ + 1;
        stats_.expectedFrames += slots;
        stats_.missedFrames += slots - 1;
        nextDeadlineUs_ += slots * intervalUs_;
    } else {
        ++stats_.expectedFrames;
    }

    if (lastStartedUs_ >= 0) {
        const std::int64_t gap = nowUs - lastStartedUs_;
        if (gap > stats_.longestGapUs) {
            stats_.longestGapUs = gap;
        }
    }

    lastStartedUs_ = nowUs;
    currentDeadlineUs_ = nowUs;
    ++stats_.startedFrames;
    frameOpen_ = true;
    return true;
}

void FrameScheduler::EndFrame(std::int64_t nowUs, bool presented) {
    if (!frameOpen_) {
        return;
    }

    frameOpen_ = false;

    if (!presented) {
        ++stats_.droppedPresents;
        return;
    }

    // 不限帧时没有「预算」可判。
    if (intervalUs_ > 0 && nowUs - currentDeadlineUs_ > intervalUs_) {
        ++stats_.overBudgetFrames;
    }
}

}
