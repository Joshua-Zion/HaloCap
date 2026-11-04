using System;
using System.Diagnostics;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.UI;
using Windows.Graphics.Capture;

namespace HaloCap.Services;

/// <summary>
/// 一次录屏的请求：区域模式给选区，窗口模式给目标窗口句柄。
/// </summary>
internal sealed record RecordingRequest(
    ScreenMonitor Monitor,
    PhysicalRect Anchor,
    IntPtr WindowHandle,
    RecordingSettings Settings,
    string ConfiguredDirectory);

/// <summary>
/// 录制完成后的结果，用于通知与日志。
/// </summary>
internal sealed record RecordingOutput(
    string Path,
    TimeSpan Duration,
    int Width,
    int Height,
    int FramesPerSecond,
    int EncodedFrames,
    long DroppedFrames,
    bool HardwareEncoder,
    long Bytes);

internal sealed class RecordingService : IDisposable
{
    private const int MinimumFreeSpaceBytes = 200_000_000;
    private static readonly TimeSpan HudRefreshInterval = TimeSpan.FromMilliseconds(250);

    private readonly Dispatcher _dispatcher;
    private readonly object _stopLock = new();
    private RecordingSession? _session;
    private RecordingHudWindow? _hud;
    private RecordingFrameWindow? _frame;
    private RecordingCountdownWindow? _countdown;
    private DispatcherTimer? _hudTimer;
    private Task? _stopTask;
    private IntPtr _windowHandle;
    private volatile bool _isRecording;
    private bool _disposed;

    internal RecordingService()
    {
        _dispatcher = Dispatcher.CurrentDispatcher;
    }

    internal bool IsRecording => _isRecording;

    internal bool IsPaused => _session?.IsPaused == true;

    internal event Action? StateChanged;

    internal event Action<RecordingOutput>? Saved;

    internal event Action<string>? Failed;

