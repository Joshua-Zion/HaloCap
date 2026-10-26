using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;

namespace HaloCap.Models;

internal enum AnnotationTool
{
    Pen,
    Rectangle,
    Ellipse,
    Arrow,
    Text,
    Highlighter
}

internal sealed class AnnotationStroke
{
    public AnnotationTool Tool { get; init; }

    public Color Color { get; init; }

    public double Thickness { get; init; } = AnnotationDefaults.Thickness;

    public List<Point> Points { get; } = new();

    public string? Text { get; init; }
}

internal static class AnnotationDefaults
{
    public const double Thickness = 3;
    public const double HighlighterThickness = 14;
    public const double TextSize = 18;

    public static readonly Color[] Palette =
    {
        Color.FromRgb(0xFF, 0x5A, 0x5A),
        Color.FromRgb(0xFF, 0xD1, 0x66),
        Color.FromRgb(0x7C, 0xE3, 0x8B),
        Color.FromRgb(0x6C, 0xA8, 0xFF),
        Color.FromRgb(0xFF, 0xFF, 0xFF)
    };
}
