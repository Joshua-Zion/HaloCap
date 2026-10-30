using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using DrawingPixelFormat = System.Drawing.Imaging.PixelFormat;
using DrawingSize = System.Drawing.Size;

namespace HaloCap.Services;

internal static class ScreenCaptureService
{
    public static Bitmap Capture(PhysicalRect rect)
    {
        if (rect.IsEmpty)
        {
            throw new ArgumentException("截图区域为空。", nameof(rect));
        }

        var bitmap = new Bitmap(rect.Width, rect.Height, DrawingPixelFormat.Format32bppArgb);

        try
        {
            using var graphics = Graphics.FromImage(bitmap);
            graphics.CopyFromScreen(
                rect.Left,
                rect.Top,
                0,
                0,
                new DrawingSize(rect.Width, rect.Height),
                CopyPixelOperation.SourceCopy);

            bitmap.SetResolution(96, 96);
            return bitmap;
        }
        catch (Exception ex)
        {
            bitmap.Dispose();
            Log.Error($"屏幕捕获失败：{rect}", ex);
            throw;
        }
    }

    public static BitmapSource ToBitmapSource(Bitmap bitmap) => ToBitmapSource(bitmap, 96.0);

    public static BitmapSource ToBitmapSource(Bitmap bitmap, double dpi)
    {
        var effectiveDpi = dpi > 1 ? dpi : 96.0;
        var data = bitmap.LockBits(
            new Rectangle(0, 0, bitmap.Width, bitmap.Height),
            ImageLockMode.ReadOnly,
            DrawingPixelFormat.Format32bppArgb);

        try
        {
            // 直接从锁定内存创建一次位图，避免 HBITMAP + 中间字节数组 + 二次拷贝，
            // 4K 双屏下每次冻结画面可少占用数十 MB 峰值内存。
            var source = BitmapSource.Create(
                bitmap.Width,
                bitmap.Height,
                effectiveDpi,
                effectiveDpi,
                PixelFormats.Bgr32,
                null,
                data.Scan0,
                data.Stride * bitmap.Height,
                data.Stride);

            source.Freeze();
            return source;
        }
        finally
        {
            bitmap.UnlockBits(data);
        }
    }

    public static BitmapSource FromMonitor(ScreenMonitor monitor) => ToBitmapSource(Capture(monitor.Bounds));

    public static BitmapSource CropToBitmapSource(Bitmap source, PhysicalRect localRect, double dpi = 96.0)
    {
        var normalized = ScreenGeometry.Intersect(
            localRect,
            new PhysicalRect(0, 0, source.Width, source.Height));

        if (normalized.IsEmpty)
        {
            throw new InvalidOperationException("截图区域超出屏幕范围。");
        }

        using var cropped = source.Clone(
            new Rectangle(normalized.Left, normalized.Top, normalized.Width, normalized.Height),
            DrawingPixelFormat.Format32bppArgb);

        return ToBitmapSource(cropped, dpi);
    }

    public static BitmapSource CaptureRegion(PhysicalRect virtualRect, double dpi = 96.0)
    {
        using var bitmap = Capture(virtualRect);
        return ToBitmapSource(bitmap, dpi);
    }
}
