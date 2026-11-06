#pragma once

#include "display_list.h"
#include "geometry.h"

#include <cstdint>
#include <vector>

namespace halocap::core {

// 一次采集更新。
// contentChanged 就等于 AccumulatedFrames > 0：spike 2 实测 8 秒里唤醒 1215 次，
// 其中 1068 次只是鼠标在动。不过滤就会被空帧拖着走，所以这一位是调用方的主判据。
struct FrameUpdate {
    std::int64_t timestampUs = 0;
    std::int64_t accumulatedFrames = 0;
    bool contentChanged = false;
    bool rectsCoalesced = false;

    // 这个输出自己的像素坐标（左上角是 0,0）；换成桌面坐标要加上 desktopBounds 的左上角。
    // target 已按这些矩形更新过，所以脏区之外不必再动。
    std::vector<IntRect> dirtyRects;

    // 常驻纹理在渲染后端的资源表里的句柄（§3.3 规则二：core 只看得到句柄与尺寸）。
    ImageHandle target = 0;
    IntRect desktopBounds;
    int widthPx = 0;
    int heightPx = 0;
};

// 8 个 seam 里的 ICaptureSource（§3.2）：帧以纹理交付，含脏矩形。
// 复制权被占用（E_ACCESSDENIED）是常态不是异常——桌面切换、UAC、别的采集者都会碰到，
// 用 LastErrorKind 区分，调用方据此退避而不是报错给用户。
class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;

    // outputIndex 是平台枚举出来的输出序号；设备与适配器的配对由平台侧负责。
    virtual bool Open(int outputIndex) = 0;

    virtual void Close() = 0;

    // timeoutMs 内没有新帧返回 false（这不是错误）。拿到帧时 out 一定填好。
    virtual bool WaitForUpdate(std::int64_t timeoutMs, FrameUpdate& out) = 0;

    // "ok" / "timeout" / "busy" / "lost" / "invalid" / "none"
    virtual const char* LastErrorKind() const = 0;

    virtual IntRect DesktopBounds() const = 0;
};

// 脏矩形的面积统计。sumAreaPx 才是要搬运的量；boundingBoxAreaPx 在静态桌面上
// 经常比实际大好几个数量级，所以两个都要看，不能只报包围盒。
struct DirtySummary {
    std::int64_t rectCount = 0;
    std::int64_t sumAreaPx = 0;
    std::int64_t boundingBoxAreaPx = 0;
    IntRect boundingBox;
};

DirtySummary SummarizeDirtyRects(const std::vector<IntRect>& rects);

// 退避序列：第 attempt 次（从 0 起）要等多久。封顶是因为 spike 2 实测失败能持续几分钟，
// 不封顶会越退越久、恢复也慢。
std::int64_t BackoffUs(int attempt, std::int64_t baseUs, std::int64_t ceilingUs);

}
