#pragma once

#include <objbase.h>
#include <gdiplus.h>

namespace halocap::app {

// 真软阴影：把形状画进一张降采样的小位图，对 alpha 做两次盒式模糊，
// 再按目标矩形拉伸回来。比“多层同心椭圆/圆角矩形”少了同心环和硬边，
// 也比 1:1 全窗多层填充快得多；只在窗口打开/缓存帧构建时调用一次。
void DrawSoftShadow(Gdiplus::Graphics& target, const Gdiplus::RectF& rect, float cornerRadius,
                    float opacity, float blurRadius, float offsetY, bool ellipse);

}
