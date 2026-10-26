namespace HaloCap.Models;

public enum ScreenshotImageFormat
{
    Png = 0,
    Jpeg = 1
}

public sealed class AppSettings
{
    public HotkeyDefinition Hotkey { get; set; } = new();

    public ScreenshotImageFormat ScreenshotFormat { get; set; } = ScreenshotImageFormat.Png;

    public string RecordingDirectory { get; set; } = string.Empty;
}
