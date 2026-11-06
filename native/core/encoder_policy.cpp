#include "encoder.h"

#include <algorithm>

namespace halocap::core {

std::int64_t VideoEncoderConfig::Bitrate() const {
    if (bitrateBitsPerSec > 0) {
        return bitrateBitsPerSec;
    }
    return DefaultBitrateBitsPerSec(widthPx, heightPx, targetFps);
}

std::int64_t ToReferenceTime100ns(std::int64_t microUs) { return microUs * 10; }

std::int64_t DefaultBitrateBitsPerSec(int widthPx, int heightPx, int fps) {
    if (widthPx <= 0 || heightPx <= 0 || fps <= 0) {
        return 1000000;
    }
    // 用整数算，避免 float 让同一档位在不同机器上给出不同码率。
    const std::int64_t pixels = static_cast<std::int64_t>(widthPx) * heightPx;
    const std::int64_t bits = pixels * 11 * fps / 100;
    return std::clamp(bits, std::int64_t{1000000}, std::int64_t{45000000});
}

void RecordingClock::Start(std::int64_t wallUs) {
    startWallUs_ = wallUs;
    pausedTotalUs_ = 0;
    pauseBeganWallUs_ = -1;
    started_ = true;
}

void RecordingClock::Pause(std::int64_t wallUs) {
    if (!started_ || pauseBeganWallUs_ >= 0) {
        return;
    }
    pauseBeganWallUs_ = wallUs;
}

void RecordingClock::Resume(std::int64_t wallUs) {
    if (pauseBeganWallUs_ < 0) {
        return;
    }
    pausedTotalUs_ += std::max<std::int64_t>(0, wallUs - pauseBeganWallUs_);
    pauseBeganWallUs_ = -1;
}

std::int64_t RecordingClock::MediaTimeUs(std::int64_t wallUs) const {
    if (!started_) {
        return 0;
    }
    // 暂停中的这一段既不在 pausedTotal 里，也不能算进媒体时间。
    const std::int64_t inFlightPauseUs = pauseBeganWallUs_ >= 0 ? wallUs - pauseBeganWallUs_ : 0;
    const std::int64_t elapsedUs = wallUs - startWallUs_ - pausedTotalUs_ - inFlightPauseUs;
    return std::max<std::int64_t>(0, elapsedUs);
}

FrameGate::FrameGate(int targetFps) : intervalUs_(FrameIntervalUs(targetFps)) {}

FrameGate::Decision FrameGate::Decide(std::int64_t mediaTimeUs) {
    if (mediaTimeUs < lastAcceptedUs_) {
        ++dropped_;
        return Decision::NonMonotonic;
    }

    // interval 为 0 就是不限帧：这时每一帧都该送，倒退之外没有丢帧的理由。
    if (intervalUs_ > 0 && lastAcceptedUs_ >= 0 && mediaTimeUs - lastAcceptedUs_ < intervalUs_) {
        ++dropped_;
        return Decision::TooEarly;
    }

    lastAcceptedUs_ = mediaTimeUs;
    ++accepted_;
    return Decision::Submit;
}

}
