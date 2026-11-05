#pragma once

#include "geometry.h"

#include <cstdint>
#include <variant>
#include <vector>

namespace halocap::core {

// 规则三：core 只看到「指针 + stride + 尺寸」，映射与解映射归 platform/。
struct ImageRef {
    const void* pixels = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

struct FillRect {
    Rect rect;
    Color color;
};

struct StrokeRect {
    Rect rect;
    Color color;
    float thickness = 1.0f;
};

struct Blit {
    ImageRef image;
    Rect source;
    Rect destination;
    float opacity = 1.0f;
};

// 字形位置由 IFontRasterizer 产出的 token 表示，core 不做排版与测量；
// bounds 由持有度量的一侧填，文本轮廓在 core 里算不出来。
struct GlyphRun {
    int fontToken = 0;
    Color color;
    Point origin;
    Rect bounds;
    std::vector<int> glyphTokens;
    std::vector<float> advances;
};

struct Path {
    std::vector<Point> points;
    Color color;
    float thickness = 1.0f;
    bool closed = false;
};

using Item = std::variant<FillRect, StrokeRect, Blit, GlyphRun, Path>;

// 规则二：core 只产显示列表，向量顺序即图层顺序，不触碰任何渲染 API。
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
