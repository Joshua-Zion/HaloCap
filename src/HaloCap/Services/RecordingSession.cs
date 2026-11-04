using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using Windows.Foundation.Metadata;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;

namespace HaloCap.Services;

/// <summary>
/// 一次录屏的采集与编码会话。视频在 GPU 上按选区裁剪后再回到 CPU，音频在独立线程
/// 轮询系统声音与麦克风；两条流共用同一个录制时钟打时间戳，所以暂停/恢复后仍然对齐。
/// </summary>
internal sealed class RecordingSession : IDisposable
{
    private const long TicksPerSecond = 10_000_000L;
    private const int MaxQueueDepth = 3;
    private const int LargeFrameThresholdBytes = 12_000_000;
    private const int MaxQueuedAudioSamples = 90;
    private const int AudioIdleSleepMs = 2;
    private const int AudioOpenTimeoutMs = 2_000;

    private readonly GraphicsCaptureItem _item;
    private readonly string _path;
    private readonly RecordingPlan _plan;
    private readonly Func<PhysicalRect> _cropResolver;
    private readonly bool _isWindow;
    private readonly TaskCompletionSource<bool> _encoderReady =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource<Exception?> _encoderCompleted =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly BlockingCollection<int> _videoQueue = new();
    private readonly BlockingCollection<AudioPacket> _audioQueue = new();
    private readonly ConcurrentQueue<int> _freeBuffers = new();
    private readonly Stopwatch _clock = new();
    private readonly object _stopLock = new();
    private readonly object _captureLock = new();
    private readonly long _frameInterval;

    private byte[][]? _frameBuffers;
    private long[]? _frameTimestamps;
    private byte[]? _cropScratch;
    private int _cropStride;
    private PhysicalRect _crop;
    private int _surfaceWidth;
    private int _surfaceHeight;
    private int _frameBytes;
    private int _queueCapacity;
    private int _bufferCount;
    private D3D11CropCopier? _copier;
    private Direct3DDeviceHolder? _deviceHolder;
    private Direct3D11CaptureFramePool? _framePool;
    private GraphicsCaptureSession? _captureSession;
    private RecordingAudioEngine? _audio;
    private Thread? _encoderThread;
    private Task<string>? _stopTask;
    private int _frameBusy;
    private int _reconfigureScheduled;
    private int _encodedFrames;
    private int _audioSamplesWritten;
    private long _droppedFrames;
    private long _droppedAudioSamples;
    private int _refreshCount;
    private long _lastAcceptedTimestamp = -1;
    private long _baseTime100Ns = -1;
    private long _pausedTotal;
    private long _pauseStartedAt = -1;
    private long _stoppedTimestamp = -1;
    private bool _captureStarted;
    private bool _encoderThreadStarted;
    private bool _muxAudio;
    private bool _audioEnabledByWriter;
    private bool _disposed;
    private volatile bool _stopping;
    private volatile bool _isPaused;

    internal RecordingSession(
        GraphicsCaptureItem item,
        string path,
        RecordingPlan plan,
        Func<PhysicalRect> cropResolver,
        bool isWindow)
    {
        _item = item;
        _path = path;
        _plan = plan;
        _cropResolver = cropResolver;
        _isWindow = isWindow;
        _frameInterval = TicksPerSecond / Math.Max(1, plan.FramesPerSecond);

        if (string.IsNullOrEmpty(path))
        {
            throw new ArgumentException("录制文件路径不能为空。", nameof(path));
        }

        if (plan.OutputWidth < 16 || plan.OutputHeight < 16)
        {
            throw new InvalidOperationException("录制输出尺寸太小，请重新选择区域。");
        }

        if ((plan.OutputWidth & 1) != 0 || (plan.OutputHeight & 1) != 0)
        {
            throw new InvalidOperationException("录制输出尺寸必须为偶数。");
        }

        _frameBytes = checked(plan.OutputWidth * plan.OutputHeight * 4);
        _queueCapacity = _frameBytes > LargeFrameThresholdBytes ? 2 : MaxQueueDepth;
        _bufferCount = _queueCapacity + 1;
    }

