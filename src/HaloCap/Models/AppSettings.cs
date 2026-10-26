namespace HaloCap.Models;

public enum ScreenshotImageFormat
{
    Png = 0,
    Jpeg = 1
}

public enum RadialMenuSize
{
    Small = 0,
    Medium = 1,
    Large = 2
}

public sealed class AppSettings
{
    public HotkeyDefinition Hotkey { get; set; } = new();

    public ScreenshotImageFormat ScreenshotFormat { get; set; } = ScreenshotImageFormat.Png;

    public RadialMenuSize MenuSize { get; set; } = RadialMenuSize.Medium;

    public string RecordingDirectory { get; set; } = string.Empty;
}
