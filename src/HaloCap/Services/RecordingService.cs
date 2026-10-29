using System;
using System.IO;
using System.Threading.Tasks;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Models;
using HaloCap.UI;

namespace HaloCap.Services;

internal sealed class RecordingService : IDisposable
{
    private readonly Dispatcher _dispatcher;
    private readonly object _stopLock = new();
    private RecordingSession? _session;
    private RecordingHudWindow? _hud;
    private RecordingFrameWindow? _frame;
    private Task? _stopTask;
    private volatile bool _isRecording;
    private bool _disposed;

    internal RecordingService()
    {
        _dispatcher = Dispatcher.CurrentDispatcher;
    }

    internal bool IsRecording => _isRecording;

    internal event Action? StateChanged;

    internal event Action<string>? Saved;

    internal event Action<string>? Failed;

    internal async Task StartAsync(
        ScreenMonitor monitor,
        PhysicalRect selection,
        string configuredDirectory)
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

        var local = ScreenGeometry.ToLocal(selection, monitor.Bounds);
        var clamped = ScreenGeometry.Intersect(
            local,
            new PhysicalRect(0, 0, monitor.Bounds.Width, monitor.Bounds.Height));
        var crop = ScreenGeometry.MakeEven(clamped);

        if (!ScreenGeometry.IsUsableSelection(crop, 16))
        {
            throw new InvalidOperationException("录制区域太小，请选择至少 16 × 16 像素。");
        }

        var path = AppPaths.CreateRecordingPath(configuredDirectory, DateTime.Now);
        var session = new RecordingSession(monitor, crop, path);
        var hud = new RecordingHudWindow(monitor, selection);
        var frame = new RecordingFrameWindow(ScreenGeometry.ToVirtual(crop, monitor.Bounds));
        session.Faulted += OnSessionFaulted;
        hud.StopRequested += OnHudStopRequested;

        try
        {
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
            _isRecording = true;

            try
            {
                RaiseStateChanged();
            }
            catch (Exception ex)
            {
                Log.Error("更新录制状态失败", ex);
            }

            Log.Info($"开始录制：{path}（{crop.Width}x{crop.Height}）");
        }
        catch (Exception ex)
        {
            _session = null;
            _hud = null;
            _frame = null;
            _isRecording = false;

            session.Faulted -= OnSessionFaulted;
            hud.StopRequested -= OnHudStopRequested;
            session.Dispose();

            try
            {
                hud.CloseHud();
            }
            catch
            {
                // 浮层尚未显示时关闭可能失败，忽略。
            }

            try
            {
                frame.CloseFrame();
            }
            catch
            {
                // 边框尚未显示时关闭可能失败，忽略。
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

    internal Task StopAsync()
    {
        return StopAsync(null);
    }

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
            }

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
                        else if (finalPath is not null)
                        {
                            Saved?.Invoke(finalPath);
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

    private void OnHudStopRequested(object? sender, EventArgs e)
    {
        _ = StopAsync();
    }

    private void RaiseStateChanged()
    {
        StateChanged?.Invoke();
    }

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

        _hud = null;
        _frame = null;
        _session?.Dispose();
        _session = null;
        _isRecording = false;
    }
}
