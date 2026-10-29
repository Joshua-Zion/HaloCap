using System.Windows.Input;

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

    // 默认值参考常见截图/效率工具的按键习惯，避免与呼出光圈冲突。
    public HotkeyDefinition ScreenshotHotkey { get; set; } =
        new(ModifierKeys.Control | ModifierKeys.Alt, Key.A);

    public HotkeyDefinition OcrHotkey { get; set; } =
        new(ModifierKeys.Control | ModifierKeys.Alt, Key.O);

    public HotkeyDefinition RecordHotkey { get; set; } =
        new(ModifierKeys.Control | ModifierKeys.Alt, Key.R);

    public HotkeyDefinition PinHotkey { get; set; } =
        new(ModifierKeys.Control | ModifierKeys.Alt, Key.P);

    public ScreenshotImageFormat ScreenshotFormat { get; set; } = ScreenshotImageFormat.Png;

    public RadialMenuSize MenuSize { get; set; } = RadialMenuSize.Medium;

    public string RecordingDirectory { get; set; } = string.Empty;

    public bool ConvertFilePathsToImages { get; set; } = true;

    public HotkeyDefinition GetActionHotkey(RadialAction action) => action switch
    {
        RadialAction.Screenshot => ScreenshotHotkey,
        RadialAction.Ocr => OcrHotkey,
        RadialAction.Record => RecordHotkey,
        RadialAction.Pin => PinHotkey,
        _ => ScreenshotHotkey
    };

    public void SetActionHotkey(RadialAction action, HotkeyDefinition hotkey)
    {
        switch (action)
        {
            case RadialAction.Screenshot:
                ScreenshotHotkey = hotkey;
                break;
            case RadialAction.Ocr:
                OcrHotkey = hotkey;
                break;
            case RadialAction.Record:
                RecordHotkey = hotkey;
                break;
            case RadialAction.Pin:
                PinHotkey = hotkey;
                break;
        }
    }
}
