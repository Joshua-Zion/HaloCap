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

// 圆角矩形：1.x 的工具栏/提示条/卡片全是 8–12 px 圆角，直角会明显不像。
struct FillRoundedRect {
    Rect rect;
    float radius = 8.0f;
    Color color;
};

struct StrokeRoundedRect {
    Rect rect;
    float radius = 8.0f;
    Color color;
    float thickness = 1.0f;
};

struct Blit {
    ImageHandle image = 0;
    Rect source;
    Rect destination;
    float opacity = 1.0f;
};

// 填充路径：1.x 的工具栏/选项栏图标就是「160×160 的填充路径」，形状复杂到没法用
// 矩形+线段拼回来（画笔、橡皮、识别框都是曲线轮廓），所以让显示列表直接承载路径。
// 命令集只覆盖 WPF path 数据里用到的部分：M/L/C/Q/Z（H/V/S/T 在解析时展开）。
struct PathCommand {
    enum class Kind : std::uint8_t { Move = 0, Line = 1, Cubic = 2, Quad = 3, Close = 4 };
    Kind kind = Kind::Line;
    Point points[3];
};

struct PathShape {
    std::vector<PathCommand> commands;
    Color color;
    bool evenOdd = false;
    // > 0 时按描边画（圆头圆角），否则填充。画笔/荧光笔用描边：一笔一条路径，
    // 半透明色只叠一次，不会被无数小段叠深、也不会在拐点露缝。
    float strokeWidth = 0.0f;
    // 后端按它缓存 D2D 几何：图标是静态的，位置不变时不该每帧重建。
    int cacheKey = 0;
};
// 折线（Path）与字形 run（GlyphRun）尚未进入显示列表：前者要等几何缓存策略，
// 后者要等 IFontRasterizer 的图集。放进列表却画不出来比不放更糟。
using Item = std::variant<FillRect, StrokeRect, FillEllipse, StrokeEllipse, FillRoundedRect,
                          StrokeRoundedRect, Segment, PathShape, Blit>;

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
