using System;
using HaloCap.Interop;

namespace HaloCap.Models;

internal readonly struct PhysicalRect : IEquatable<PhysicalRect>
{
    public PhysicalRect(int left, int top, int width, int height)
    {
        Left = left;
        Top = top;
        Width = width;
        Height = height;
    }

    public int Left { get; }

    public int Top { get; }

    public int Width { get; }

    public int Height { get; }

    public int Right => Left + Width;

    public int Bottom => Top + Height;

    public bool IsEmpty => Width <= 0 || Height <= 0;

    public long Area => (long)Width * Height;

    public static PhysicalRect FromEdges(int left, int top, int right, int bottom) =>
        new(left, top, right - left, bottom - top);

    public static PhysicalRect FromNative(NativeMethods.RECT rect) =>
        new(rect.Left, rect.Top, rect.Width, rect.Height);

    public NativeMethods.RECT ToNative() => new()
    {
        Left = Left,
        Top = Top,
        Right = Right,
        Bottom = Bottom
    };

    public PhysicalRect Offset(int dx, int dy) => new(Left + dx, Top + dy, Width, Height);

    public bool Contains(int x, int y) => x >= Left && x < Right && y >= Top && y < Bottom;

    public bool Equals(PhysicalRect other) =>
        Left == other.Left &&
        Top == other.Top &&
        Width == other.Width &&
        Height == other.Height;

    public override bool Equals(object? obj) => obj is PhysicalRect other && Equals(other);

    public override int GetHashCode() => HashCode.Combine(Left, Top, Width, Height);

    public override string ToString() => $"{Left},{Top} {Width}x{Height}";
}

internal sealed class ScreenMonitor
{
    public IntPtr Handle { get; set; }

    public string DeviceName { get; set; } = string.Empty;

    public PhysicalRect Bounds { get; set; }

    public PhysicalRect WorkArea { get; set; }

    public double Scale { get; set; } = 1.0;

    public bool IsPrimary { get; set; }

    public override string ToString() =>
        $"{DeviceName} {Bounds} @{Scale:0.##}x";
}

internal static class ScreenGeometry
{
    public const int MinimumSelectionSize = 4;

    public static PhysicalRect NormalizeEdges(int x1, int y1, int x2, int y2)
    {
        var left = Math.Min(x1, x2);
        var top = Math.Min(y1, y2);
        var right = Math.Max(x1, x2);
        var bottom = Math.Max(y1, y2);
        return PhysicalRect.FromEdges(left, top, right, bottom);
    }

    public static PhysicalRect NormalizeSelection(
        int startX,
        int startY,
        int endX,
        int endY,
        int width,
        int height)
    {
        var maxX = Math.Max(0, width);
        var maxY = Math.Max(0, height);

        var x1 = Math.Clamp(startX, 0, maxX);
        var y1 = Math.Clamp(startY, 0, maxY);
        var x2 = Math.Clamp(endX, 0, maxX);
        var y2 = Math.Clamp(endY, 0, maxY);

        return NormalizeEdges(x1, y1, x2, y2);
    }

    public static PhysicalRect Intersect(PhysicalRect a, PhysicalRect b)
    {
        var left = Math.Max(a.Left, b.Left);
        var top = Math.Max(a.Top, b.Top);
        var right = Math.Min(a.Right, b.Right);
        var bottom = Math.Min(a.Bottom, b.Bottom);

        if (right <= left || bottom <= top)
        {
            return new PhysicalRect(left, top, 0, 0);
        }

        return PhysicalRect.FromEdges(left, top, right, bottom);
    }

    public static PhysicalRect ClampTo(PhysicalRect rect, PhysicalRect container)
    {
        var width = Math.Clamp(rect.Width, 0, container.Width);
        var height = Math.Clamp(rect.Height, 0, container.Height);
        var left = Math.Clamp(rect.Left, container.Left, container.Right - width);
        var top = Math.Clamp(rect.Top, container.Top, container.Bottom - height);
        return new PhysicalRect(left, top, width, height);
    }

    public static PhysicalRect ToLocal(PhysicalRect virtualRect, PhysicalRect monitorBounds) =>
        virtualRect.Offset(-monitorBounds.Left, -monitorBounds.Top);

    public static PhysicalRect ToVirtual(PhysicalRect localRect, PhysicalRect monitorBounds) =>
        localRect.Offset(monitorBounds.Left, monitorBounds.Top);

    public static bool IsUsableSelection(PhysicalRect rect, int minimum = MinimumSelectionSize) =>
        rect.Width >= minimum && rect.Height >= minimum;

    public static PhysicalRect MakeEven(PhysicalRect rect)
    {
        var width = rect.Width - (rect.Width & 1);
        var height = rect.Height - (rect.Height & 1);
        return new PhysicalRect(rect.Left, rect.Top, Math.Max(0, width), Math.Max(0, height));
    }

    public static double DipToPixel(double dip, double pixelsPerDip) =>
        dip * pixelsPerDip;

    public static double PixelToDip(int pixels, double pixelsPerDip) =>
        pixelsPerDip <= 0 ? pixels : pixels / pixelsPerDip;
}
