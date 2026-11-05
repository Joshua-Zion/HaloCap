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

struct Blit {
    ImageHandle image = 0;
    Rect source;
    Rect destination;
    float opacity = 1.0f;
};

// 折线（Path）与字形 run（GlyphRun）尚未进入显示列表：前者要等几何缓存策略，
// 后者要等 IFontRasterizer 的图集。放进列表却画不出来比不放更糟。
using Item = std::variant<FillRect, StrokeRect, Segment, Blit>;

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

}
