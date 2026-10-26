using System;
using System.Windows.Media;

namespace HaloCap.UI;

internal static class UiColors
{
    public static Color Parse(string hex) => (Color)ColorConverter.ConvertFromString(hex);

    public static SolidColorBrush Brush(string hex) => Freeze(new SolidColorBrush(Parse(hex)));

    public static SolidColorBrush WithAlpha(Color color, byte alpha)
    {
        return Freeze(new SolidColorBrush(Color.FromArgb(alpha, color.R, color.G, color.B)));
    }

    private static SolidColorBrush Freeze(SolidColorBrush brush)
    {
        brush.Freeze();
        return brush;
    }
}
