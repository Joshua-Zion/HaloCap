#pragma once

#include "display_list.h"
#include "geometry.h"

#include <string>
#include <string_view>

namespace halocap::core {

// 图集的分组键：一张图集只属于一种「字体 + 字重 + 字号 + 颜色」。
// 换其中任何一项就是另一张图集，因此图元不必再带颜色——颜色已经烧进像素里了。
struct FontSpec {
    std::string family;
    int weight = 400;
    float emSizePx = 0.0f;
    Color color;

    bool operator==(const FontSpec& other) const {
        return family == other.family && weight == other.weight && emSizePx == other.emSizePx &&
               color == other.color;
    }
};

// 一次排版的产物：图集里的这块像素矩形，贴到目标上的那个矩形。
// 对显示列表来说它就是一次普通 blit，所以文本不需要新的图元类型。
struct TextCell {
    ImageHandle atlas = 0;
    Rect source;
    Rect destination;

    Blit ToBlit() const { return Blit{ atlas, source, destination, 1.0f }; }
};

// 8 个 seam 里的 IFontRasterizer（§3.2）。文本用 UTF-8；origin 是目标上的左上角，
// 设备像素（目标 96dpi，故与 DIP 等值）。同一 (spec, text) 第二次调用应命中缓存。
class IFontRasterizer {
public:
    virtual ~IFontRasterizer() = default;

    virtual TextCell LayOut(std::string_view text, const FontSpec& spec, Point origin) = 0;
};

}
