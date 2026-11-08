#include "soft_shadow.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace halocap::app {

namespace {

using namespace Gdiplus;

void BlurAlphaHorizontal(std::vector<std::uint8_t>& alpha, int width, int height, int radius) {
    if (radius <= 0) {
        return;
    }
    std::vector<int> prefix(static_cast<std::size_t>(width) + 1);
    std::vector<std::uint8_t> row(static_cast<std::size_t>(width));
    for (int y = 0; y < height; ++y) {
        prefix[0] = 0;
        for (int x = 0; x < width; ++x) {
            prefix[static_cast<std::size_t>(x) + 1] =
                prefix[static_cast<std::size_t>(x)] +
                alpha[static_cast<std::size_t>(y) * width + x];
        }
        for (int x = 0; x < width; ++x) {
            const int left = std::max(0, x - radius);
            const int right = std::min(width - 1, x + radius);
            const int sum = prefix[static_cast<std::size_t>(right) + 1] -
                            prefix[static_cast<std::size_t>(left)];
            row[static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>(sum / (right - left + 1));
        }
        std::memcpy(&alpha[static_cast<std::size_t>(y) * width], row.data(),
                    static_cast<std::size_t>(width));
    }
}

void BlurAlphaVertical(std::vector<std::uint8_t>& alpha, int width, int height, int radius) {
    if (radius <= 0) {
        return;
    }
    std::vector<int> prefix(static_cast<std::size_t>(height) + 1);
    std::vector<std::uint8_t> column(static_cast<std::size_t>(height));
    for (int x = 0; x < width; ++x) {
        prefix[0] = 0;
        for (int y = 0; y < height; ++y) {
            prefix[static_cast<std::size_t>(y) + 1] =
                prefix[static_cast<std::size_t>(y)] +
                alpha[static_cast<std::size_t>(y) * width + x];
        }
        for (int y = 0; y < height; ++y) {
            const int top = std::max(0, y - radius);
            const int bottom = std::min(height - 1, y + radius);
            const int sum = prefix[static_cast<std::size_t>(bottom) + 1] -
                            prefix[static_cast<std::size_t>(top)];
            column[static_cast<std::size_t>(y)] =
                static_cast<std::uint8_t>(sum / (bottom - top + 1));
        }
        for (int y = 0; y < height; ++y) {
            alpha[static_cast<std::size_t>(y) * width + x] =
                column[static_cast<std::size_t>(y)];
        }
    }
}

}  // namespace

void DrawSoftShadow(Graphics& target, const RectF& rect, float cornerRadius, float opacity,
                    float blurRadius, float offsetY, bool ellipse) {
    if (rect.Width <= 1.0f || rect.Height <= 1.0f || opacity <= 0.0f) {
        return;
    }

    const float margin = std::max(2.0f, blurRadius * 2.0f);
    const float totalWidth = rect.Width + margin * 2.0f;
    const float totalHeight = rect.Height + margin * 2.0f;
    const float downscale = std::min(0.5f, 300.0f / std::max(totalWidth, totalHeight));
    const int smallWidth = std::max(8, static_cast<int>(std::lround(totalWidth * downscale)));
    const int smallHeight = std::max(8, static_cast<int>(std::lround(totalHeight * downscale)));

    Bitmap mask(smallWidth, smallHeight, PixelFormat32bppPARGB);
    {
        Graphics graphics(&mask);
        graphics.SetSmoothingMode(SmoothingModeAntiAlias);
        graphics.Clear(Color(0, 0, 0, 0));
        const float scaleX = smallWidth / totalWidth;
        const float scaleY = smallHeight / totalHeight;
        const RectF smallRect(margin * scaleX, margin * scaleY, rect.Width * scaleX,
                              rect.Height * scaleY);
        SolidBrush black(Color(255, 0, 0, 0));
        if (ellipse) {
            graphics.FillEllipse(&black, smallRect);
        } else {
            GraphicsPath path;
            const float radius = std::max(0.0f, cornerRadius * std::min(scaleX, scaleY));
            const float d = radius * 2.0f;
            path.AddArc(smallRect.X, smallRect.Y, d, d, 180.0f, 90.0f);
            path.AddArc(smallRect.X + smallRect.Width - d, smallRect.Y, d, d, 270.0f, 90.0f);
            path.AddArc(smallRect.X + smallRect.Width - d, smallRect.Y + smallRect.Height - d, d, d,
                        0.0f, 90.0f);
            path.AddArc(smallRect.X, smallRect.Y + smallRect.Height - d, d, d, 90.0f, 90.0f);
            path.CloseFigure();
            graphics.FillPath(&black, &path);
        }
    }

    Rect lockRect(0, 0, smallWidth, smallHeight);
    BitmapData data{};
    if (mask.LockBits(&lockRect, ImageLockModeRead | ImageLockModeWrite, PixelFormat32bppPARGB,
                      &data) != Ok) {
        return;
    }
    std::vector<std::uint8_t> alpha(static_cast<std::size_t>(smallWidth) * smallHeight);
    auto* pixels = static_cast<std::uint8_t*>(data.Scan0);
    for (int y = 0; y < smallHeight; ++y) {
        const auto* row = pixels + static_cast<std::size_t>(y) * data.Stride;
        for (int x = 0; x < smallWidth; ++x) {
            alpha[static_cast<std::size_t>(y) * smallWidth + x] = row[x * 4 + 3];
        }
    }

    const int radiusPixels =
        std::max(1, static_cast<int>(std::lround(blurRadius * downscale * 0.5f)));
    BlurAlphaHorizontal(alpha, smallWidth, smallHeight, radiusPixels);
    BlurAlphaVertical(alpha, smallWidth, smallHeight, radiusPixels);
    BlurAlphaHorizontal(alpha, smallWidth, smallHeight, radiusPixels);
    BlurAlphaVertical(alpha, smallWidth, smallHeight, radiusPixels);

    for (int y = 0; y < smallHeight; ++y) {
        auto* row = pixels + static_cast<std::size_t>(y) * data.Stride;
        for (int x = 0; x < smallWidth; ++x) {
            const std::uint8_t value = alpha[static_cast<std::size_t>(y) * smallWidth + x];
            row[x * 4 + 0] = value;   // 预乘：黑 = 0，alpha 同时写进 RGB
            row[x * 4 + 1] = value;
            row[x * 4 + 2] = value;
            row[x * 4 + 3] = value;
        }
    }
    mask.UnlockBits(&data);

    ColorMatrix matrix = {};
    matrix.m[0][0] = 1.0f;
    matrix.m[1][1] = 1.0f;
    matrix.m[2][2] = 1.0f;
    matrix.m[3][3] = opacity;
    ImageAttributes attributes;
    attributes.SetColorMatrix(&matrix);

    target.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    target.DrawImage(&mask, RectF(rect.X - margin, rect.Y - margin + offsetY,
                                  totalWidth, totalHeight),
                     0.0f, 0.0f, static_cast<float>(smallWidth), static_cast<float>(smallHeight),
                     UnitPixel, &attributes);
}

}
