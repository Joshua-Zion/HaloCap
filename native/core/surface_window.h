#pragma once

namespace halocap::core {

// core 不解释它，只负责透传：这是 ISurfaceWindow 与 IRenderBackend 的交接点。
using NativeHandle = void*;

struct WindowBounds {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// 四个开关都对应 1.x 已经存在的窗口形态，不是预留设计：
// 选区覆盖层 = 不可聚焦 + 吃鼠标；HUD/贴图边框 = 不可聚焦 + 透传；贴图窗口 = 可聚焦。
struct WindowOptions {
    bool visible = true;
    bool focusable = false;
    bool clickThrough = false;
    bool topMost = true;
};

// 8 个 seam 里的 ISurfaceWindow（§3.2）：透明全屏覆盖层、逐像素命中、不抢焦点。
// 呈现不在这里——那一帧的位图归 IRenderBackend，本接口只管窗口本身。
class ISurfaceWindow {
public:
    virtual ~ISurfaceWindow() = default;

    virtual bool Create(const WindowBounds& bounds, const WindowOptions& options) = 0;

    virtual bool SetBounds(const WindowBounds& bounds) = 0;

    virtual void SetVisible(bool visible) = 0;

    // 选区阶段要吃鼠标，标注阶段要透传，所以这是运行期可切的。
    virtual void SetClickThrough(bool clickThrough) = 0;

    virtual WindowBounds Bounds() const = 0;

    virtual NativeHandle Handle() const = 0;
};

}
