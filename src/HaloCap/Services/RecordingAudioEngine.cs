using System;
using System.Threading;
using System.Threading.Tasks;
using HaloCap.Diagnostics;
using HaloCap.Interop;

namespace HaloCap.Services;

/// <summary>一次交付给编码器的音频采样：48kHz 立体声 float 交错数据。</summary>
internal sealed class AudioPacket
{
    internal AudioPacket(float[] samples, int frames, long time100Ns)
    {
        Samples = samples;
        Frames = frames;
        Time100Ns = time100Ns;
    }

    internal float[] Samples { get; }

    internal int Frames { get; }

    internal long Time100Ns { get; }
}

/// <summary>
/// 录屏音频引擎：在同一个线程里轮询系统声音 loopback 与麦克风，混音成
/// 48kHz 立体声后按 1024 帧一块交给编码器。发送节奏跟随录制时钟，
/// 所以没有声音时仍会补静音、暂停时一起停住，音画不会漂开。
/// </summary>
internal sealed class RecordingAudioEngine : IDisposable
{
    private const int FramesPerSample = 1024;
    private const int RingFrames = 8192;
    private const int MaxCatchUpSamples = 32;
    private const int LevelRefreshSamples = 16;

    internal const long SampleDuration100Ns =
        10_000_000L * FramesPerSample / AudioFormatConverter.TargetSampleRate;

    private readonly bool _wantSystemAudio;
    private readonly bool _wantMicrophone;
    private readonly Func<long> _clockNow100Ns;
    private readonly Action<AudioPacket> _emit;
    private readonly Source?[] _sources = new Source?[2];
    private readonly float[] _mixBuffer = new float[FramesPerSample * 2];
    private readonly float[] _scratchBuffer = new float[FramesPerSample * 2];
    private readonly TaskCompletionSource<bool> _sourcesOpened =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private Thread? _thread;
    private long _nextSampleTime100Ns;
    private int _samplesSinceLevelRefresh;
    private int _resumePending;
    private volatile bool _paused;
    private float _systemLevel;
    private float _micLevel;
    private volatile bool _running;
    private bool _disposed;

    internal RecordingAudioEngine(
        bool wantSystemAudio,
        bool wantMicrophone,
        Func<long> clockNow100Ns,
        Action<AudioPacket> emit)
    {
        _wantSystemAudio = wantSystemAudio;
        _wantMicrophone = wantMicrophone;
        _clockNow100Ns = clockNow100Ns;
        _emit = emit;
    }

    /// <summary>某一路音频不可用时的一次性提示，参数是面向用户的说明。</summary>
    internal event Action<string>? SourceFailed;

    internal bool WantsAudio => _wantSystemAudio || _wantMicrophone;

    internal float SystemLevel => Volatile.Read(ref _systemLevel);

    internal float MicLevel => Volatile.Read(ref _micLevel);

    /// <summary>这一路现在是否真的在采集；设备失败后电平条就该收起。</summary>
    internal bool HasSystemSource => _wantSystemAudio && Volatile.Read(ref _sources[0]) is not null;

    internal bool HasMicrophoneSource => _wantMicrophone && Volatile.Read(ref _sources[1]) is not null;

    /// <summary>
    /// 启动采集线程并等到设备开完。返回是否真的至少开成一路：
    /// 调用方据此决定 MP4 里要不要放音轨，免得录完才发现是一空轨。
    /// </summary>
    internal Task<bool> StartAndOpenAsync()
    {
        if (_disposed || _running || !WantsAudio)
        {
            return Task.FromResult(false);
        }

        _running = true;
        _thread = new Thread(Run)
        {
            IsBackground = true,
            Name = "HaloCap.Audio"
        };
        _thread.SetApartmentState(ApartmentState.MTA);
        _thread.Start();
        return _sourcesOpened.Task;
    }

    internal void Stop(TimeSpan timeout)
    {
        _running = false;

        try
        {
            _thread?.Join(timeout);
        }
        catch (ThreadStateException)
        {
            // 线程可能尚未启动。
        }

        _thread = null;
        _sourcesOpened.TrySetResult(false);
    }

