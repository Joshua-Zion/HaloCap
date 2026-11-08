#pragma once

#include "geometry.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace halocap::core {

// 裸缓冲的唯一形态：core 只看得到「指针 + stride + 尺寸」，映射与上传归平台（§3.3 规则二）。
struct ImageRef {
    const void* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

// 显示列表不携带裸缓冲：帧循环里不做上传，所以图元只引用后端注册后的句柄。
using ImageHandle = int;

struct FillRect {
    Rect rect;
    Color color;
};

struct StrokeRect {
    Rect rect;
    Color color;
    float thickness = 1.0f;
};

// 直线段：箭头、画笔笔画的落点、选区边框都落在这里。
struct Segment {
    Point from;
    Point to;
    Color color;
    float thickness = 1.0f;
};

// 椭圆（描边/填充）：1.x 的矩形/椭圆标注与序号圆都在这里。加进列表而不是用折线模拟，
// 是因为 D2D 的 DrawEllipse 是原生抗锯齿椭圆，32 段折线在 4K 上仍能看出多边形。
struct FillEllipse {
    Rect rect;
    Color color;
};

struct StrokeEllipse {
    Rect rect;
    Color color;
    float thickness = 1.0f;
};

struct Blit {
    ImageHandle image = 0;
    Rect source;
    Rect destination;
    float opacity = 1.0f;
};

// 折线（Path）与字形 run（GlyphRun）尚未进入显示列表：前者要等几何缓存策略，
// 后者要等 IFontRasterizer 的图集。放进列表却画不出来比不放更糟。
using Item = std::variant<FillRect, StrokeRect, FillEllipse, StrokeEllipse, Segment, Blit>;

// core 只产显示列表，向量顺序即图层顺序，不触碰任何渲染 API（§3.3 规则一）。
class FramePlan {
public:
    void Clear();

    void Add(Item item);

    const std::vector<Item>& Items() const { return items_; }

    std::size_t ItemCount() const { return items_.size(); }

    Rect ContentBounds() const;

private:
    std::vector<Item> items_;
};

Rect BoundsOf(const Item& item);

// 只重绘变化区时必须先在 core 侧丢掉脏区外的图元：只在设备上压一个 clip，
// 命令提交与逐图元裁剪测试的成本一分都不省（§5.1 原生复测第 3 条）。
// bounds 为空矩形表示整面重绘，原样全部追加。
void AppendIntersecting(const FramePlan& source, Rect bounds, FramePlan& destination);

}
