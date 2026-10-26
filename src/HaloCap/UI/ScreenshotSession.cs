using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;
using DrawingBitmap = System.Drawing.Bitmap;

namespace HaloCap.UI;

internal sealed class ScreenshotSession : IDisposable
{
    private readonly CaptureSessionMode _mode;
    private readonly Action<CaptureResult> _completed;
    private readonly IntPtr _previousForeground;
    private readonly List<ScreenshotOverlayWindow> _overlays = new();
    private ScreenshotOverlayWindow? _activeOverlay;
    private bool _completing;
    private bool _disposed;

    private ScreenshotSession(CaptureSessionMode mode, Action<CaptureResult> completed)
    {
        _mode = mode;
        _completed = completed;
        _previousForeground = NativeMethods.GetForegroundWindow();
    }

    internal static ScreenshotSession Start(CaptureSessionMode mode, Action<CaptureResult> completed)
    {
        var monitors = MonitorService.GetMonitors();
        var session = new ScreenshotSession(mode, completed);
        session.CreateOverlays(monitors);
        return session;
    }

    internal void NotifySelectionStarted(ScreenshotOverlayWindow overlay)
    {
        if (_completing || _disposed)
        {
            return;
        }

        if (ReferenceEquals(_activeOverlay, overlay))
        {
            return;
        }

        foreach (var other in _overlays)
        {
            if (!ReferenceEquals(other, overlay))
            {
                other.ClearSelectionFromSession();
            }
        }

        _activeOverlay = overlay;
    }

    internal async void RequestAction(ScreenshotOverlayWindow overlay, CaptureCommand command)
    {
        if (_completing || _disposed)
        {
            return;
        }

        _completing = true;

        var selection = overlay.VirtualSelection;
        var monitor = overlay.Monitor;
        BitmapSource? image = null;

        if (command != CaptureCommand.StartRecording)
        {
            try
            {
                image = overlay.CreateSelectionBitmap(
                    command is CaptureCommand.Copy or CaptureCommand.Save or CaptureCommand.Pin);
            }
            catch (Exception ex)
            {
                Log.Error("裁剪截图失败", ex);
            }
        }

        CloseAllOverlays();

        if (command == CaptureCommand.Cancel)
        {
            MonitorService.RestoreForegroundWindow(_previousForeground);
            Complete(new CaptureResult
            {
                Command = CaptureCommand.Cancel,
                PhysicalRect = selection,
                Monitor = monitor
            });
            return;
        }

        if (command != CaptureCommand.StartRecording && image is null)
        {
            // 预捕获失败的显示器：等遮罩消失后实时抓取。
            await Task.Delay(140);

            try
            {
                image = ScreenCaptureService.CaptureRegion(
                    selection,
                    96.0 * (monitor.Scale > 0.1 ? monitor.Scale : 1.0));

                if (image is not null &&
                    command is CaptureCommand.Copy or CaptureCommand.Save or CaptureCommand.Pin)
                {
                    image = overlay.ComposeAnnotations(image);
                }
            }
            catch (Exception ex)
            {
                Log.Error("实时捕获选区失败", ex);
            }
        }

        MonitorService.RestoreForegroundWindow(_previousForeground);

        Complete(new CaptureResult
        {
            Command = command,
            Image = image,
            PhysicalRect = selection,
            Monitor = monitor
        });
    }

    internal void CancelAll()
    {
        if (_completing || _disposed)
        {
            return;
        }

        _completing = true;
        CloseAllOverlays();
        MonitorService.RestoreForegroundWindow(_previousForeground);
        Complete(new CaptureResult { Command = CaptureCommand.Cancel });
    }

    private void CreateOverlays(IReadOnlyList<ScreenMonitor> monitors)
    {
        try
        {
            foreach (var monitor in monitors)
            {
                DrawingBitmap? frozen = null;

                if (_mode != CaptureSessionMode.Record)
                {
                    try
                    {
                        frozen = ScreenCaptureService.Capture(monitor.Bounds);
                    }
                    catch (Exception ex)
                    {
                        Log.Error($"预捕获显示器 {monitor} 失败，将使用实时捕获", ex);
                    }
                }

                ScreenshotOverlayWindow overlay;

                try
                {
                    overlay = new ScreenshotOverlayWindow(this, monitor, _mode, frozen);
                }
                catch
                {
                    frozen?.Dispose();
                    throw;
                }

                _overlays.Add(overlay);
            }

            foreach (var overlay in _overlays)
            {
                overlay.PrepareAndShow();
            }

            var cursorMonitor = MonitorService.GetMonitorAtCursor();
            var active = _overlays.FirstOrDefault(o => o.Monitor.Handle == cursorMonitor.Handle)
                         ?? _overlays.FirstOrDefault(o => o.Monitor.IsPrimary)
                         ?? _overlays[0];

            _activeOverlay = active;
            active.ActivateForSelection();
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    private void CloseAllOverlays()
    {
        foreach (var overlay in _overlays)
        {
            try
            {
                overlay.CloseOverlay();
            }
            catch (Exception ex)
            {
                Log.Error("关闭截图遮罩失败", ex);
            }
        }

        _overlays.Clear();
        _activeOverlay = null;
    }

    private void Complete(CaptureResult result)
    {
        _disposed = true;

        try
        {
            _completed(result);
        }
        catch (Exception ex)
        {
            Log.Error("处理截图结果失败", ex);
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            _completing = true;
            CloseAllOverlays();
            return;
        }

        _disposed = true;
        CloseAllOverlays();
    }
}
