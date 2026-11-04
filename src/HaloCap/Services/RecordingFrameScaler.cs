using System;
using HaloCap.Models;

namespace HaloCap.Services;

/// <summary>
/// 录屏帧的缩放：区域原样按行拷贝，需要下采样时按面积平均（盒式滤镜），
/// 保证文字边缘不会出现双线性那样的拉丝和闪烁。
/// </summary>
internal static class RecordingFrameScaler
{
    public static PhysicalRect ComputeFitRect(
        int sourceWidth,
        int sourceHeight,
        int canvasWidth,
        int canvasHeight)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0 || canvasWidth <= 0 || canvasHeight <= 0)
        {
            return new PhysicalRect(0, 0, 0, 0);
        }

        if (sourceWidth == canvasWidth && sourceHeight == canvasHeight)
        {
            return new PhysicalRect(0, 0, canvasWidth, canvasHeight);
        }

        var scale = Math.Min((double)canvasWidth / sourceWidth, (double)canvasHeight / sourceHeight);
        var width = Math.Max(2, MakeEven((int)Math.Round(sourceWidth * scale)));
        var height = Math.Max(2, MakeEven((int)Math.Round(sourceHeight * scale)));

        width = Math.Min(width, MakeEven(canvasWidth));
        height = Math.Min(height, MakeEven(canvasHeight));

        var left = Math.Max(0, (MakeEven(canvasWidth) - width) / 2);
        var top = Math.Max(0, (MakeEven(canvasHeight) - height) / 2);

        return new PhysicalRect(left, top, width, height);
    }

    public static void ClearOutside(
        byte[] destination,
        int canvasWidth,
        int canvasHeight,
        PhysicalRect content)
    {
        var stride = canvasWidth * 4;

        FillSpan(destination, stride, 0, 0, canvasWidth, content.Top);
        FillSpan(
            destination,
            stride,
            0,
            content.Bottom,
            canvasWidth,
            Math.Max(0, canvasHeight - content.Bottom));
        FillSpan(destination, stride, 0, content.Top, content.Left, content.Height);
        FillSpan(
            destination,
            stride,
            content.Right,
            content.Top,
            Math.Max(0, canvasWidth - content.Right),
            content.Height);
    }

    public static void CopyOrScale(
        byte[] source,
        int sourceWidth,
        int sourceHeight,
        int sourceStride,
        byte[] destination,
        int destinationWidth,
        int destinationHeight,
        int destinationStride,
        int destinationOffsetX,
        int destinationOffsetY)
    {
        if (destinationWidth <= 0 || destinationHeight <= 0)
        {
            return;
        }

        if (sourceWidth == destinationWidth &&
            sourceHeight == destinationHeight &&
            sourceStride == destinationStride &&
            destinationOffsetX == 0 &&
            destinationOffsetY == 0)
        {
            for (var row = 0; row < sourceHeight; row++)
            {
                Buffer.BlockCopy(
                    source,
                    row * sourceStride,
                    destination,
                    row * destinationStride,
                    sourceWidth * 4);
            }

            return;
        }

        if (sourceWidth <= 0 || sourceHeight <= 0)
        {
            return;
        }

        var xStep = (double)sourceWidth / destinationWidth;
        var yStep = (double)sourceHeight / destinationHeight;

        for (var y = 0; y < destinationHeight; y++)
        {
            var sourceTop = Math.Min(sourceHeight - 1, (int)(y * yStep));
            var sourceBottom = Math.Min(sourceHeight, Math.Max(sourceTop + 1, (int)Math.Ceiling((y + 1) * yStep)));
            var targetRow = ((destinationOffsetY + y) * destinationStride) + (destinationOffsetX * 4);

            if (targetRow < 0 || destination.Length < targetRow + (destinationWidth * 4))
            {
                break;
            }

            for (var x = 0; x < destinationWidth; x++)
            {
                var left = Math.Min(sourceWidth - 1, (int)(x * xStep));
                var right = Math.Min(sourceWidth, Math.Max(left + 1, (int)Math.Ceiling((x + 1) * xStep)));

                var blue = 0L;
                var green = 0L;
                var red = 0L;
                var count = 0L;

                for (var sy = sourceTop; sy < sourceBottom; sy++)
                {
                    var rowOffset = sy * sourceStride;

                    for (var sx = left; sx < right; sx++)
                    {
                        var offset = rowOffset + (sx * 4);

                        if (offset + 3 >= source.Length)
                        {
                            break;
                        }

                        blue += source[offset];
                        green += source[offset + 1];
                        red += source[offset + 2];
                        count++;
                    }
                }

                var divisor = count <= 0 ? 1L : count;
                var target = targetRow + (x * 4);
                destination[target] = (byte)(blue / divisor);
                destination[target + 1] = (byte)(green / divisor);
                destination[target + 2] = (byte)(red / divisor);
                destination[target + 3] = 255;
            }
        }
    }

    private static void FillSpan(
        byte[] destination,
        int stride,
        int x,
        int y,
        int width,
        int height)
    {
        if (width <= 0 || height <= 0)
        {
            return;
        }

        var rowBytes = width * 4;
        var blackRow = new byte[rowBytes];
        for (var i = 3; i < rowBytes; i += 4)
        {
            blackRow[i] = 255;
        }

        for (var row = 0; row < height; row++)
        {
            var offset = ((y + row) * stride) + (x * 4);

            if (offset < 0 || destination.Length < offset + rowBytes)
            {
                break;
            }

            Buffer.BlockCopy(blackRow, 0, destination, offset, rowBytes);
        }
    }

    private static int MakeEven(int value) => value - (value & 1);
}
