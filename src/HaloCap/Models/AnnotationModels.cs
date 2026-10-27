using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace HaloCap.Models;

internal enum AnnotationTool
{
    Pen,
    Rectangle,
    Ellipse,
    Arrow,
    Text,
    Highlighter,
    Mosaic,
    Step,
    Eraser
}

internal enum ArrowHeadMode
{
    End,
    Both,
    None
}

internal sealed class AnnotationStroke
{
    public AnnotationTool Tool { get; init; }

    public Color Color { get; init; }

    public double Thickness { get; set; } = AnnotationDefaults.Thickness;

    public double TextSize { get; init; } = AnnotationDefaults.TextSize;

    public bool Bold { get; init; } = true;

    public bool Italic { get; init; }

    public bool Filled { get; init; }

    public ArrowHeadMode ArrowHead { get; init; } = ArrowHeadMode.End;

    public BitmapSource? Mosaic { get; set; }

    public List<Point> Points { get; } = new();

    public string? Text { get; init; }
}

internal static class AnnotationDefaults
{
    public const double Thickness = 3;
    public const double MinThickness = 1;
    public const double MaxThickness = 24;
    public const double HighlighterThickness = 14;
    public const double MinHighlighterThickness = 6;
    public const double MaxHighlighterThickness = 40;
    public const double TextSize = 18;
    public const double MinTextSize = 10;
    public const double MaxTextSize = 48;
    public const int MosaicBlockDip = 12;
    public const int MinMosaicBlockDip = 6;
    public const int MaxMosaicBlockDip = 30;
    public const double EraserSize = 18;
    public const double MinEraserSize = 8;
    public const double MaxEraserSize = 48;
    public const double StepRadius = 11;

    public static readonly Color[] Palette =
    {
        Color.FromRgb(0xFF, 0x5A, 0x5A),
        Color.FromRgb(0xFF, 0xA9, 0x4D),
        Color.FromRgb(0xFF, 0xD1, 0x66),
        Color.FromRgb(0x7C, 0xE3, 0x8B),
        Color.FromRgb(0x6C, 0xA8, 0xFF),
        Color.FromRgb(0xB0, 0x8C, 0xFF),
        Color.FromRgb(0x24, 0x29, 0x33),
        Color.FromRgb(0xFF, 0xFF, 0xFF)
    };
}

internal static class AnnotationGeometry
{
    public static Rect NormalizeRect(Point a, Point b) => new(
        Math.Min(a.X, b.X),
        Math.Min(a.Y, b.Y),
        Math.Abs(a.X - b.X),
        Math.Abs(a.Y - b.Y));

    public static Point ConstrainSquare(Point start, Point end)
    {
        var dx = end.X - start.X;
        var dy = end.Y - start.Y;
        var size = Math.Max(Math.Abs(dx), Math.Abs(dy));
        var signX = dx < 0 ? -1 : 1;
        var signY = dy < 0 ? -1 : 1;
        return new Point(start.X + (size * signX), start.Y + (size * signY));
    }

    public static Point SnapToAngle(Point start, Point end)
    {
        var dx = end.X - start.X;
        var dy = end.Y - start.Y;
        var length = Math.Sqrt((dx * dx) + (dy * dy));

        if (length < 0.001)
        {
            return end;
        }

        const double step = Math.PI / 4;
        var angle = Math.Round(Math.Atan2(dy, dx) / step) * step;
        return new Point(
            start.X + (length * Math.Cos(angle)),
            start.Y + (length * Math.Sin(angle)));
    }

    public static bool IsLightColor(Color color) =>
        (((color.R * 299) + (color.G * 587) + (color.B * 114)) / 1000) > 178;
}
