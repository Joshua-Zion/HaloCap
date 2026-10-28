using System.Collections.Generic;

namespace HaloCap.Models;

public enum RadialAction
{
    Screenshot = 0,
    Ocr = 1,
    Record = 2,
    Pin = 3
}

public sealed record RadialActionInfo(
    RadialAction Action,
    string Title,
    string Shortcut,
    string IconGeometry,
    string AccentHex)
{
    public static IReadOnlyList<RadialActionInfo> All { get; } = new[]
    {
        new RadialActionInfo(
            RadialAction.Screenshot,
            "截图",
            "1",
            "M3 7V5a2 2 0 0 1 2-2h2 M17 3h2a2 2 0 0 1 2 2v2 M21 17v2a2 2 0 0 1-2 2h-2 M7 21H5a2 2 0 0 1-2-2v-2 M12 12h0.01",
            "#6CA8FF"),
        new RadialActionInfo(
            RadialAction.Ocr,
            "文字识别",
            "2",
            "M4 7V4h16v3 M9 20h6 M12 4v16",
            "#B08CFF"),
        new RadialActionInfo(
            RadialAction.Record,
            "录屏",
            "3",
            "M4 6h11a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2Z M17 10.5l4-2v7l-4-2Z",
            "#FF7373"),
        new RadialActionInfo(
            RadialAction.Pin,
            "贴图",
            "4",
            "M9 4h6v5l3 3v2H6v-2l3-3V4Z M12 14v6",
            "#F5B963")
    };

    public static RadialActionInfo Get(RadialAction action) => All[(int)action];
}