    internal event Action<Exception>? Faulted;

    internal event Action? CaptureItemClosed;

    /// <summary>某一路音频不可用时的一次性提示。</summary>
    internal event Action<string>? AudioUnavailable;

    internal string OutputPath => _path;

    internal bool IsPaused => _isPaused;

    internal int OutputWidth => _plan.OutputWidth;

    internal int OutputHeight => _plan.OutputHeight;

    internal int FramesPerSecond => _plan.FramesPerSecond;

    internal int EncodedFrames => _encodedFrames;

    internal PhysicalRect CropRegion => _crop;

    internal long DroppedFrames => Interlocked.Read(ref _droppedFrames);

    internal int AudioSamplesWritten => Volatile.Read(ref _audioSamplesWritten);

    internal long DroppedAudioSamples => Interlocked.Read(ref _droppedAudioSamples);

    internal bool UsesHardwareEncoder { get; private set; }

    /// <summary>音频是否真的进了编码器（设备与 AAC 编码器都可用时才是 true）。</summary>
    internal bool AudioEncoded => _audioEnabledByWriter && Volatile.Read(ref _audioSamplesWritten) > 0;

    internal float SystemAudioLevel => _audio?.SystemLevel ?? 0f;

    internal float MicrophoneLevel => _audio?.MicLevel ?? 0f;

    internal bool SystemAudioActive => _audioEnabledByWriter && (_audio?.HasSystemSource ?? false);

    internal bool MicrophoneActive => _audioEnabledByWriter && (_audio?.HasMicrophoneSource ?? false);

    /// <summary>录制源尺寸变化后重建捕获的次数。</summary>
    internal int RefreshCount => Volatile.Read(ref _refreshCount);

    /// <summary>不含暂停时长的已录制时间，单位 100 纳秒。</summary>
    internal long ActiveElapsed100Ns
    {
        get
        {
            var stopped = Interlocked.Read(ref _stoppedTimestamp);

            if (stopped >= 0)
            {
                return Math.Max(0, stopped);
            }

            return !_clock.IsRunning ? 0 : Math.Max(0, ReadTimestamp());
        }
    }

