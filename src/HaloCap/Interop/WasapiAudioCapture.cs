using System;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;

namespace HaloCap.Interop;

/// <summary>音频数据流方向：播放端用于系统声音 loopback，录音端是麦克风。</summary>
internal enum AudioFlow
{
    Render = 0,
    Capture = 1
}

/// <summary>设备不存在、被隐私设置拒绝等情况；调用方据此改为静默录制并提示。</summary>
internal sealed class AudioUnavailableException : Exception
{
    internal AudioUnavailableException(string message, int hr)
        : base(message)
    {
        HResult = hr;
        ErrorCode = hr;
    }

    internal int ErrorCode { get; }
}

/// <summary>
/// WASAPI 共享模式采集的一路输入：系统声音用 loopback 标志，麦克风用普通采集。
/// 只手写用到的 COM 虚表槽位，不引入音频依赖库。
/// </summary>
internal sealed class WasapiAudioCapture : IDisposable
{
    private const int ClsctxInprocServer = 0x00000001;
    private const uint ShareModeShared = 0;
    private const uint StreamFlagsLoopback = 0x00020000;
    private const uint StreamFlagsAutoConvertPcm = 0x80000000;
    private const uint StreamFlagsSrcDefaultQuality = 0x08000000;

    private const int SlotGetDefaultAudioEndpoint = 4;
    private const int SlotActivate = 3;
    private const int SlotInitialize = 3;
    private const int SlotGetBufferSize = 4;
    private const int SlotGetMixFormat = 8;
    private const int SlotStart = 10;
    private const int SlotStop = 11;
    private const int SlotGetService = 14;
    private const int SlotGetBuffer = 3;
    private const int SlotReleaseBuffer = 4;
    private const int SlotGetNextPacketSize = 5;

    private const int MaxScratchFrames = 10_240;
    private const int AudclntEDeviceInvalidated = unchecked((int)0x888A0004);
    private const int AudclntEDeviceInUse = unchecked((int)0x888A0005);
    private const int AccessDenied = unchecked((int)0x80070005);

    private static readonly Guid ClsidMmDeviceEnumerator =
        new("BCDE0395-E52F-467C-8E3D-C4579291692E");

    private static readonly Guid IidImmDeviceEnumerator =
        new("A95664D2-9614-4F35-A746-DE8DB63617E6");

    private static readonly Guid IidAudioClient =
        new("1CB9AD4C-DBFA-4C32-B178-C2F568A703B2");

    private static readonly Guid IidAudioCaptureClient =
        new("C8ADBD64-E71E-48A0-A4DE-185C395CD317");

    private IntPtr _client = IntPtr.Zero;
    private IntPtr _captureClient = IntPtr.Zero;
    private GCHandle _formatHandle;
    private byte[] _formatBytes = Array.Empty<byte>();
    private bool _started;
    private bool _disposed;

    private WasapiAudioCapture()
    {
    }

    internal int SampleRate { get; private set; } = 48_000;

    internal int Channels { get; private set; } = 2;

    internal int BitsPerSample { get; private set; } = 32;

    internal bool IsFloat { get; private set; } = true;

    /// <summary>设备实际接受的格式，决定混音端要不要做转换。</summary>
    internal bool NeedsConversion => SampleRate != 48_000 || Channels != 2;

    /// <summary>按设备格式交付的交错 float 采样，长度足够放下一个最大包。</summary>
    internal float[] Samples { get; private set; } = Array.Empty<float>();

    internal int MaxFrames { get; private set; }

    internal static WasapiAudioCapture Open(AudioFlow flow, bool loopback)
    {
        var enumerator = IntPtr.Zero;
        var device = IntPtr.Zero;

        try
        {
            var clsid = ClsidMmDeviceEnumerator;
            var enumeratorIid = IidImmDeviceEnumerator;
            Check(
                CoCreateInstance(ref clsid, IntPtr.Zero, ClsctxInprocServer, ref enumeratorIid, out enumerator),
                "创建音频设备枚举器");

            Check(Invoke<GetDefaultEndpointDelegate>(enumerator, SlotGetDefaultAudioEndpoint)(
                    enumerator,
                    (int)flow,
                    0,
                    out device),
                flow == AudioFlow.Render ? "获取默认播放设备" : "获取默认录音设备");

            var clientIid = IidAudioClient;
            Check(Invoke<ActivateDelegate>(device, SlotActivate)(
                    device,
                    ref clientIid,
                    ClsctxInprocServer,
                    IntPtr.Zero,
                    out var client),
                "激活音频客户端");

            var capture = new WasapiAudioCapture();

            try
            {
                capture.Initialize(client, loopback);
            }
            catch
            {
                capture.Dispose();
                throw;
            }

            return capture;
        }
        finally
        {
            if (device != IntPtr.Zero)
            {
                Marshal.Release(device);
            }

            if (enumerator != IntPtr.Zero)
            {
                Marshal.Release(enumerator);
            }
        }
    }

