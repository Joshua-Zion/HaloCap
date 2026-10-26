using System.Windows.Media.Imaging;

namespace HaloCap.Models;

internal enum CaptureSessionMode
{
    Screenshot,
    Ocr,
    Pin,
    Record
}

internal enum CaptureCommand
{
    Copy,
    Save,
    Pin,
    Ocr,
    StartRecording,
    Cancel
}

internal sealed class CaptureResult
{
    public CaptureCommand Command { get; init; }

    public BitmapSource? Image { get; init; }

    public PhysicalRect PhysicalRect { get; init; }

    public ScreenMonitor? Monitor { get; init; }
}