    /// <summary>
    /// 跟随录制的暂停/恢复：暂停时继续把设备数据读出来丢掉，免得恢复后先把积压的
    /// 旧声音吐出来；恢复时把发送时钟对回当前时间，静音也不会补发。
    /// </summary>
    internal void SetPaused(bool paused)
    {
        _paused = paused;

        if (!paused)
        {
            Interlocked.Exchange(ref _resumePending, 1);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Stop(TimeSpan.FromSeconds(2));
        CloseSources();
    }

    private void Run()
    {
        var comInitialized = NativeMethods.CoInitializeEx(IntPtr.Zero, 0) >= 0;

        try
        {
            OpenSources();
            _sourcesOpened.TrySetResult(HasSystemSource || HasMicrophoneSource);
            _nextSampleTime100Ns = _clockNow100Ns();

            while (_running && !_disposed)
            {
                if (Interlocked.Exchange(ref _resumePending, 0) != 0)
                {
                    _nextSampleTime100Ns = Math.Max(_nextSampleTime100Ns, _clockNow100Ns());
                }

                var pulled = Pump();

                if (!_paused)
                {
                    EmitDueSamples();
                }

                if (!pulled)
                {
                    Thread.Sleep(5);
                }
            }

            if (!_paused)
            {
                EmitDueSamples();
            }
        }
        catch (Exception ex)
        {
            Log.Error("录屏音频线程异常", ex);
        }
        finally
        {
            _sourcesOpened.TrySetResult(false);
            CloseSources();

            if (comInitialized)
            {
                NativeMethods.CoUninitialize();
            }
        }
    }

    private void OpenSources()
    {
        if (_wantSystemAudio)
        {
            _sources[0] = TryOpen("系统声音", AudioFlow.Render, true, 0);
        }

        if (_wantMicrophone)
        {
            _sources[1] = TryOpen("麦克风", AudioFlow.Capture, false, 1);
        }
    }

    private Source? TryOpen(string label, AudioFlow flow, bool loopback, int index)
    {
        try
        {
            var capture = WasapiAudioCapture.Open(flow, loopback);
            capture.Start();
            Log.Info($"录屏音频已启用：{label} {capture.SampleRate}Hz/{capture.Channels}ch。");
            return new Source(label, capture);
        }
        catch (Exception ex)
        {
            var reason = ex is AudioUnavailableException unavailable
                ? unavailable.Message
                : ex.Message;

            Log.Error($"启用录屏音频失败（{label}）", ex);
            SourceFailed?.Invoke($"{label}不可用：{reason} 录屏会继续，但没有这路声音。");
            return null;
        }
    }

    private bool Pump()
    {
        var any = false;

        for (var i = 0; i < _sources.Length; i++)
        {
            if (_sources[i] is not { } source)
            {
                continue;
            }

            while (true)
            {
                if (!source.Capture.TryRead(out var frames, out _, out var lost))
                {
                    if (lost)
                    {
                        FailSource(i, source.Label, "设备已断开或被占用");
                    }

                    break;
                }

                if (frames > 0 && !_paused)
                {
                    source.Write(frames);
                    any = true;
                }
            }
        }

        return any;
    }

    private void FailSource(int index, string label, string why)
    {
        if (_sources[index] is not { } source)
        {
            return;
        }

        _sources[index] = null;
        source.Dispose();
        Log.Error($"{label}采集中断：{why}");
        SourceFailed?.Invoke($"{label}中断：{why} 录屏会继续，但没有这路声音。");
    }

    private void EmitDueSamples()
    {
        var now = _clockNow100Ns();
        var guard = 0;

        while (_nextSampleTime100Ns + SampleDuration100Ns <= now && guard < MaxCatchUpSamples)
        {
            EmitSample(_nextSampleTime100Ns);
            _nextSampleTime100Ns += SampleDuration100Ns;
            guard++;
        }

        if (guard >= MaxCatchUpSamples)
        {
            // 明显跟不上时钟时重新对齐，宁可留下静音也不要把时间轴越拖越长。
            _nextSampleTime100Ns = now;
        }
    }

    private void EmitSample(long time100Ns)
    {
        Array.Clear(_mixBuffer, 0, _mixBuffer.Length);
        var systemPeak = 0f;
        var micPeak = 0f;
        var anySource = false;

        for (var i = 0; i < _sources.Length; i++)
        {
            if (_sources[i] is not { } source)
            {
                continue;
            }

            anySource = true;

            // 先单独读出这一路，电平才不会把另一路的声音算进来。
            Array.Clear(_scratchBuffer, 0, _scratchBuffer.Length);
            source.Ring.ReadInto(_scratchBuffer, FramesPerSample);

            var level = AudioFormatConverter.PeakLevel(_scratchBuffer, FramesPerSample, 2);

            if (i == 0)
            {
                systemPeak = level;
            }
            else
            {
                micPeak = level;
            }

            MixInto(_scratchBuffer);
        }

        if (!anySource)
        {
            return;
        }

        var samples = new float[_mixBuffer.Length];
        Array.Copy(_mixBuffer, samples, _mixBuffer.Length);
        _emit(new AudioPacket(samples, FramesPerSample, Math.Max(0, time100Ns)));
        UpdateLevels(systemPeak, micPeak);
    }

    private void MixInto(float[] source)
    {
        for (var i = 0; i < _mixBuffer.Length; i++)
        {
            var value = _mixBuffer[i] + source[i];
            _mixBuffer[i] = value > 1f ? 1f : value < -1f ? -1f : value;
        }
    }

    private void UpdateLevels(float systemPeak, float micPeak)
    {
        _samplesSinceLevelRefresh++;

        if (_samplesSinceLevelRefresh >= LevelRefreshSamples)
        {
            _samplesSinceLevelRefresh = 0;
            Volatile.Write(ref _systemLevel, systemPeak);
            Volatile.Write(ref _micLevel, micPeak);
            return;
        }

        if (systemPeak > Volatile.Read(ref _systemLevel))
        {
            Volatile.Write(ref _systemLevel, systemPeak);
        }

        if (micPeak > Volatile.Read(ref _micLevel))
        {
            Volatile.Write(ref _micLevel, micPeak);
        }
    }

    private void CloseSources()
    {
        for (var i = 0; i < _sources.Length; i++)
        {
            _sources[i]?.Dispose();
            _sources[i] = null;
        }
    }

    private sealed class Source : IDisposable
    {
        private readonly float[] _carry;
        private float[] _converted = new float[RingFrames * 2];
        private double _readOffset;

        internal Source(string label, WasapiAudioCapture capture)
        {
            Label = label;
            Capture = capture;
            Ring = new FrameRing(RingFrames);
            _carry = new float[Math.Max(1, capture.Channels)];
        }

        internal string Label { get; }

        internal WasapiAudioCapture Capture { get; }

        internal FrameRing Ring { get; }

        /// <summary>把设备刚交付的包转成 48kHz 立体声后写入环形缓冲。</summary>
        internal void Write(int frames)
        {
            var source = Capture.Samples;
            var usable = Math.Min(frames, source.Length / Math.Max(1, Capture.Channels));

            if (usable <= 0)
            {
                return;
            }

            if (!Capture.NeedsConversion)
            {
                Ring.Write(source, usable);
                return;
            }

            var wanted = (int)Math.Ceiling(usable * (AudioFormatConverter.TargetSampleRate / (double)Capture.SampleRate)) + 2;

            if (_converted.Length < wanted * 2)
            {
                _converted = new float[wanted * 2];
            }

            var written = AudioFormatConverter.ResampleTo48k(
                source,
                usable,
                Capture.Channels,
                Capture.SampleRate,
                _converted,
                wanted,
                _carry,
                ref _readOffset);

            if (written > 0)
            {
                Ring.Write(_converted, written);
            }
        }

        public void Dispose() => Capture.Dispose();
    }
}

/// <summary>
/// 录制音频用的帧环形缓冲：只有一个线程读写，因此不需要加锁；写满时丢最旧的帧保持实时。
/// </summary>
internal sealed class FrameRing
{
    private readonly float[] _buffer;
    private readonly int _capacity;
    private int _head;
    private int _tail;