    private void Initialize(IntPtr client, bool loopback)
    {
        _client = client;
        _formatBytes = WaveFormatHelper.BuildIeeeFloat(48_000, 2);
        var hr = InitializeWithPinnedFormat(loopback);

        if (hr < 0 && !TryFallbackToMixFormat(loopback, out hr))
        {
            Check(hr, "初始化音频采集");
        }

        Check(Invoke<GetBufferSizeDelegate>(_client, SlotGetBufferSize)(_client, out var bufferFrames), "读取音频缓冲大小");
        MaxFrames = Math.Clamp((int)bufferFrames, 1_024, MaxScratchFrames);
        Samples = new float[MaxFrames * Channels];

        var captureIid = IidAudioCaptureClient;
        Check(
            Invoke<GetServiceDelegate>(_client, SlotGetService)(_client, ref captureIid, out _captureClient),
            "获取音频采集客户端");
    }

    private int InitializeWithPinnedFormat(bool loopback)
    {
        ReleaseFormat();
        _formatHandle = GCHandle.Alloc(_formatBytes, GCHandleType.Pinned);

        var flags = StreamFlagsAutoConvertPcm | StreamFlagsSrcDefaultQuality;

        if (loopback)
        {
            flags |= StreamFlagsLoopback;
        }

        var hr = Invoke<InitializeDelegate>(_client, SlotInitialize)(
            _client,
            ShareModeShared,
            flags,
            0L,
            0L,
            _formatHandle.AddrOfPinnedObject(),
            IntPtr.Zero);

        if (hr < 0)
        {
            ReleaseFormat();
        }

        return hr;
    }

    private bool TryFallbackToMixFormat(bool loopback, out int hr)
    {
        hr = Invoke<GetMixFormatDelegate>(_client, SlotGetMixFormat)(_client, out var mixPointer);

        if (hr < 0 || mixPointer == IntPtr.Zero)
        {
            return false;
        }

        try
        {
            var header = Marshal.PtrToStructure<WaveFormatEx>(mixPointer);

            if (header.Channels <= 0 || header.SamplesPerSecond <= 0 || header.BitsPerSample <= 0)
            {
                return false;
            }

            var size = header.FormatTag == WaveFormatHelper.FormatExtensible
                ? Marshal.SizeOf<WaveFormatHelper.Extensible>()
                : Marshal.SizeOf<WaveFormatEx>();

            _formatBytes = new byte[size];
            Marshal.Copy(mixPointer, _formatBytes, 0, size);
        }
        finally
        {
            CoTaskMemFree(mixPointer);
        }

        hr = InitializeWithPinnedFormat(loopback);

        if (hr < 0)
        {
            return false;
        }

        ApplyFormat();
        Log.Info($"音频设备改用设备格式 {SampleRate}Hz/{Channels}ch {(IsFloat ? "float" : "PCM")}。");
        return true;
    }

    private void ApplyFormat()
    {
        var pointer = GCHandle.ToIntPtr(_formatHandle);
        var header = Marshal.PtrToStructure<WaveFormatEx>(pointer);
        var isFloat = header.FormatTag == 3;

        if (header.FormatTag == WaveFormatHelper.FormatExtensible &&
            _formatBytes.Length >= Marshal.SizeOf<WaveFormatHelper.Extensible>())
        {
            var ext = Marshal.PtrToStructure<WaveFormatHelper.Extensible>(pointer);
            isFloat = ext.SubType.Equals(WaveFormatHelper.SubtypeIeeeFloat);
        }

        SampleRate = header.SamplesPerSecond;
        Channels = Math.Max(1, (int)header.Channels);
        BitsPerSample = header.BitsPerSample;
        IsFloat = isFloat;
    }

    internal void Start()
    {
        if (_started || _disposed)
        {
            return;
        }

        Check(Invoke<SimpleDelegate>(_client, SlotStart)(_client), "启动音频采集");
        _started = true;
    }

