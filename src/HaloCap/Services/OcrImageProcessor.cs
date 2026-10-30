using System;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace HaloCap.Services;

internal sealed record PreparedOcrImage(byte[] Pixels, int Width, int Height);

internal static class OcrImageProcessor
{
    public const int MaxWorkingDimension = 4096;

    public static PreparedOcrImage Prepare(BitmapSource source, int maxDimension)
    {
        if (source.Format != PixelFormats.Bgra32)
        {
            var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);
            converted.Freeze();
            source = converted;
        }

        var width = source.PixelWidth;
        var height = source.PixelHeight;

        if (width <= 0 || height <= 0)
        {
            throw new ArgumentException("图像尺寸无效。", nameof(source));
        }

        var stride = width * 4;
        var pixels = new byte[stride * height];
        source.CopyPixels(pixels, stride, 0);
        CompositeOverWhite(pixels);

        if (maxDimension > 0)
        {
            var longest = Math.Max(width, height);

            if (longest > maxDimension)
            {
                var scale = maxDimension / (double)longest;
                var targetWidth = Math.Max(1, (int)Math.Round(width * scale));
                var targetHeight = Math.Max(1, (int)Math.Round(height * scale));
                pixels = ResizeBilinear(pixels, width, height, targetWidth, targetHeight);
                width = targetWidth;
                height = targetHeight;
            }
        }

        return new PreparedOcrImage(pixels, width, height);
    }

    internal static bool InvertIfDark(byte[] pixels)
    {
        if (pixels.Length == 0)
        {
            return false;
        }

        long luminance = 0;
        var samples = 0;

        for (var index = 0; index + 2 < pixels.Length; index += 64)
        {
            luminance += (long)Math.Round(
                (pixels[index] * 0.114) +
                (pixels[index + 1] * 0.587) +
                (pixels[index + 2] * 0.299));
            samples++;
        }

        if (samples == 0 || luminance / samples >= 110)
        {
            return false;
        }

        for (var index = 0; index + 2 < pixels.Length; index += 4)
        {
            pixels[index] = (byte)(255 - pixels[index]);
            pixels[index + 1] = (byte)(255 - pixels[index + 1]);
            pixels[index + 2] = (byte)(255 - pixels[index + 2]);
        }

        return true;
    }

    internal static void CompositeOverWhite(byte[] pixels)
    {
        for (var index = 0; index < pixels.Length; index += 4)
        {
            var alpha = pixels[index + 3];

            if (alpha == 255)
            {
                continue;
            }

            if (alpha == 0)
            {
                pixels[index] = 255;
                pixels[index + 1] = 255;
                pixels[index + 2] = 255;
                pixels[index + 3] = 255;
                continue;
            }

            var inverse = 255 - alpha;
            pixels[index] = (byte)((pixels[index] * alpha + (255 * inverse)) / 255);
            pixels[index + 1] = (byte)((pixels[index + 1] * alpha + (255 * inverse)) / 255);
            pixels[index + 2] = (byte)((pixels[index + 2] * alpha + (255 * inverse)) / 255);
            pixels[index + 3] = 255;
        }
    }

    internal static byte[] ResizeBilinear(
        byte[] source,
        int sourceWidth,
        int sourceHeight,
        int targetWidth,
        int targetHeight)
    {
        var target = new byte[targetWidth * targetHeight * 4];

        if (sourceWidth == targetWidth && sourceHeight == targetHeight)
        {
            Buffer.BlockCopy(source, 0, target, 0, Math.Min(source.Length, target.Length));
            return target;
        }

        var xRatio = sourceWidth / (double)targetWidth;
        var yRatio = sourceHeight / (double)targetHeight;

        for (var y = 0; y < targetHeight; y++)
        {
            var sourceY = ((y + 0.5) * yRatio) - 0.5;
            var y0 = (int)Math.Floor(sourceY);
            var y1 = Math.Min(sourceHeight - 1, Math.Max(0, y0 + 1));
            var y0Clamped = Math.Clamp(y0, 0, sourceHeight - 1);
            var fy = Math.Clamp(sourceY - y0, 0, 1);

            var row0 = y0Clamped * sourceWidth * 4;
            var row1 = y1 * sourceWidth * 4;
            var targetRow = y * targetWidth * 4;

            for (var x = 0; x < targetWidth; x++)
            {
                var sourceX = ((x + 0.5) * xRatio) - 0.5;
                var x0 = (int)Math.Floor(sourceX);
                var x1 = Math.Min(sourceWidth - 1, Math.Max(0, x0 + 1));
                var x0Clamped = Math.Clamp(x0, 0, sourceWidth - 1);
                var fx = Math.Clamp(sourceX - x0, 0, 1);

                var index = targetRow + (x * 4);
                var topLeft = row0 + (x0Clamped * 4);
                var topRight = row0 + (x1 * 4);
                var bottomLeft = row1 + (x0Clamped * 4);
                var bottomRight = row1 + (x1 * 4);

                for (var channel = 0; channel < 4; channel++)
                {
                    var top = (source[topLeft + channel] * (1 - fx)) +
                              (source[topRight + channel] * fx);
                    var bottom = (source[bottomLeft + channel] * (1 - fx)) +
                                 (source[bottomRight + channel] * fx);
                    target[index + channel] = (byte)Math.Clamp(
                        (int)Math.Round((top * (1 - fy)) + (bottom * fy)),
                        0,
                        255);
                }
            }
        }

        return target;
    }
}
