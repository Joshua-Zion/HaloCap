#include "mosaic.h"

#include <algorithm>
#include <cmath>

namespace halocap::core {

namespace {

int FloorToBlock(int value, int blockPixels) {
    const int block = std::max(1, blockPixels);
    const int quotient = value >= 0 ? value / block : (value - block + 1) / block;
    return quotient * block;
}

int CeilToBlock(int value, int blockPixels) {
    const int block = std::max(1, blockPixels);
    const int floor = FloorToBlock(value, block);
    return floor == value ? value : floor + block;
}

}  // namespace

int MosaicBlockPixels(int blockDip, float scale) {
    if (scale <= 0.0f) {
        scale = 1.0f;
    }
    return std::max(1, static_cast<int>(std::lround(static_cast<double>(blockDip) * scale)));
}

IntRect SnapMosaicRegion(IntRect region, IntRect limit, int blockPixels) {
    IntRect snapped{};
    snapped.left = FloorToBlock(region.left, blockPixels);
    snapped.top = FloorToBlock(region.top, blockPixels);
    snapped.right = CeilToBlock(region.right, blockPixels);
    snapped.bottom = CeilToBlock(region.bottom, blockPixels);

    if (!limit.IsEmpty()) {
        snapped.left = std::max(snapped.left, limit.left);
        snapped.top = std::max(snapped.top, limit.top);
        snapped.right = std::min(snapped.right, limit.right);
        snapped.bottom = std::min(snapped.bottom, limit.bottom);
    }

    if (snapped.right <= snapped.left || snapped.bottom <= snapped.top) {
        return IntRect{};
    }
    return snapped;
}

MosaicTile DownsampleRegion(const ImageRef& source, IntRect region, int blockPixels) {
    MosaicTile tile;

    const int block = std::max(1, blockPixels);
    if (source.pixels == nullptr || source.width <= 0 || source.height <= 0 || source.stride <= 0) {
        return tile;
    }

    const IntRect limit{ 0, 0, source.width, source.height };
    const IntRect clamped = SnapMosaicRegion(region, limit, block);
    if (clamped.IsEmpty()) {
        return tile;
    }

    const int columns = (clamped.right - clamped.left + block - 1) / block;
    const int rows = (clamped.bottom - clamped.top + block - 1) / block;

    tile.width = columns;
    tile.height = rows;
    tile.bgra.assign(static_cast<std::size_t>(columns) * rows * 4, 0);

    const auto* bits = static_cast<const std::uint8_t*>(source.pixels);

    for (int blockY = 0; blockY < rows; ++blockY) {
        const int startY = clamped.top + blockY * block;
        const int endY = std::min(startY + block, clamped.bottom);

        for (int blockX = 0; blockX < columns; ++blockX) {
            const int startX = clamped.left + blockX * block;
            const int endX = std::min(startX + block, clamped.right);

            std::uint64_t sumB = 0;
            std::uint64_t sumG = 0;
            std::uint64_t sumR = 0;
            std::uint64_t count = 0;

            for (int y = startY; y < endY; ++y) {
                const std::uint8_t* row = bits + static_cast<std::size_t>(y) * source.stride;
                for (int x = startX; x < endX; ++x) {
                    const std::uint8_t* pixel = row + static_cast<std::size_t>(x) * 4;
                    sumB += pixel[0];
                    sumG += pixel[1];
                    sumR += pixel[2];
                    ++count;
                }
            }

            if (count == 0) {
                continue;
            }

            const std::size_t offset =
                (static_cast<std::size_t>(blockY) * columns + blockX) * 4;
            tile.bgra[offset + 0] = static_cast<std::uint8_t>(sumB / count);
            tile.bgra[offset + 1] = static_cast<std::uint8_t>(sumG / count);
            tile.bgra[offset + 2] = static_cast<std::uint8_t>(sumR / count);
            tile.bgra[offset + 3] = 255;
        }
    }

    return tile;
}

}
