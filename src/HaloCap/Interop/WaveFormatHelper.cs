using System;
using System.Runtime.InteropServices;

namespace HaloCap.Interop;

/// <summary>
/// 录制音频用到的 WAVEFORMATEX 字节块：WASAPI 初始化要 IEEE float 的
/// WAVEFORMATEXTENSIBLE，Media Foundation 的 AAC 编码器要 16 位整数 PCM。
/// </summary>
internal static class WaveFormatHelper
{
    internal static readonly Guid SubtypeIeeeFloat =
        new("00000003-0000-0010-8000-00AA00389B71");

    internal const short FormatExtensible = unchecked((short)0xFFFE);

    [StructLayout(LayoutKind.Sequential)]
    internal struct Extensible
    {
        public short FormatTag;
        public short Channels;
        public int SamplesPerSecond;
        public int AverageBytesPerSecond;
        public short BlockAlign;
        public short BitsPerSample;
        public short Size;
        public short ValidBitsPerSample;
        public int ChannelMask;
        public Guid SubType;
    }

    internal static byte[] BuildIeeeFloat(int sampleRate, int channels)
    {
        var format = new Extensible
        {
            FormatTag = FormatExtensible,
            Channels = (short)channels,
            SamplesPerSecond = sampleRate,
            AverageBytesPerSecond = sampleRate * channels * 4,
            BlockAlign = (short)(channels * 4),
            BitsPerSample = 32,
            Size = 22,
            ValidBitsPerSample = 32,
            ChannelMask = channels == 1 ? 0x4 : 0x3,
            SubType = SubtypeIeeeFloat
        };

        var bytes = new byte[Marshal.SizeOf<Extensible>()];
        var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);

        try
        {
            Marshal.StructureToPtr(format, handle.AddrOfPinnedObject(), false);
        }
        finally
        {
            handle.Free();
        }

        return bytes;
    }

    /// <summary>
    /// 普通 16 位整数 PCM 的 WAVEFORMATEX。Media Foundation 的 AAC 编码器对这种
    /// 格式的接受度最好，混音结果送进编码器前就按这个精度交出去。
    /// </summary>
    internal static byte[] BuildPcm16(int sampleRate, int channels)
    {
        var bytes = new byte[Marshal.SizeOf<Pcm16>()];
        var handle = GCHandle.Alloc(bytes, GCHandleType.Pinned);

        try
        {
            Marshal.StructureToPtr(
                new Pcm16
                {
                    FormatTag = 1,
                    Channels = (short)channels,
                    SamplesPerSecond = sampleRate,
                    AverageBytesPerSecond = sampleRate * channels * 2,
                    BlockAlign = (short)(channels * 2),
                    BitsPerSample = 16,
                    Size = 0
                },
                handle.AddrOfPinnedObject(),
                false);
        }
        finally
        {
            handle.Free();
        }

        return bytes;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct Pcm16
    {
        public short FormatTag;
        public short Channels;
        public int SamplesPerSecond;
        public int AverageBytesPerSecond;
        public short BlockAlign;
        public short BitsPerSample;
        public short Size;
    }
}
