using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Windows;
using System.Windows.Interop;
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
        var handle = bitmap.GetHbitmap();
        BitmapSource source;

        try
        {
            source = Imaging.CreateBitmapSourceFromHBitmap(
                handle,
                IntPtr.Zero,
                Int32Rect.Empty,
                BitmapSizeOptions.FromEmptyOptions());
        }
        finally
        {
            NativeMethods.DeleteObject(handle);
        }

        if (dpi <= 1 || Math.Abs(dpi - source.DpiX) < 0.5)
        {
            source.Freeze();
            return source;
        }

        var bytesPerPixel = Math.Max(1, (source.Format.BitsPerPixel + 7) / 8);
        var stride = source.PixelWidth * bytesPerPixel;
        var pixels = new byte[stride * source.PixelHeight];
        source.CopyPixels(pixels, stride, 0);

        var adjusted = BitmapSource.Create(
            source.PixelWidth,
            source.PixelHeight,
            dpi,
            dpi,
            source.Format,
            source.Palette,
            pixels,
            stride);

        adjusted.Freeze();
        return adjusted;
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
