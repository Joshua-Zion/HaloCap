#pragma once

#include <cstdint>

namespace halocap::core {

// 目标帧率的间隔，微秒；targetFps <= 0 表示不限帧（返回 0）。
std::int64_t FrameIntervalUs(int targetFps);

struct FrameStats {
    std::int64_t expectedFrames = 0;   // 按时间轴本该有多少帧
    std::int64_t startedFrames = 0;    // 真的开始画了多少帧
    std::int64_t droppedPresents = 0;  // 画了但没提交成功
    std::int64_t overBudgetFrames = 0; // 画完时已经超过一个帧间隔
    std::int64_t missedFrames = 0;     // expected - started，即卡住时丢掉的槽位
    std::int64_t longestGapUs = 0;     // 相邻两帧开始时刻的最大间隔
};

// 覆盖层与录制的节奏都要自己给：实测 DXGI 合成链的 Present(1) 与帧槽等待对象都不节流
// （独立翻转），所以「不丢帧」只能按「已提交 / 应有」这个口径统计。
//
// 时钟由调用方注入（微秒），因此这块逻辑完全可测，也不碰任何平台 API。
// 用法是 BeginFrame → 画 → EndFrame，没到钟时 BeginFrame 返回 false，调用方自己休眠或让出线程。
class FrameScheduler {
public:
    void Start(int targetFps, std::int64_t nowUs);

    // 一次调用最多领一帧；追不回来的槽位记成 missed，不靠连发补帧。
    bool BeginFrame(std::int64_t nowUs);

    void EndFrame(std::int64_t nowUs, bool presented);

    const FrameStats& Stats() const { return stats_; }

    std::int64_t NextDeadlineUs() const { return nextDeadlineUs_; }

    std::int64_t IntervalUs() const { return intervalUs_; }

private:
    int targetFps_ = 0;
    std::int64_t intervalUs_ = 0;
    std::int64_t nextDeadlineUs_ = 0;
    std::int64_t currentDeadlineUs_ = 0;
    std::int64_t lastStartedUs_ = -1;
    bool frameOpen_ = false;
    FrameStats stats_{};
};

}