    internal async Task StartAsync(RecordingRequest request)
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(RecordingService));
        }

        Task? pendingStop;
        lock (_stopLock)
        {
            pendingStop = _stopTask;
        }

        if (pendingStop is not null)
        {
            try
            {
                await pendingStop.ConfigureAwait(true);
            }
            catch
            {
                // 上一次停止失败已通过 Failed 事件报告。
            }
        }

        if (_isRecording)
        {
            throw new InvalidOperationException("已经在录制中。请先停止当前录制。");
        }

        var settings = request.Settings.Clone();
        settings.Normalize();
        var isWindow = request.WindowHandle != IntPtr.Zero;

        var target = default(WindowCaptureTarget);

        if (isWindow && !WindowGeometry.TryGet(request.WindowHandle, out target))
        {
            throw new InvalidOperationException("目标窗口已关闭或尺寸太小，请重新选择窗口。");
        }

        var crop = isWindow
            ? ScreenGeometry.MakeEven(target.CropInSurface)
            : ScreenGeometry.MakeEven(ScreenGeometry.Intersect(
                ScreenGeometry.ToLocal(request.Anchor, request.Monitor.Bounds),
                new PhysicalRect(0, 0, request.Monitor.Bounds.Width, request.Monitor.Bounds.Height)));

        if (!ScreenGeometry.IsUsableSelection(crop, 16))
        {
            throw new InvalidOperationException("录制区域太小，请选择至少 16 × 16 像素。");
        }

        var (outputWidth, outputHeight) = settings.ResolveOutputSize(crop.Width, crop.Height);
        var bitrate = settings.ComputeBitrate(outputWidth, outputHeight);
        var path = AppPaths.CreateRecordingPath(request.ConfiguredDirectory, DateTime.Now);
        EnsureFreeSpace(path, bitrate);

        if (settings.CountdownSeconds > 0)
        {
            await RunCountdownAsync(request.Anchor, settings.CountdownSeconds).ConfigureAwait(true);
        }

        if (_disposed)
        {
            throw new OperationCanceledException("应用正在退出。");
        }

        if (isWindow && !WindowGeometry.TryGet(request.WindowHandle, out target))
        {
            throw new InvalidOperationException("倒计时期间目标窗口已关闭。");
        }

        var monitor = isWindow ? WindowGeometry.GetMonitor(target) : request.Monitor;
        var plan = new RecordingPlan(
            outputWidth,
            outputHeight,
            settings.FramesPerSecond,
            bitrate,
            settings.RecordCursor);

        GraphicsCaptureItem? item = null;
        RecordingSession? session = null;
        RecordingHudWindow? hud = null;
        RecordingFrameWindow? frame = null;

        try
        {
            item = isWindow
                ? WinRtCaptureInterop.CreateItemForWindow(request.WindowHandle)
                : WinRtCaptureInterop.CreateItemForMonitor(monitor.Handle);

            session = new RecordingSession(
                item,
                path,
                plan,
                () => isWindow
                    ? WindowGeometry.TryGet(request.WindowHandle, out var live)
                        ? live.CropInSurface
                        : target.CropInSurface
                    : ScreenGeometry.ToLocal(request.Anchor, monitor.Bounds),
                isWindow);

            hud = new RecordingHudWindow(monitor, request.Anchor);
            frame = new RecordingFrameWindow(
                isWindow ? target.VisibleRect : ScreenGeometry.ToVirtual(crop, monitor.Bounds));

            session.Faulted += OnSessionFaulted;
            session.CaptureItemClosed += OnCaptureItemClosed;
            hud.StopRequested += OnHudStopRequested;
            hud.PauseRequested += OnHudPauseRequested;
            hud.ResumeRequested += OnHudResumeRequested;

            hud.PrepareForCapture();
            frame.PrepareForCapture();
            await session.StartAsync();

            if (_disposed)
            {
                throw new OperationCanceledException("应用正在退出。");
            }

            hud.ShowHud();
            frame.ShowFrame();

            _session = session;
            _hud = hud;
            _frame = frame;
            _windowHandle = isWindow ? request.WindowHandle : IntPtr.Zero;
            _isRecording = true;

            StartHudTimer();
            UpdateHud();
            RaiseStateChanged();

            Log.Info(
                $"开始录制：{path}（{outputWidth}x{outputHeight} " +
                $"{settings.FramesPerSecond}fps " +
                $"{(isWindow ? "窗口" : "区域")}" +
                $"{(session.UsesHardwareEncoder ? " · 硬件编码" : string.Empty)}）");
        }
        catch (Exception ex)
        {
            _session = null;
            _hud = null;
            _frame = null;
            _windowHandle = IntPtr.Zero;
            _isRecording = false;

            StopHudTimer();

            if (session is not null)
            {
                session.Faulted -= OnSessionFaulted;
                session.CaptureItemClosed -= OnCaptureItemClosed;
                session.Dispose();
            }

            if (hud is not null)
            {
                hud.StopRequested -= OnHudStopRequested;
                hud.PauseRequested -= OnHudPauseRequested;
                hud.ResumeRequested -= OnHudResumeRequested;

                try
                {
                    hud.CloseHud();
                }
                catch
                {
                    // 浮层尚未显示时关闭可能失败，忽略。
                }
            }

            if (frame is not null)
            {
                try
                {
                    frame.CloseFrame();
                }
                catch
                {
                    // 边框尚未显示时关闭可能失败，忽略。
                }
            }

            TryDelete(path);

            if (ex is OperationCanceledException)
            {
                throw;
            }

            Log.Error("启动录制失败", ex);
            throw new InvalidOperationException("启动录制失败：" + ex.Message, ex);
        }
    }

    internal Task StopAsync() => StopAsync(null);

    internal Task StopAsync(Exception? cause)
    {
        lock (_stopLock)
        {
            if (_session is null && !_isRecording)
            {
                return Task.CompletedTask;
            }

            if (_stopTask is not null)
            {
                return _stopTask;
            }

            var task = StopCoreAsync(cause);
            _stopTask = task;

            // 任务可能已经同步结束，此时 finally 会先于赋值把字段清空。
            if (task.IsCompleted)
            {
                _stopTask = null;
            }

            return task;
        }
    }

    internal void TogglePause()
    {
        var session = _session;

        if (session is null || !_isRecording)
        {
            return;
        }

        if (session.IsPaused)
        {
            session.Resume();
        }
        else
        {
            session.Pause();
        }

        UpdateHud();
        RaiseStateChanged();
    }

    internal void StopForExit(TimeSpan timeout)
    {
        try
        {
            var task = StopAsync();
            task.Wait(timeout);
        }
        catch (Exception ex)
        {
            Log.Error("退出时停止录制失败", ex);
        }
    }

    private async Task StopCoreAsync(Exception? cause)
    {
        try
        {
            var session = _session;
            var hud = _hud;
            var frame = _frame;
            _hud = null;
            _frame = null;
            _windowHandle = IntPtr.Zero;

            StopHudTimer();

            if (session is null)
            {
                _isRecording = false;
                return;
            }

            try
            {
                hud?.CloseHud();
            }
            catch
            {
                // 浮层可能已经关闭。
            }

            try
            {
                frame?.CloseFrame();
            }
            catch
            {
                // 边框可能已经关闭。
            }

            if (hud is not null)
            {
                hud.StopRequested -= OnHudStopRequested;
                hud.PauseRequested -= OnHudPauseRequested;
                hud.ResumeRequested -= OnHudResumeRequested;
            }

            var elapsed = TimeSpan.FromTicks(session.ActiveElapsed100Ns / 100);
            string? savedPath = null;
            Exception? error = cause;

            try
            {
                savedPath = await session.StopAsync().ConfigureAwait(false);
            }
            catch (Exception ex)
            {
                if (error is null)
                {
                    error = ex;
                }
            }

            session.Faulted -= OnSessionFaulted;
            session.CaptureItemClosed -= OnCaptureItemClosed;

            try
            {
                session.Dispose();
            }
            catch (Exception disposeError)
            {
                Log.Error("释放录制会话失败", disposeError);

                if (error is null)
                {
                    error = disposeError;
                }
            }

            if (error is not null && savedPath is not null)
            {
                TryDelete(savedPath);
            }

            _session = null;
            _isRecording = false;

            var finalCause = error;
            var finalPath = error is null ? savedPath : null;
            var finalOutput = BuildOutput(finalPath, session, elapsed);

            _ = _dispatcher.BeginInvoke(
                DispatcherPriority.Normal,
                new Action(() =>
                {
                    if (_disposed)
                    {
                        return;
                    }

                    try
                    {
                        StateChanged?.Invoke();

                        if (finalCause is not null)
                        {
                            Failed?.Invoke(finalCause.Message);
                        }
                        else if (finalOutput is not null)
                        {
                            Saved?.Invoke(finalOutput);
                        }
                    }
                    catch (Exception eventError)
                    {
                        Log.Error("分发录制完成事件失败", eventError);
                    }
                }));
        }
        finally
        {
            lock (_stopLock)
            {
                _stopTask = null;
            }
        }
    }

    private static RecordingOutput? BuildOutput(
        string? path,
        RecordingSession session,
        TimeSpan elapsed)
    {
        if (path is null)
        {
            return null;
        }

        long bytes = 0;

        try
        {
            bytes = new FileInfo(path).Length;
        }
        catch (Exception ex)
        {
            Log.Error("读取录制文件大小失败", ex);
        }

        return new RecordingOutput(
            path,
            elapsed,
            session.OutputWidth,
            session.OutputHeight,
            session.FramesPerSecond,
            session.EncodedFrames,
            session.DroppedFrames,
            session.UsesHardwareEncoder,
            bytes);
    }

    private async Task RunCountdownAsync(PhysicalRect anchor, int seconds)
    {
        RecordingCountdownWindow? window = null;

        try
        {
            window = new RecordingCountdownWindow(anchor, seconds);
            _countdown = window;
            await window.StartAsync();
        }
        catch (Exception ex)
        {
            Log.Error("录屏倒计时异常", ex);
        }
        finally
        {
            if (ReferenceEquals(_countdown, window))
            {
                _countdown = null;
            }

            if (window is not null)
            {
                window.Cancel();
            }
        }
    }

    private void StartHudTimer()
    {
        StopHudTimer();

        _hudTimer = new DispatcherTimer { Interval = HudRefreshInterval };
        _hudTimer.Tick += (_, _) =>
        {
            TrackWindowTarget();
            UpdateHud();
        };
        _hudTimer.Start();
    }

    private void StopHudTimer()
    {
        _hudTimer?.Stop();
        _hudTimer = null;
    }

    /// <summary>窗口录制时跟随窗口：移动更新边框与浮层，尺寸变化重建捕获，关闭或最小化则收尾保存。</summary>
    private void TrackWindowTarget()
    {
        if (_windowHandle == IntPtr.Zero)
        {
            _session?.RefreshIfSourceResized();
            return;
        }

        if (!WindowGeometry.TryGet(_windowHandle, out var target))
        {
            Log.Info("录制目标窗口已关闭或最小化，停止并保存。");
            _ = StopAsync();
            return;
        }

        _frame?.UpdateCrop(target.VisibleRect);
        _hud?.UpdateAnchor(target.VisibleRect);
        _session?.RefreshIfSourceResized();
    }

    private void UpdateHud()
    {
        var session = _session;

        if (session is null || _hud is null)
        {
            return;
        }

        _hud.UpdateState(
            TimeSpan.FromTicks(session.ActiveElapsed100Ns / 100),
            session.IsPaused,
            BuildHudInfo(session));
    }

    private static string BuildHudInfo(RecordingSession session)
    {
        var size = string.Empty;

        try
        {
            var bytes = new FileInfo(session.OutputPath).Length;
            size = $" · {FormatSize(bytes)}";
        }
        catch
        {
            // 文件尚未创建时不显示大小。
        }

        return $"{session.OutputWidth}×{session.OutputHeight} · {session.FramesPerSecond}fps{size}";
    }

    private static void EnsureFreeSpace(string path, int bitrate)
    {
        try
        {
            var fullPath = Path.GetFullPath(path);
            var root = Path.GetPathRoot(fullPath);

            if (string.IsNullOrEmpty(root))
            {
                return;
            }

            var drive = new DriveInfo(root);

            if (!drive.IsReady)
            {
                throw new InvalidOperationException($"保存目录所在磁盘 {root} 当前不可用。");
            }

            var required = Math.Max(MinimumFreeSpaceBytes, (long)bitrate / 8 * 60);
            var available = drive.AvailableFreeSpace;

            if (available < required)
            {
                throw new InvalidOperationException(
                    $"磁盘剩余空间不足：{FormatSize(available)}，至少需要 {FormatSize(required)}（约 1 分钟）。请在设置中更换保存目录。");
            }
        }
        catch (InvalidOperationException)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log.Error("检查录制磁盘空间失败", ex);
        }
    }

    private static string FormatSize(long bytes) =>
        bytes >= 1024L * 1024 * 1024
            ? $"{bytes / 1024.0 / 1024.0 / 1024.0:0.##} GB"
            : $"{bytes / 1024.0 / 1024.0:0.##} MB";

    private void OnSessionFaulted(Exception exception)
    {
        if (_disposed)
        {
            return;
        }

        _dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() =>
            {
                if (!_disposed && _isRecording)
                {
                    _ = StopAsync(exception);
                }
            }));
    }

    private void OnCaptureItemClosed()
    {
        if (_disposed)
        {
            return;
        }

        _dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() =>
            {
                if (!_disposed && _isRecording)
                {
                    _ = StopAsync();
                }
            }));
    }

    private void OnHudStopRequested(object? sender, EventArgs e) => _ = StopAsync();

    private void OnHudPauseRequested(object? sender, EventArgs e) => TogglePause();

    private void OnHudResumeRequested(object? sender, EventArgs e) => TogglePause();

    private void RaiseStateChanged() => StateChanged?.Invoke();

    private static void TryDelete(string path)
    {
        try
        {
            if (!string.IsNullOrEmpty(path) && File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch (Exception ex)
        {
            Log.Error("清理录制文件失败", ex);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        StopForExit(TimeSpan.FromSeconds(8));
        _disposed = true;

        try
        {
            _countdown?.Cancel();
        }
        catch
        {
            // 忽略倒计时关闭异常。
        }

        _countdown = null;

        try
        {
            _hud?.CloseHud();
        }
        catch
        {
            // 忽略浮层关闭异常。
        }

        try
        {
            _frame?.CloseFrame();
        }
        catch
        {
            // 忽略边框关闭异常。
        }

        StopHudTimer();
        _hud = null;
        _frame = null;
        _session?.Dispose();
        _session = null;
        _isRecording = false;
    }
}
