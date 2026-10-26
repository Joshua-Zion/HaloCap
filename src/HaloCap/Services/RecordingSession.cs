using System;
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using Windows.Foundation.Metadata;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX;
using Windows.Graphics.DirectX.Direct3D11;
using Windows.Graphics.Imaging;
using Windows.Storage.Streams;
using WinRtBuffer = Windows.Storage.Streams.Buffer;

namespace HaloCap.Services;

internal sealed class RecordingSession : IDisposable
{
    private const int FramesPerSecond = 30;
    private const int FrameDuration = 333_333;
    private const int MaxQueueDepth = 3;

    private readonly ScreenMonitor _monitor;
    private readonly PhysicalRect _crop;
    private readonly string _path;
    private readonly int _captureWidth;
    private readonly int _captureHeight;
    private readonly int _frameWidth;
    private readonly int _frameHeight;
    private readonly int _frameBytes;
    private readonly int _bitrate;
    private readonly byte[] _fullFrame;
    private readonly BlockingCollection<FramePacket> _queue = new(MaxQueueDepth);
    private readonly TaskCompletionSource<bool> _encoderReady =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly TaskCompletionSource<Exception?> _encoderCompleted =
        new(TaskCreationOptions.RunContinuationsAsynchronously);
    private readonly Stopwatch _clock = new();
    private readonly object _stopLock = new();

    private GraphicsCaptureItem? _item;
    private IDirect3DDevice? _device;
    private Direct3D11CaptureFramePool? _framePool;
    private GraphicsCaptureSession? _captureSession;
    private WinRtBuffer? _frameBuffer;
    private Thread? _encoderThread;
    private Task<string>? _stopTask;
    private int _frameBusy;
    private int _encodedFrames;
    private long _lastFrameTimestamp = -1;
    private long _firstTimestamp = -1;
    private long _stoppedTimestamp = -1;
    private bool _captureStarted;
    private bool _encoderThreadStarted;
    private bool _disposed;
    private volatile bool _stopping;

    internal RecordingSession(ScreenMonitor monitor, PhysicalRect crop, string path)
    {
        _monitor = monitor;
        _crop = crop;
        _path = path;
        _captureWidth = monitor.Bounds.Width;
        _captureHeight = monitor.Bounds.Height;
        _frameWidth = crop.Width;
        _frameHeight = crop.Height;
        _frameBytes = checked(_frameWidth * _frameHeight * 4);
        _fullFrame = new byte[checked(_captureWidth * _captureHeight * 4)];
        _bitrate = (int)Math.Clamp((long)_frameWidth * _frameHeight * FramesPerSecond / 8, 2_000_000, 20_000_000);

        if (crop.Left < 0 ||
            crop.Top < 0 ||
            crop.Right > _captureWidth ||
            crop.Bottom > _captureHeight)
        {
            throw new InvalidOperationException("录制区域超出显示器范围。");
        }

        if ((crop.Width & 1) != 0 || (crop.Height & 1) != 0)
        {
            throw new InvalidOperationException("录制区域尺寸必须为偶数。");
        }

        if (crop.Width < 16 || crop.Height < 16)
        {
            throw new InvalidOperationException("录制区域太小，请选择至少 16 × 16 像素。");
        }
    }

    internal event Action<Exception>? Faulted;

    internal string OutputPath => _path;

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

            _item = WinRtCaptureInterop.CreateItemForMonitor(_monitor.Handle);
            var size = _item.Size;

            if (size.Width != _captureWidth || size.Height != _captureHeight)
            {
                throw new InvalidOperationException("显示器分辨率已变化，请重新选择录制区域。");
            }

            _device = WinRtCaptureInterop.CreateDirect3DDevice();
            _framePool = Direct3D11CaptureFramePool.CreateFreeThreaded(
                _device,
                DirectXPixelFormat.B8G8R8A8UIntNormalized,
                2,
                size);

            _framePool.FrameArrived += OnFrameArrived;
            _captureSession = _framePool.CreateCaptureSession(_item);

            try
            {
                // Windows 10 2004 才加入此属性；1903/1909 上保持系统默认（包含光标）。
                if (ApiInformation.IsPropertyPresent(
                        "Windows.Graphics.Capture.GraphicsCaptureSession",
                        "IsCursorCaptureEnabled"))
                {
                    _captureSession.IsCursorCaptureEnabled = true;
                }
            }
            catch
            {
                // 属性查询失败时使用系统默认值。
            }

            _frameBuffer = new WinRtBuffer((uint)_fullFrame.Length)
            {
                Length = (uint)_fullFrame.Length
            };

            StartEncoderThread();
            await _encoderReady.Task.ConfigureAwait(true);

            if (_disposed || _stopping)
            {
                throw new OperationCanceledException("录制已取消。");
            }

