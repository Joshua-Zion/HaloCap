using System;
using System.Collections.Generic;

namespace HaloCap.Models;

public enum OcrEngineKind
{
    Native = 0,
    Model = 1
}

internal enum OcrImageSource
{
    Selection,
    Pin
}

public sealed class OcrSettings
{
    public OcrEngineKind Engine { get; set; } = OcrEngineKind.Native;

    public bool AutoCopy { get; set; } = false;

    public bool PinOcrEnabled { get; set; } = true;

    public OcrSettings Clone() => new()
    {
        Engine = Engine,
        AutoCopy = AutoCopy,
        PinOcrEnabled = PinOcrEnabled
    };

    public void Normalize()
    {
        if (!Enum.IsDefined(Engine))
        {
            Engine = OcrEngineKind.Native;
        }
    }

    public bool ValueEquals(OcrSettings? other) =>
        other is not null &&
        Engine == other.Engine &&
        AutoCopy == other.AutoCopy &&
        PinOcrEnabled == other.PinOcrEnabled;
}

internal enum OcrStatus
{
    Success,
    Empty,
    NoLanguage,
    Failed
}

internal sealed class OcrOutcome
{
    public OcrStatus Status { get; init; }

    public IReadOnlyList<string> Lines { get; init; } = Array.Empty<string>();

    public string Text { get; init; } = string.Empty;

    public string Message { get; init; } = string.Empty;

    public string EngineLabel { get; init; } = string.Empty;

    public OcrEngineKind Engine { get; init; } = OcrEngineKind.Native;

    public long ElapsedMilliseconds { get; init; }
}
