using System;

namespace HaloCap.Services;

/// <summary>
/// 录制音频用的采样转换：把设备交付的任意声道/采样率 float 交错数据变成
/// 48kHz 立体声。按包处理并借一帧上一包结尾做跨包插值，所以既不吞帧也不产生接缝。
/// </summary>
internal static class AudioFormatConverter
{
    public const int TargetSampleRate = 48_000;
    public const int TargetChannels = 2;

    private const double BoundaryTolerance = 1e-9;

    /// <summary>
    /// 线性插值重采样。<paramref name="readOffset"/> 是相对当前包起点的读取位置，
    /// 可以为负：那一帧落在上一包的尾巴上，由 <paramref name="previousFrame"/> 顶上。
    /// 记下上一包最后一帧才不会在包边界丢样本，长时间录制才不会有节拍性空洞。
    /// </summary>
    public static int ResampleTo48k(
        float[] source,
        int sourceFrames,
        int sourceChannels,
        double sourceRate,
        float[] destination,
        int destinationFrames,
        float[] previousFrame,
        ref double readOffset)
    {
        if (source is null || destination is null || previousFrame is null ||
            sourceChannels <= 0 || sourceFrames < 1 || sourceRate <= 1 ||
            destinationFrames <= 0)
        {
            return 0;
        }

        var step = sourceRate / TargetSampleRate;
        var position = readOffset;
        var written = 0;

        while (written < destinationFrames && position < sourceFrames)
        {
            var frame = (int)Math.Floor(position);
            var fraction = position - frame;

            // 只有真的需要插值时才要求后一帧存在，否则同采样率搬运也要白丢最后一帧。
            // 位置是累加出来的，边界会带 1e-13 的误差，所以留一点容差。
            if (frame + 1 >= sourceFrames && fraction > BoundaryTolerance)
            {
                break;
            }

            for (var channel = 0; channel < TargetChannels; channel++)
            {
                var from = SampleAt(source, previousFrame, frame, channel, sourceChannels);
                var to = SampleAt(source, previousFrame, frame + 1, channel, sourceChannels);
                destination[(written * TargetChannels) + channel] = from + ((to - from) * (float)fraction);
            }

            position += step;
            written++;
        }

        // 下一包从"本包最后一帧之后"继续，所以坐标要相对下一包的起点重新算。
        // 最多只能借上一包的一帧（就是 previousFrame），所以夹在 -1。
        readOffset = Math.Max(-1.0, position - sourceFrames);
        CarryLastFrame(source, sourceFrames, sourceChannels, previousFrame);
        return written;
    }

    private static float SampleAt(
        float[] source,
        float[] previousFrame,
        int frame,
        int channel,
        int sourceChannels)
    {
        if (frame < 0)
        {
            return channel < previousFrame.Length ? previousFrame[channel] : 0f;
        }

        var index = frame * sourceChannels + Math.Min(channel, sourceChannels - 1);
        return index < source.Length ? source[index] : 0f;
    }

    private static void CarryLastFrame(
        float[] source,
        int sourceFrames,
        int sourceChannels,
        float[] previousFrame)
    {
        if (sourceFrames < 1)
        {
            return;
        }

        var lastOffset = (sourceFrames - 1) * sourceChannels;

        for (var channel = 0; channel < previousFrame.Length; channel++)
        {
            var index = lastOffset + Math.Min(channel, sourceChannels - 1);
            previousFrame[channel] = index < source.Length ? source[index] : 0f;
        }
    }

    /// <summary>峰值电平（0..1），供 HUD 电平条使用。</summary>
    public static float PeakLevel(float[] samples, int frames, int channels)
    {
        if (samples is null || channels <= 0)
        {
            return 0f;
        }

        var count = System.Math.Min(samples.Length, frames * channels);
        var peak = 0f;

        for (var i = 0; i < count; i++)
        {
            var value = System.Math.Abs(samples[i]);

            if (value > peak)
            {
                peak = value;
            }
        }

        return float.IsFinite(peak) ? System.Math.Min(1f, peak) : 0f;
    }
}