    /// <summary>
    /// 轮询一个包，采样已写入 <see cref="Samples"/>。返回 false 表示暂无数据；
    /// <paramref name="deviceLost"/> 为 true 时说明设备已不可用。
    /// </summary>
    internal bool TryRead(out int frames, out long qpcPosition100Ns, out bool deviceLost)
    {
        frames = 0;
        qpcPosition100Ns = 0;
        deviceLost = false;

        if (_disposed || _captureClient == IntPtr.Zero)
        {
            return false;
        }

        var hr = Invoke<GetNextPacketSizeDelegate>(_captureClient, SlotGetNextPacketSize)(_captureClient, out var pending);

        if (hr < 0)
        {
            deviceLost = IsDeviceLost(hr);
            return false;
        }

        if (pending == 0)
        {
            return false;
        }

        hr = Invoke<GetBufferDelegate>(_captureClient, SlotGetBuffer)(
            _captureClient,
            out var data,
            out var obtained,
            out _,
            out _,
            out var qpc);

        if (hr < 0)
        {
            deviceLost = IsDeviceLost(hr);
            return false;
        }

        try
        {
            var usable = Math.Min((int)obtained, MaxFrames);
            CopyToFloat(data, usable);
            frames = usable;
            qpcPosition100Ns = (long)qpc;
        }
        finally
        {
            Invoke<ReleaseBufferDelegate>(_captureClient, SlotReleaseBuffer)(_captureClient, obtained);
        }

        return true;
    }

    private void CopyToFloat(IntPtr data, int frames)
    {
        var count = frames * Channels;

        if (count <= 0)
        {
            return;
        }

        if (IsFloat && BitsPerSample == 32)
        {
            Marshal.Copy(data, Samples, 0, count);
            return;
        }

        if (BitsPerSample == 16)
        {
            var shorts = new short[count];
            Marshal.Copy(data, shorts, 0, count);

            for (var i = 0; i < count; i++)
            {
                Samples[i] = shorts[i] / 32768f;
            }

            return;
        }

        if (BitsPerSample == 32)
        {
            var ints = new int[count];
            Marshal.Copy(data, ints, 0, count);

            for (var i = 0; i < count; i++)
            {
                Samples[i] = ints[i] / 2147483648f;
            }

            return;
        }

        // 其它位深（如 24 位）按静音处理，不拖累视频录制。
        Array.Clear(Samples, 0, count);
    }

    private static bool IsDeviceLost(int hr) =>
        hr is AudclntEDeviceInvalidated or AudclntEDeviceInUse;

    private void ReleaseFormat()
    {
        if (_formatHandle.IsAllocated)
        {
            _formatHandle.Free();
        }
    }

    private static T Invoke<T>(IntPtr comObject, int slot) where T : class
    {
        var vtable = Marshal.ReadIntPtr(comObject);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
    }

    private static void Check(int hr, string operation)
    {
        if (hr >= 0)
        {
            return;
        }

        if (hr == AccessDenied)
        {
            throw new AudioUnavailableException("系统当前禁止访问该音频设备。", hr);
        }

        throw new AudioUnavailableException($"{operation}失败（HRESULT 0x{hr:X8}）。", hr);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        if (_started && _client != IntPtr.Zero)
        {
            try
            {
                Invoke<SimpleDelegate>(_client, SlotStop)(_client);
            }
            catch
            {
                // 停止失败不影响释放。
            }
        }

        ReleaseFormat();

        if (_captureClient != IntPtr.Zero)
        {
            Marshal.Release(_captureClient);
            _captureClient = IntPtr.Zero;
        }

        if (_client != IntPtr.Zero)
        {
            Marshal.Release(_client);
            _client = IntPtr.Zero;
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct WaveFormatEx
    {
        public short FormatTag;
        public short Channels;
        public int SamplesPerSecond;
        public int AverageBytesPerSecond;
        public short BlockAlign;
        public short BitsPerSample;
        public short Size;
    }

    [DllImport("ole32.dll", ExactSpelling = true)]
    private static extern int CoCreateInstance(
        ref Guid rclsid,
        IntPtr pUnkOuter,
        int dwClsContext,
        ref Guid riid,
        out IntPtr ppv);

    [DllImport("ole32.dll", ExactSpelling = true)]
    private static extern void CoTaskMemFree(IntPtr pointer);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int SimpleDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetDefaultEndpointDelegate(
        IntPtr self,
        int dataFlow,
        int role,
        out IntPtr device);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int ActivateDelegate(
        IntPtr self,
        ref Guid iid,
        int clsCtx,
        IntPtr activationParams,
        out IntPtr interfacePointer);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int InitializeDelegate(
        IntPtr self,
        uint shareMode,
        uint streamFlags,
        long bufferDuration,
        long periodicity,
        IntPtr format,
        IntPtr sessionGuid);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetBufferSizeDelegate(IntPtr self, out uint frames);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetMixFormatDelegate(IntPtr self, out IntPtr format);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetServiceDelegate(IntPtr self, ref Guid iid, out IntPtr service);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetNextPacketSizeDelegate(IntPtr self, out uint frames);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetBufferDelegate(
        IntPtr self,
        out IntPtr data,
        out uint frames,
        out uint flags,
        out ulong devicePosition,
        out ulong qpcPosition);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int ReleaseBufferDelegate(IntPtr self, uint frames);
}
