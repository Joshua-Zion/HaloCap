// 马赛克：块大小换算、网格对齐、块平均降采样（含 BGR 通道顺序与 alpha）。
#include "mosaic.h"

#include <cstdio>
#include <vector>

namespace {

namespace core = halocap::core;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void TestBlockPixels() {
    CheckEqual(core::MosaicBlockPixels(12, 1.0f), 12, "1x keeps the DIP size");
    CheckEqual(core::MosaicBlockPixels(12, 1.5f), 18, "1.5x scales the block");
    CheckEqual(core::MosaicBlockPixels(12, 2.0f), 24, "2x scales the block");
    CheckEqual(core::MosaicBlockPixels(0, 1.0f), 1, "a zero block is at least one pixel");
    CheckEqual(core::MosaicBlockPixels(12, 0.0f), 12, "a zero scale falls back to 1x");
}

void TestSnap() {
    const core::IntRect limit{ 0, 0, 1920, 1080 };

    const core::IntRect snapped = core::SnapMosaicRegion(core::IntRect{ 10, 10, 50, 50 }, limit, 12);
    CheckEqual(snapped.left, 0, "left snaps down to the block grid");
    CheckEqual(snapped.top, 0, "top snaps down to the block grid");
    CheckEqual(snapped.right, 60, "right snaps up to the block grid");
    CheckEqual(snapped.bottom, 60, "bottom snaps up to the block grid");

    // 已经对齐的区域不动。
    const core::IntRect aligned = core::SnapMosaicRegion(core::IntRect{ 24, 24, 48, 48 }, limit, 12);
    CheckEqual(aligned.left, 24, "aligned left stays");
    CheckEqual(aligned.right, 48, "aligned right stays");

    // 画面之外的部分被夹掉；完全在外面时空矩形。
    const core::IntRect clamped =
        core::SnapMosaicRegion(core::IntRect{ 1900, 1070, 2000, 1200 }, limit, 12);
    CheckEqual(clamped.right, 1920, "right is clamped to the limit");
    CheckEqual(clamped.bottom, 1080, "bottom is clamped to the limit");

    const core::IntRect outside =
        core::SnapMosaicRegion(core::IntRect{ 3000, 3000, 3100, 3100 }, limit, 12);
    CheckEqual(outside.right, 0, "fully outside is empty");
}

void TestDownsample() {
    // 4×4 的图，左上 2×2 红、右上 2×2 绿、左下 2×2 蓝、右下 2×2 白。
    const int width = 4;
    const int height = 4;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4, 0);

    const auto setPixel = [&](int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
        const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
        pixels[offset + 0] = b;
        pixels[offset + 1] = g;
        pixels[offset + 2] = r;
        pixels[offset + 3] = 255;
    };

    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            setPixel(x, y, 0, 0, 255);          // 红（BGRA 下 B=0 G=0 R=255）
            setPixel(x + 2, y, 0, 255, 0);      // 绿
            setPixel(x, y + 2, 255, 0, 0);      // 蓝
            setPixel(x + 2, y + 2, 255, 255, 255);   // 白
        }
    }

    const core::ImageRef source{ pixels.data(), width, height, width * 4 };
    const core::MosaicTile tile = core::DownsampleRegion(source, core::IntRect{ 0, 0, 4, 4 }, 2);

    CheckEqual(tile.width, 2, "two columns");
    CheckEqual(tile.height, 2, "two rows");
    CheckEqual(static_cast<long long>(tile.bgra.size()), 16, "tile buffer is 2*2*4");

    CheckEqual(tile.bgra[0], 0, "red block B");
    CheckEqual(tile.bgra[1], 0, "red block G");
    CheckEqual(tile.bgra[2], 255, "red block R");
    CheckEqual(tile.bgra[3], 255, "tile alpha is opaque");

    const std::size_t greenOffset = 4;
    CheckEqual(tile.bgra[greenOffset + 1], 255, "green block G");

    const std::size_t blueOffset = 8;
    CheckEqual(tile.bgra[blueOffset + 0], 255, "blue block B");

    const std::size_t whiteOffset = 12;
    CheckEqual(tile.bgra[whiteOffset + 0], 255, "white block B");
    CheckEqual(tile.bgra[whiteOffset + 2], 255, "white block R");

    // 非法输入给出空块，不崩。
    const core::MosaicTile empty = core::DownsampleRegion(core::ImageRef{}, core::IntRect{ 0, 0, 4, 4 }, 2);
    CheckEqual(empty.width, 0, "null image gives an empty tile");
    const core::MosaicTile wide = core::DownsampleRegion(source, core::IntRect{ 0, 0, 4, 4 }, 3);
    CheckEqual(wide.width, 2, "block wider than the image still covers it");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestBlockPixels();
    TestSnap();
    TestDownsample();

    if (failures != 0) {
        std::printf("MOSAIC_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("MOSAIC_TESTS=OK checks=%d\n", checks);
    return 0;
}