    internal async Task StartAsync()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(RecordingSession));
        }

        if (_captureStarted)
        {
            return;
        }

        try
        {
            if (!GraphicsCaptureSession.IsSupported())
            {
                throw new InvalidOperationException("当前系统或显卡驱动不支持 Windows 屏幕捕获。");
            }

            _deviceHolder = WinRtCaptureInterop.CreateDirect3DDeviceHolder();
            _copier = new D3D11CropCopier(_deviceHolder.NativeDevice, _deviceHolder.NativeContext);

            AllocateBuffers();

            if (_plan.WantsAudio)
            {
                _muxAudio = await OpenAudioAsync().ConfigureAwait(true);
            }

            StartEncoderThread();
            await _encoderReady.Task.ConfigureAwait(true);

            if (_disposed || _stopping)
            {
                throw new OperationCanceledException("录制已取消。");
            }

            _clock.Start();

            lock (_captureLock)
            {
                StartCapture();
            }

            _item.Closed += OnItemClosed;
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    internal void Pause()
    {
        if (_disposed || _stopping || _isPaused)
        {
            return;
        }

        Interlocked.Exchange(ref _pauseStartedAt, ReadRawTimestamp());
        _isPaused = true;
        _audio?.SetPaused(true);
    }

    internal void Resume()
    {
        if (!_isPaused)
        {
            return;
        }

        var pauseStartedAt = Interlocked.Read(ref _pauseStartedAt);

        if (pauseStartedAt >= 0)
        {
            Interlocked.Add(ref _pausedTotal, ReadRawTimestamp() - pauseStartedAt);
        }

        Interlocked.Exchange(ref _pauseStartedAt, -1);
        _isPaused = false;
        _lastAcceptedTimestamp = -1;
        _audio?.SetPaused(false);
    }

    /// <summary>录制源尺寸变化时由服务端轮询调用，重建捕获管线。</summary>
    internal void RefreshIfSourceResized()
    {
        if (_disposed || _stopping || Volatile.Read(ref _reconfigureScheduled) != 0)
        {
            return;
        }

        var size = _item.Size;

        if (size.Width == _surfaceWidth && size.Height == _surfaceHeight)
        {
            return;
        }

        ScheduleReconfigure();
    }

    internal Task<string> StopAsync()
    {
        lock (_stopLock)
        {
            return _stopTask ??= StopCoreAsync();
        }
    }

    private async Task<string> StopCoreAsync()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(RecordingSession));
        }

        _stopping = true;

        if (_isPaused)
        {
            Resume();
        }

        MarkStopped();

        lock (_captureLock)
        {
            DisposeCapture();
        }

        _clock.Stop();
        StopAudio();

        try
        {
            _videoQueue.CompleteAdding();
            _audioQueue.CompleteAdding();
        }
        catch (ObjectDisposedException)
        {
            // 编码线程已经结束。
        }

        var timeout = Task.Delay(TimeSpan.FromSeconds(25));
        var finished = await Task.WhenAny(_encoderCompleted.Task, timeout).ConfigureAwait(false);

        if (finished != _encoderCompleted.Task)
        {
            throw new InvalidOperationException("保存录制文件超时，录制已停止。");
        }

        var error = await _encoderCompleted.Task.ConfigureAwait(false);

        try
        {
            _encoderThread?.Join(2000);
        }
        catch (ThreadStateException)
        {
            // 编码线程可能尚未启动。
        }

        if (error is not null)
        {
            TryDeleteFile();
            throw new InvalidOperationException("录制文件保存失败：" + error.Message, error);
        }

        var info = new FileInfo(_path);

        if (!info.Exists || info.Length == 0)
        {
            TryDeleteFile();
            throw new InvalidOperationException("录制文件为空，未能保存。");
        }

        return _path;
    }

    private void AllocateBuffers()
    {
        _frameBuffers = new byte[_bufferCount][];
        _frameTimestamps = new long[_bufferCount];

        for (var index = 0; index < _bufferCount; index++)
        {
            _frameBuffers[index] = new byte[_frameBytes];
            _freeBuffers.Enqueue(index);
        }
    }

    private void StartEncoderThread()
    {
        _encoderThread = new Thread(EncoderLoop)
        {
            IsBackground = true,
            Name = "HaloCap.MediaEncoder"
        };
        _encoderThread.SetApartmentState(ApartmentState.MTA);
        _encoderThreadStarted = true;
        _encoderThread.Start();
    }

    private void EncoderLoop()
    {
        MediaFoundationWriter? writer = null;
        var comInitialized = false;
        var pending = -1;

        try
        {
            var hr = NativeMethods.CoInitializeEx(IntPtr.Zero, 0);
            comInitialized = hr >= 0;

            writer = new MediaFoundationWriter(
                _path,
                _plan.OutputWidth,
                _plan.OutputHeight,
                _plan.FramesPerSecond,
                _plan.Bitrate,
                _muxAudio ? _plan.Audio : null);

            _audioEnabledByWriter = writer.SupportsAudio;

            if (_muxAudio && !writer.SupportsAudio)
            {
                var reason = writer.AudioFailureReason ?? "未知原因";
                Log.Error($"MP4 无法承载 AAC 音频流：{reason} 本次录屏只保留画面。");
                AudioUnavailable?.Invoke($"这台电脑没能创建 AAC 音轨（{reason}），录屏会继续，但视频没有声音。");
            }

            _encoderReady.TrySetResult(true);

            var buffers = _frameBuffers!;
            var timestamps = _frameTimestamps!;

            while (true)
            {
                var progressed = false;

                while (_videoQueue.Count > 0 && _videoQueue.TryTake(out var index, 0))
                {
                    progressed = true;

                    if (pending >= 0)
                    {
                        var duration = Math.Clamp(
                            timestamps[index] - timestamps[pending],
                            10_000L,
                            TicksPerSecond);
                        WriteVideoPacket(writer, buffers, timestamps, pending, duration);
                    }

                    EnsureBaseTime(timestamps[index]);
                    pending = index;
                }

                while (_audioQueue.Count > 0 && _audioQueue.TryTake(out var packet, 0))
                {
                    progressed = true;
                    EnsureBaseTime(packet.Time100Ns);
                    writer.WriteAudioSample(
                        packet.Samples,
                        Math.Max(0, packet.Time100Ns - _baseTime100Ns),
                        RecordingAudioEngine.SampleDuration100Ns);
                    Interlocked.Increment(ref _audioSamplesWritten);
                }

                var videoClosed = _videoQueue.IsCompleted && _videoQueue.Count == 0;
                var audioClosed = _audioQueue.IsCompleted && _audioQueue.Count == 0;

                if (videoClosed && audioClosed)
                {
                    break;
                }

                if (!progressed)
                {
                    Thread.Sleep(AudioIdleSleepMs);
                }
            }

            if (pending >= 0)
            {
                // 静态画面下 WGC 可能长时间不再送帧；用最后一帧覆盖到停止时刻，
                // 保证 MP4 时长与录制时长一致。
                var stopped = Interlocked.Read(ref _stoppedTimestamp);
                var finalTime = timestamps[pending];
                var finalDuration = stopped > finalTime
                    ? Math.Max(_frameInterval, stopped - finalTime)
                    : _frameInterval;

                WriteVideoPacket(writer, buffers, timestamps, pending, finalDuration);
                pending = -1;
            }

            if (_encodedFrames == 0)
            {
                throw new InvalidOperationException("没有捕获到任何画面，请重新选择录制区域后重试。");
            }

            writer.Complete();
            writer = null;
            _encoderCompleted.TrySetResult(null);
        }
        catch (Exception ex)
        {
            _encoderReady.TrySetException(ex);
            _encoderCompleted.TrySetResult(ex);

            if (!_stopping && _encoderReady.Task.Status == TaskStatus.RanToCompletion)
            {
                Faulted?.Invoke(ex);
            }
        }
        finally
        {
            if (pending >= 0)
            {
                _freeBuffers.Enqueue(pending);
            }

            if (writer is not null)
            {
                try
                {
                    writer.Dispose();
                }
                catch
                {
                    // 清理阶段不再抛出。
                }
            }

            if (comInitialized)
            {
                NativeMethods.CoUninitialize();
            }
        }
    }

    private void EnsureBaseTime(long rawTime)
    {
        if (_baseTime100Ns < 0)
        {
            _baseTime100Ns = rawTime;
        }
    }

    private void WriteVideoPacket(
        MediaFoundationWriter writer,
        byte[][] buffers,
        long[] timestamps,
        int index,
        long duration)
    {
        try
        {
            var timestamp = Math.Max(0, timestamps[index] - Math.Max(0, _baseTime100Ns));
            writer.WriteFrame(buffers[index], timestamp, duration);
            _encodedFrames++;
        }
        finally
        {
            _freeBuffers.Enqueue(index);
        }
    }

    private void OnFrameArrived(Direct3D11CaptureFramePool sender, object args)
    {
        if (_disposed || _stopping)
        {
            return;
        }

        if (Interlocked.CompareExchange(ref _frameBusy, 1, 0) != 0)
        {
            return;
        }

        try
        {
            using var frame = sender.TryGetNextFrame();

            if (frame is null)
            {
                return;
            }

            if (_isPaused)
            {
                // 暂停时仍要把帧取出来丢弃，否则帧池缓冲被占满，恢复后拿到的是旧画面。
                return;
            }

            if (!Monitor.TryEnter(_captureLock, TimeSpan.FromMilliseconds(500)))
            {
                return;
            }

            try
            {
                if (_disposed || _stopping || Volatile.Read(ref _reconfigureScheduled) != 0)
                {
                    return;
                }

                var timestamp = ReadTimestamp();

                // 首帧一定要收下：静态画面下 WGC 可能只送这一帧，
                // 按帧距把它挡掉就会出现"整段录制没有画面"。
                if (_lastAcceptedTimestamp >= 0 &&
                    timestamp - _lastAcceptedTimestamp < _frameInterval)
                {
                    return;
                }

                var size = _item.Size;

                if (size.Width != _surfaceWidth || size.Height != _surfaceHeight)
                {
                    ScheduleReconfigure();
                    return;
                }

                if (!TryBuildPacket(frame.Surface, timestamp))
                {
                    return;
                }

                _lastAcceptedTimestamp = timestamp;
            }
            finally
            {
                Monitor.Exit(_captureLock);
            }
        }
        catch (Exception ex)
        {
            if (!_stopping)
            {
                Log.Error("捕获录制画面失败", ex);
                Faulted?.Invoke(ex);
            }
        }
        finally
        {
            Volatile.Write(ref _frameBusy, 0);
        }
    }

    private bool TryBuildPacket(IDirect3DSurface surface, long timestamp)
    {
        var buffers = _frameBuffers;
        var timestamps = _frameTimestamps;
        var copier = _copier;

        if (buffers is null || timestamps is null || copier is null)
        {
            return false;
        }

        if (!_freeBuffers.TryDequeue(out var index))
        {
            Interlocked.Increment(ref _droppedFrames);
            return false;
        }

        try
        {
            var texture = WinRtCaptureInterop.GetNativeTexture(surface);

            try
            {
                FillFrameBuffer(copier, texture, buffers[index]);
            }
            finally
            {
                Marshal.Release(texture);
            }

            timestamps[index] = timestamp;

            if (_videoQueue.IsAddingCompleted)
            {
                _freeBuffers.Enqueue(index);
                return false;
            }

            _videoQueue.Add(index);
            return true;
        }
        catch
        {
            _freeBuffers.Enqueue(index);
            throw;
        }
    }

    private void FillFrameBuffer(D3D11CropCopier copier, IntPtr texture, byte[] destination)
    {
        var destinationStride = _plan.OutputWidth * 4;
        var fit = RecordingFrameScaler.ComputeFitRect(
            _crop.Width,
            _crop.Height,
            _plan.OutputWidth,
            _plan.OutputHeight);
        var coversCanvas = fit.Left == 0 &&
                           fit.Top == 0 &&
                           fit.Width == _plan.OutputWidth &&
                           fit.Height == _plan.OutputHeight;

        if (coversCanvas && fit.Width == _crop.Width && fit.Height == _crop.Height)
        {
            // _crop 自带选区在捕获表面内的偏移，直接按它裁剪才会录到选中的那块区域。
            copier.Copy(texture, _crop, destination, destinationStride);
            return;
        }

        EnsureCropScratch();
        copier.Copy(texture, _crop, _cropScratch!, _cropStride);

        if (coversCanvas)
        {
            RecordingFrameScaler.CopyOrScale(
                _cropScratch!,
                _crop.Width,
                _crop.Height,
                _cropStride,
                destination,
                _plan.OutputWidth,
                _plan.OutputHeight,
                destinationStride,
                0,
                0);
            return;
        }

        RecordingFrameScaler.ClearOutside(
            destination,
            _plan.OutputWidth,
            _plan.OutputHeight,
            fit);

        RecordingFrameScaler.CopyOrScale(
            _cropScratch!,
            _crop.Width,
            _crop.Height,
            _cropStride,
            destination,
            fit.Width,
            fit.Height,
            destinationStride,
            fit.Left,
            fit.Top);
    }

    private void EnsureCropScratch()
    {
        var stride = _crop.Width * 4;
        var length = checked(stride * _crop.Height);

        if (_cropScratch is not null && _cropScratch.Length == length && _cropStride == stride)
        {
            return;
        }

        _cropScratch = new byte[length];
        _cropStride = stride;
    }

    private void StartCapture()
    {
        var size = _item.Size;
        _surfaceWidth = size.Width;
        _surfaceHeight = size.Height;
        ResolveCrop();

        _framePool = Direct3D11CaptureFramePool.CreateFreeThreaded(
            _deviceHolder!.Device,
            DirectXPixelFormat.B8G8R8A8UIntNormalized,
            2,
            size);

        _framePool.FrameArrived += OnFrameArrived;
        _captureSession = _framePool.CreateCaptureSession(_item);
        ConfigureSession(_captureSession);
        _captureSession.StartCapture();
        _captureStarted = true;
    }

    private void ConfigureSession(GraphicsCaptureSession session)
    {
        try
        {
            // Windows 10 2004 才加入此属性；更早版本保持系统默认（包含光标）。
            if (ApiInformation.IsPropertyPresent(
                    "Windows.Graphics.Capture.GraphicsCaptureSession",
                    "IsCursorCaptureEnabled"))
            {
                session.IsCursorCaptureEnabled = _plan.RecordCursor;
            }
        }
        catch (Exception ex)
        {
            Log.Error("设置光标采集失败，使用系统默认。", ex);
        }

        if (_isWindow)
        {
            try
            {
                if (!WinRtCaptureInterop.TryDisableBorder(session))
                {
                    Log.Info("系统不允许关闭录屏彩色边框。");
                }
            }
            catch (Exception ex)
            {
                Log.Error("关闭录屏彩色边框异常", ex);
            }
        }
    }

    /// <summary>
    /// 先开音频设备再建编码器：两路都开不成时干脆不放音轨，
    /// 而不是留下一个没有任何样本的空 AAC 流。
    /// </summary>
    private async Task<bool> OpenAudioAsync()
    {
        var engine = new RecordingAudioEngine(
            _plan.SystemAudio,
            _plan.Microphone,
            ReadTimestamp,
            EnqueueAudioPacket);

        engine.SourceFailed += message => AudioUnavailable?.Invoke(message);
        _audio = engine;

        var openTask = engine.StartAndOpenAsync();
        var finished = await Task.WhenAny(openTask, Task.Delay(AudioOpenTimeoutMs)).ConfigureAwait(true);

        if (finished != openTask || !openTask.Result)
        {
            Log.Info("录屏音频设备不可用，本次只录画面。");
            StopAudio();
            return false;
        }

        return true;
    }

    private void EnqueueAudioPacket(AudioPacket packet)
    {
        if (_audioQueue.IsAddingCompleted || _stopping || _audioQueue.Count > MaxQueuedAudioSamples)
        {
            Interlocked.Increment(ref _droppedAudioSamples);
            return;
        }

        try
        {
            _audioQueue.Add(packet);
        }
        catch (InvalidOperationException)
        {
            // 收尾阶段队列刚被关闭。
            Interlocked.Increment(ref _droppedAudioSamples);
        }
    }

    private void StopAudio()
    {
        var engine = _audio;
        _audio = null;
        engine?.Dispose();
    }

    private void ScheduleReconfigure()
    {
        if (Interlocked.Exchange(ref _reconfigureScheduled, 1) != 0)
        {
            return;
        }

        ThreadPool.QueueUserWorkItem(_ =>
        {
            try
            {
                if (_disposed || _stopping)
                {
                    return;
                }

                lock (_captureLock)
                {
                    if (_disposed || _stopping)
                    {
                        return;
                    }

                    DisposeCapture();
                    StartCapture();
                }

                Volatile.Write(ref _refreshCount, _refreshCount + 1);
                Log.Info($"录制源尺寸变化，已重建捕获：{_surfaceWidth}x{_surfaceHeight}");
            }
            catch (Exception ex)
            {
                Log.Error("重建录屏捕获失败", ex);
                Faulted?.Invoke(ex);
            }
            finally
            {
                Volatile.Write(ref _reconfigureScheduled, 0);
            }
        });
    }

    private void ResolveCrop()
    {
        var crop = _cropResolver();
        var clamped = ScreenGeometry.Intersect(
            crop,
            new PhysicalRect(0, 0, _surfaceWidth, _surfaceHeight));
        var even = ScreenGeometry.MakeEven(clamped);

        if (!ScreenGeometry.IsUsableSelection(even, 16))
        {
            throw new InvalidOperationException("录制区域太小或已移出屏幕，请重新选择。");
        }

        _crop = even;
        _cropScratch = null;
        _cropStride = 0;
    }

    private long ReadRawTimestamp() =>
        (long)(_clock.ElapsedTicks * (10_000_000.0 / Stopwatch.Frequency));

    private long ReadTimestamp()
    {
        var raw = ReadRawTimestamp();
        var paused = Interlocked.Read(ref _pausedTotal);
        var pauseStartedAt = Interlocked.Read(ref _pauseStartedAt);

        // 正在进行的这段暂停也要扣掉，录制时间轴才会在暂停时真正冻住：
        // 浮层计时不会继续走，音频时间戳也不会在恢复时往回跳。
        if (pauseStartedAt >= 0)
        {
            paused += raw - pauseStartedAt;
        }

        return Math.Max(0, raw - paused);
    }

    private void MarkStopped() =>
        Interlocked.Exchange(ref _stoppedTimestamp, Math.Max(0, ReadTimestamp()));

    private void OnItemClosed(GraphicsCaptureItem sender, object args)
    {
        if (_disposed || _stopping)
        {
            return;
        }

        CaptureItemClosed?.Invoke();
    }

    private void DisposeCapture()
    {
        try
        {
            if (_framePool is not null)
            {
                _framePool.FrameArrived -= OnFrameArrived;
            }
        }
        catch
        {
            // 忽略事件解绑异常。
        }

        try
        {
            _captureSession?.Dispose();
        }
        catch
        {
            // 忽略捕获会话关闭异常。
        }

        try
        {
            _framePool?.Dispose();
        }
        catch
        {
            // 忽略帧池关闭异常。
        }

        _captureSession = null;
        _framePool = null;
        _captureStarted = false;
    }

    private void TryDeleteFile()
    {
        try
        {
            if (File.Exists(_path))
            {
                File.Delete(_path);
            }
        }
        catch (Exception ex)
        {
            Log.Error("删除失败的录制文件时出错", ex);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _stopping = true;
        MarkStopped();

        lock (_captureLock)
        {
            DisposeCapture();
        }

        _clock.Stop();
        StopAudio();

        try
        {
            _item.Closed -= OnItemClosed;
        }
        catch
        {
            // 忽略事件解绑异常。
        }

        try
        {
            _videoQueue.CompleteAdding();
            _audioQueue.CompleteAdding();
        }
        catch (ObjectDisposedException)
        {
            // 队列已关闭。
        }

        if (_encoderThreadStarted)
        {
            try
            {
                _encoderCompleted.Task.Wait(TimeSpan.FromSeconds(8));
            }
            catch
            {
                // 忽略等待异常。
            }

            try
            {
                if (_encoderThread is not null && _encoderThread.IsAlive)
                {
                    _encoderThread.Join(2000);
                }
            }
            catch
            {
                // 忽略线程等待异常。
            }
        }

        while (_videoQueue.TryTake(out _))
        {
            // 丢弃仍在队列里的帧索引。
        }

        while (_audioQueue.TryTake(out _))
        {
            // 丢弃仍在队列里的音频采样。
        }

        DisposeQueueIfIdle(_videoQueue);
        DisposeQueueIfIdle(_audioQueue);

        lock (_captureLock)
        {
            _copier?.Dispose();
            _copier = null;
            _deviceHolder?.Dispose();
            _deviceHolder = null;
        }

        _frameBuffers = null;
        _frameTimestamps = null;
        _cropScratch = null;
    }

    private static void DisposeQueueIfIdle<T>(BlockingCollection<T> queue)
    {
        try
        {
            queue.Dispose();
        }
        catch
        {
            // 忽略队列释放异常。
        }
    }
}

/// <summary>
/// 一次录制的编码参数：视频尺寸帧率码率、是否采集光标，以及需要一起写入的音频。
/// </summary>
internal sealed record RecordingPlan(
    int OutputWidth,
    int OutputHeight,
    int FramesPerSecond,
    int Bitrate,
    bool RecordCursor,
    bool SystemAudio = false,
    bool Microphone = false,
    int AudioBitrate = 192_000)
{
    internal bool WantsAudio => SystemAudio || Microphone;

    internal AudioStreamDescriptor? Audio => WantsAudio
        ? new AudioStreamDescriptor(48_000, 2, AudioBitrate)
        : null;
}
