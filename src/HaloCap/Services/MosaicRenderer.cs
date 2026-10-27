using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Models;

namespace HaloCap.Services;

/// <summary>
/// 把选区降采样成“每格一个像素”的小位图；显示与导出时再以最近邻放大，得到清晰的马赛克块。
/// </summary>
internal static class MosaicRenderer
{
    public static Int32Rect SnapToBlocks(
        int left,
        int top,
        int right,
        int bottom,
        PhysicalRect limit,
        int blockPixels)
    {
        if (blockPixels < 1)
        {
            blockPixels = 1;
        }

        var x1 = (int)(Math.Floor(left / (double)blockPixels) * blockPixels);
        var y1 = (int)(Math.Floor(top / (double)blockPixels) * blockPixels);
        var x2 = (int)(Math.Ceiling(right / (double)blockPixels) * blockPixels);
        var y2 = (int)(Math.Ceiling(bottom / (double)blockPixels) * blockPixels);

        x1 = Math.Max(x1, limit.Left);
        y1 = Math.Max(y1, limit.Top);
        x2 = Math.Min(x2, limit.Right);
        y2 = Math.Min(y2, limit.Bottom);

        return new Int32Rect(x1, y1, Math.Max(0, x2 - x1), Math.Max(0, y2 - y1));
    }

    /// <summary>
    /// 先把底图与已有标注合成为一张区域位图，再做块平均，
    /// 这样马赛克会覆盖并像素化它下面的所有标注内容。
    /// </summary>
    public static BitmapSource? CreateCompositedTile(
        BitmapSource source,
        Int32Rect region,
        IReadOnlyList<AnnotationStroke> priorStrokes,
        Matrix strokeToRegion,
        int blockPixels)
    {
        if (region.Width < 1 ||
            region.Height < 1 ||
            region.X < 0 ||
            region.Y < 0 ||
            region.X + region.Width > source.PixelWidth ||
            region.Y + region.Height > source.PixelHeight)
        {
            return null;
        }

        var visual = new DrawingVisual();
        RenderOptions.SetBitmapScalingMode(visual, BitmapScalingMode.NearestNeighbor);

        using (var context = visual.RenderOpen())
        {
            var cropped = new CroppedBitmap(source, region);
            cropped.Freeze();
            context.DrawImage(cropped, new Rect(0, 0, region.Width, region.Height));

            if (priorStrokes.Count > 0)
            {
                context.PushTransform(new MatrixTransform(strokeToRegion));
                AnnotationRenderer.Draw(context, priorStrokes, 1.0);
                context.Pop();
            }
        }

        var target = new RenderTargetBitmap(
            region.Width,
            region.Height,
            96,
            96,
            PixelFormats.Pbgra32);

        target.Render(visual);
        target.Freeze();

        return CreateTile(target, new Int32Rect(0, 0, region.Width, region.Height), blockPixels);
    }

    public static BitmapSource? CreateTile(BitmapSource source, Int32Rect region, int blockPixels)
    {
        if (blockPixels < 1 ||
            region.Width < 1 ||
            region.Height < 1 ||
            region.X < 0 ||
            region.Y < 0 ||
            region.X + region.Width > source.PixelWidth ||
            region.Y + region.Height > source.PixelHeight)
        {
            return null;
        }

        var columns = (region.Width + blockPixels - 1) / blockPixels;
        var rows = (region.Height + blockPixels - 1) / blockPixels;
        var converted = new FormatConvertedBitmap(source, PixelFormats.Bgr32, null, 0);
        converted.Freeze();

        var stride = region.Width * 4;
        var pixels = new byte[stride * region.Height];
        converted.CopyPixels(region, pixels, stride, 0);

        var tile = new byte[columns * rows * 4];

        for (var blockY = 0; blockY < rows; blockY++)
        {
            var startY = blockY * blockPixels;
            var endY = Math.Min(startY + blockPixels, region.Height);

            for (var blockX = 0; blockX < columns; blockX++)
            {
                var startX = blockX * blockPixels;
                var endX = Math.Min(startX + blockPixels, region.Width);
                long sumBlue = 0;
                long sumGreen = 0;
                long sumRed = 0;
                var count = 0;

                for (var y = startY; y < endY; y++)
                {
                    var rowStart = (y * stride) + (startX * 4);

                    for (var x = startX; x < endX; x++)
                    {
                        var index = rowStart + ((x - startX) * 4);
                        sumBlue += pixels[index];
                        sumGreen += pixels[index + 1];
                        sumRed += pixels[index + 2];
                        count++;
                    }
                }

                if (count == 0)
                {
                    continue;
                }

                var target = ((blockY * columns) + blockX) * 4;
                tile[target] = (byte)(sumBlue / count);
                tile[target + 1] = (byte)(sumGreen / count);
                tile[target + 2] = (byte)(sumRed / count);
                tile[target + 3] = 255;
            }
        }

        var result = BitmapSource.Create(
            columns,
            rows,
            96,
            96,
            PixelFormats.Bgr32,
            null,
            tile,
            columns * 4);

        result.Freeze();
        return result;
    }
}
