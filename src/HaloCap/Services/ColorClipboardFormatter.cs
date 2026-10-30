using System.Windows.Media;

namespace HaloCap.Services;

internal static class ColorClipboardFormatter
{
    public static string Format(Color color, bool rgb) =>
        rgb
            ? $"rgb({color.R}, {color.G}, {color.B})"
            : $"#{color.R:X2}{color.G:X2}{color.B:X2}";
}
