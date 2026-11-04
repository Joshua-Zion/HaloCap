using System;

namespace HaloCap.Models;

public enum RecordingResolution
{
    Original = 0,
    P2160 = 1,
    P1440 = 2,
    P1080 = 3,
    P720 = 4
}

public enum RecordingFrameRate
{
    Thirty = 30,
    Sixty = 60
}

public enum RecordingQuality
{
    High = 0,
    Balanced = 1,
    Compact = 2
}

public enum RecordingCountdown
{
    Off = 0,
    ThreeSeconds = 3,
    FiveSeconds = 5
}

public enum RecordingRange
{
    Region = 0,
    Window = 1
}

public enum RecordingAudioBitrate
{
    K128 = 128_000,
    K192 = 192_000
}

public sealed class RecordingSettings
{
    public RecordingResolution Resolution { get; set; } = RecordingResolution.Original;

    public RecordingFrameRate FrameRate { get; set; } = RecordingFrameRate.Thirty;

    public RecordingQuality Quality { get; set; } = RecordingQuality.Balanced;

    public bool RecordCursor { get; set; } = true;

    public RecordingCountdown Countdown { get; set; } = RecordingCountdown.ThreeSeconds;

    public RecordingRange Range { get; set; } = RecordingRange.Region;

    public bool SystemAudio { get; set; } = true;

    public bool Microphone { get; set; }

    public RecordingAudioBitrate AudioBitrate { get; set; } = RecordingAudioBitrate.K192;

    public int FramesPerSecond => (int)FrameRate;

    public int CountdownSeconds => (int)Countdown;

    public int AudioBitrateValue => (int)AudioBitrate;

    public bool WantsAudio => SystemAudio || Microphone;

    public RecordingSettings Clone() => new()
    {
        Resolution = Resolution,
        FrameRate = FrameRate,
        Quality = Quality,
        RecordCursor = RecordCursor,
        Countdown = Countdown,
        Range = Range,
        SystemAudio = SystemAudio,
        Microphone = Microphone,
        AudioBitrate = AudioBitrate
    };

    public void Normalize()
    {
        if (!Enum.IsDefined(typeof(RecordingResolution), Resolution))
        {
            Resolution = RecordingResolution.Original;
        }

        if (!Enum.IsDefined(typeof(RecordingFrameRate), FrameRate))
        {
            FrameRate = RecordingFrameRate.Thirty;
        }

        if (!Enum.IsDefined(typeof(RecordingQuality), Quality))
        {
            Quality = RecordingQuality.Balanced;
        }

        if (!Enum.IsDefined(typeof(RecordingCountdown), Countdown))
        {
            Countdown = RecordingCountdown.ThreeSeconds;
        }

        if (!Enum.IsDefined(typeof(RecordingRange), Range))
        {
            Range = RecordingRange.Region;
        }

        if (!Enum.IsDefined(typeof(RecordingAudioBitrate), AudioBitrate))
        {
            AudioBitrate = RecordingAudioBitrate.K192;
        }
    }

    public bool ValueEquals(RecordingSettings? other) =>
        other is not null &&
        Resolution == other.Resolution &&
        FrameRate == other.FrameRate &&
        Quality == other.Quality &&
        RecordCursor == other.RecordCursor &&
        Countdown == other.Countdown &&
        Range == other.Range &&
        SystemAudio == other.SystemAudio &&
        Microphone == other.Microphone &&
        AudioBitrate == other.AudioBitrate;

    public static int GetTargetHeight(RecordingResolution resolution) => resolution switch
    {
        RecordingResolution.P2160 => 2160,
        RecordingResolution.P1440 => 1440,
        RecordingResolution.P1080 => 1080,
        RecordingResolution.P720 => 720,
        _ => 0
    };

    /// <summary>
    /// 按目标高度等比下采样，不放大；输出宽高保持偶数，便于 H.264 编码。
    /// </summary>
    public (int Width, int Height) ResolveOutputSize(int sourceWidth, int sourceHeight)
    {
        if (sourceWidth <= 0 || sourceHeight <= 0)
        {
            return (0, 0);
        }

        var targetHeight = GetTargetHeight(Resolution);

        if (targetHeight <= 0 || sourceHeight <= targetHeight)
        {
            return (MakeEven(sourceWidth), MakeEven(sourceHeight));
        }

        var scaledWidth = (int)Math.Round((double)sourceWidth * targetHeight / sourceHeight);
        var clampedWidth = Math.Clamp(scaledWidth, 2, MakeEven(sourceWidth));
        var clampedHeight = Math.Clamp(MakeEven(targetHeight), 2, MakeEven(sourceHeight));

        return (clampedWidth, clampedHeight);
    }

    public int ComputeBitrate(int width, int height)
    {
        if (width <= 0 || height <= 0)
        {
            return MinimumBitrate;
        }

        var bitsPerPixel = Quality switch
        {
            RecordingQuality.High => 0.20,
            RecordingQuality.Compact => 0.055,
            _ => 0.11
        };

        var estimated = (long)((double)width * height * FramesPerSecond * bitsPerPixel);

        return (int)Math.Clamp(estimated, MinimumBitrate, MaximumBitrate);
    }

    public string DescribeOutput(int width, int height) =>
        $"{width}×{height} · {(int)FrameRate}fps · {GetQualityLabel(Quality)}";

    public static string GetResolutionLabel(RecordingResolution resolution) => resolution switch
    {
        RecordingResolution.P2160 => "2160p",
        RecordingResolution.P1440 => "1440p",
        RecordingResolution.P1080 => "1080p",
        RecordingResolution.P720 => "720p",
        _ => "原始"
    };

    public static string GetQualityLabel(RecordingQuality quality) => quality switch
    {
        RecordingQuality.High => "高",
        RecordingQuality.Compact => "紧凑",
        _ => "均衡"
    };

    private const int MinimumBitrate = 1_000_000;
    private const int MaximumBitrate = 45_000_000;

    private static int MakeEven(int value) => value - (value & 1);
}