            _clock.Start();
            _captureSession.StartCapture();
            _captureStarted = true;
        }
        catch
        {
            Dispose();
            throw;
        }
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
        _clock.Stop();
        Interlocked.Exchange(
            ref _stoppedTimestamp,
            (long)(_clock.ElapsedTicks * (10_000_000.0 / Stopwatch.Frequency)));
        DisposeCapture();

        try
        {
            _queue.CompleteAdding();
        }
        catch (ObjectDisposedException)
        {
            // 编码线程已经结束。
        }

        var timeout = Task.Delay(TimeSpan.FromSeconds(20));
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

        try
        {
            var hr = NativeMethods.CoInitializeEx(IntPtr.Zero, 0);
            comInitialized = hr >= 0;

            writer = new MediaFoundationWriter(
                _path,
                _frameWidth,
                _frameHeight,
                FramesPerSecond,
                _bitrate);

            _encoderReady.TrySetResult(true);

            FramePacket? pending = null;

            try
            {
                foreach (var packet in _queue.GetConsumingEnumerable())
                {
                    if (pending is not null)
                    {
                        var duration = Math.Clamp(
                            packet.Timestamp - pending.Timestamp,
                            10_000L,
                            1_000_000L);
                        WritePacket(writer, pending, duration);
                        pending = null;
                    }

                    if (_firstTimestamp < 0)
                    {
                        _firstTimestamp = packet.Timestamp;
                    }

                    pending = packet;
                }

                if (pending is not null)
                {
                    // 静态画面下 WGC 可能长时间不再送帧；用最后一帧覆盖到停止时刻，
                    // 保证 MP4 时长与录制时长一致。
                    var stopped = Interlocked.Read(ref _stoppedTimestamp);
                    var finalDuration = stopped > pending.Timestamp
                        ? Math.Max(FrameDuration, stopped - pending.Timestamp)
                        : FrameDuration;

                    WritePacket(writer, pending, finalDuration);
                    pending = null;
                }
            }
            finally
            {
                if (pending is not null)
                {
                    ArrayPool<byte>.Shared.Return(pending.Buffer);
                }
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

    private void WritePacket(MediaFoundationWriter writer, FramePacket packet, long duration)
    {
        try
        {
            var timestamp = Math.Max(0, packet.Timestamp - _firstTimestamp);
            writer.WriteFrame(packet.Buffer, timestamp, duration);
            _encodedFrames++;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(packet.Buffer);
        }
    }

    private async void OnFrameArrived(Direct3D11CaptureFramePool sender, object args)
    {
        if (_stopping || _disposed)
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

            var timestamp = (long)(_clock.ElapsedTicks * (10_000_000.0 / Stopwatch.Frequency));

            if (timestamp == _lastFrameTimestamp)
            {
                return;
            }

            _lastFrameTimestamp = timestamp;

            using var bitmap = await SoftwareBitmap.CreateCopyFromSurfaceAsync(frame.Surface);

            if (_stopping || _disposed)
            {
                return;
            }

            var packet = ExtractFrame(bitmap, timestamp);
            if (packet is null)
            {
                return;
            }

            if (!_queue.TryAdd(packet))
            {
                ArrayPool<byte>.Shared.Return(packet.Buffer);
            }
        }
        catch (Exception ex)
        {
            if (!_stopping)
            {
                Log.Error("捕获录制画面失败", ex);
            }
        }
        finally
        {
            Volatile.Write(ref _frameBusy, 0);
        }
    }

    private FramePacket? ExtractFrame(SoftwareBitmap bitmap, long timestamp)
    {
        if (_frameBuffer is null)
        {
            return null;
        }

        if (bitmap.PixelWidth != _captureWidth || bitmap.PixelHeight != _captureHeight)
        {
            Log.Error($"录制帧尺寸异常：{bitmap.PixelWidth}x{bitmap.PixelHeight}，预期 {_captureWidth}x{_captureHeight}");
            return null;
        }

        _frameBuffer.Length = (uint)_fullFrame.Length;
        bitmap.CopyToBuffer(_frameBuffer);

        using (var reader = DataReader.FromBuffer(_frameBuffer))
        {
            reader.ReadBytes(_fullFrame);
        }

        var cropped = ArrayPool<byte>.Shared.Rent(_frameBytes);
        var sourceStride = _captureWidth * 4;
        var targetStride = _frameWidth * 4;

        for (var row = 0; row < _frameHeight; row++)
        {
            var sourceOffset = ((_crop.Top + row) * _captureWidth + _crop.Left) * 4;
            System.Buffer.BlockCopy(_fullFrame, sourceOffset, cropped, row * targetStride, targetStride);
        }

        return new FramePacket(cropped, timestamp);
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
        _clock.Stop();
        Interlocked.Exchange(
            ref _stoppedTimestamp,
            (long)(_clock.ElapsedTicks * (10_000_000.0 / Stopwatch.Frequency)));
        DisposeCapture();

        try
        {
            _queue.CompleteAdding();
        }
        catch (ObjectDisposedException)
        {
            // 队列已关闭。
        }

        if (_encoderThreadStarted)
        {
            try
            {
                _encoderCompleted.Task.Wait(TimeSpan.FromSeconds(5));
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

        while (_queue.TryTake(out var packet))
        {
            ArrayPool<byte>.Shared.Return(packet.Buffer);
        }

        if (_encoderThread is null || !_encoderThread.IsAlive)
        {
            try
            {
                _queue.Dispose();
            }
            catch
            {
                // 忽略队列释放异常。
            }
        }

        try
        {
            _device?.Dispose();
        }
        catch
        {
            // 忽略设备释放异常。
        }

        _device = null;
        _item = null;
    }

    private sealed class FramePacket
    {
        public FramePacket(byte[] buffer, long timestamp)
        {
            Buffer = buffer;
            Timestamp = timestamp;
        }

        public byte[] Buffer { get; }

        public long Timestamp { get; }
    }
}
