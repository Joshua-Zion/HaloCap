#pragma once

#include "display_list.h"

#include <cstdint>
#include <vector>

namespace halocap::core {

// 马赛克：1.x 的做法是「把区域按块平均降采样成每格一个像素，显示时再最近邻放大」。
// 平均而不是取样，是让块与块之间没有噪点；降采样后的整块图作为一张后端位图注册，
// 显示列表里就是一次普通 Blit。
struct MosaicTile {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;   // width * height * 4，不透明
};

// 块大小是 DIP；屏幕/导出缩放之后按像素算。
int MosaicBlockPixels(int blockDip, float scale);

// 把区域对齐到块网格，再夹进 limit。返回空矩形表示整块都在画面外。
IntRect SnapMosaicRegion(IntRect region, IntRect limit, int blockPixels);

// 对 BGRA 源图的一块区域做块平均。source.pixels/stride 是裸缓冲（§3.3 规则二）。
MosaicTile DownsampleRegion(const ImageRef& source, IntRect region, int blockPixels);

}
