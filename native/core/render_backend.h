#pragma once

#include "display_list.h"
#include "geometry.h"
#include "surface_window.h"

namespace halocap::core {

// 8 个 seam 里的 IRenderBackend（docs/2.0-native-plan.md §3.2）：消费显示列表，落到原生 2D API。
// 一次提交整张列表，core 不逐图元调 API；Render 里不许分配，资源在 Resize/RegisterImage 就绪。
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // 目标尺寸（设备像素）。尺寸变化才重建目标，帧循环内不调用。
    virtual bool Resize(int widthPx, int heightPx) = 0;

    // 裸缓冲在这里变成后端资源；句柄生命周期由调用方掌握。
    virtual ImageHandle RegisterImage(const ImageRef& image) = 0;

    virtual void ReleaseImage(ImageHandle handle) = 0;

    // clip 是脏区（设备像素）；空矩形表示整面重绘。
    virtual void Render(const FramePlan& plan, IntRect clip) = 0;

    // 把排队的绘制真正推给 GPU，测量与「一帧结束」都用它。
    virtual void Flush() = 0;

    // 上屏路径：把绘制目标从离屏纹理换成 DirectComposition 的合成链（预乘 alpha）。
    // Attach 之后 Render 画的就是屏幕上那块；Detach 回到离屏纹理。
    virtual bool AttachWindow(NativeHandle handle, int widthPx, int heightPx) = 0;

    virtual void DetachWindow() = 0;

    // 把这一帧交给合成器。节奏不在这里做——见 WaitForFrameSlot。
    virtual void Present() = 0;

    // 画之前等一个帧槽——在 Present 之后等是量不到 vblank 的。
    // 没有 waitable object（比如离屏）时立刻返回，调用方不需要分支。
    virtual void WaitForFrameSlot() = 0;
};

}