    internal FrameRing(int capacityFrames)
    {
        _capacity = Math.Max(2, capacityFrames);
        _buffer = new float[_capacity * 2];
    }

    internal int Count => _tail >= _head ? _tail - _head : (_capacity - _head) + _tail;

    internal void Write(float[] samples, int frames)
    {
        var available = Math.Min(frames, samples.Length / 2);

        for (var frame = 0; frame < available; frame++)
        {
            _buffer[_tail * 2] = samples[frame * 2];
            _buffer[(_tail * 2) + 1] = samples[(frame * 2) + 1];
            _tail = Next(_tail);

            if (_tail == _head)
            {
                _head = Next(_head);
            }
        }
    }

    /// <summary>把数据叠加到 destination 上，不足部分保持静音。</summary>
    internal void ReadInto(float[] destination, int frames)
    {
        var limit = Math.Min(frames, destination.Length / 2);

        for (var frame = 0; frame < limit; frame++)
        {
            if (_head == _tail)
            {
                return;
            }

            var left = destination[frame * 2] + _buffer[_head * 2];
            var right = destination[(frame * 2) + 1] + _buffer[(_head * 2) + 1];
            destination[frame * 2] = Clamp(left);
            destination[(frame * 2) + 1] = Clamp(right);
            _head = Next(_head);
        }
    }

    private int Next(int index) => (index + 1) % _capacity;

    private static float Clamp(float value) => value > 1f ? 1f : value < -1f ? -1f : value;
}
