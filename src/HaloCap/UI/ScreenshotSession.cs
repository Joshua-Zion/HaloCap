using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
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
    private PhysicalRect _desktopBounds;
    private PhysicalRect _selectionVirtual;
    private bool _hasSelection;
    private bool _selectionComplete;
    private ScreenshotOverlayWindow? _selectionHost;
    private bool _completing;
    private bool _disposed;
    private DispatcherTimer? _revealFallbackTimer;
    private DispatcherTimer? _revealWarmupTimer;

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

    internal PhysicalRect DesktopBounds => _desktopBounds;

    internal PhysicalRect SelectionVirtual => _selectionVirtual;

    internal bool HasSelection => _hasSelection;

    internal bool IsSelectionComplete => _selectionComplete;

    internal ScreenshotOverlayWindow? SelectionHost => _selectionHost;

    internal ScreenshotOverlayWindow? ActiveOverlay => _activeOverlay;

    internal void RegisterOverlay(ScreenshotOverlayWindow overlay)
    {
        if (_overlays.Contains(overlay))
        {
            return;
        }

        _overlays.Add(overlay);
        RecalculateDesktopBounds();
    }

    internal void UnregisterOverlay(ScreenshotOverlayWindow overlay)
    {
        _overlays.Remove(overlay);
        if (ReferenceEquals(_selectionHost, overlay))
        {
            _selectionHost = null;
        }

        RecalculateDesktopBounds();
    }

    private void RecalculateDesktopBounds()
    {
        if (_overlays.Count == 0)
        {
            return;
        }

        var left = int.MaxValue;
        var top = int.MaxValue;
        var right = int.MinValue;
        var bottom = int.MinValue;

        foreach (var overlay in _overlays)
        {
            var bounds = overlay.Monitor.Bounds;
            left = Math.Min(left, bounds.Left);
            top = Math.Min(top, bounds.Top);
            right = Math.Max(right, bounds.Right);
            bottom = Math.Max(bottom, bounds.Bottom);
        }

        _desktopBounds = PhysicalRect.FromEdges(left, top, right, bottom);
    }

    internal void BeginSelection(ScreenshotOverlayWindow host, PhysicalRect selectionVirtual)
    {
        if (_completing || _disposed)
        {
            return;
        }

        foreach (var overlay in _overlays.ToArray())
        {
            if (!ReferenceEquals(overlay, host))
            {
                overlay.ClearSelectionFromSession();
            }
        }

        _activeOverlay = host;
        _selectionHost = host;
        _selectionVirtual = selectionVirtual;
        _hasSelection = true;
        _selectionComplete = false;
        NotifySelectionChanged();
    }

    internal void UpdateSelection(PhysicalRect selectionVirtual)
    {
        if (_completing || _disposed)
        {
            return;
        }

        _selectionVirtual = selectionVirtual;
        _hasSelection = true;
        EnsureSelectionHost();
        NotifySelectionChanged();
    }

    internal void CompleteSelection()
    {
        if (_completing || _disposed)
        {
            return;
        }

        _selectionComplete = true;
        EnsureSelectionHost();
        NotifySelectionChanged();
    }

    internal void ClearSelection()
    {
        if (_completing || _disposed)
        {
            return;
        }

        _hasSelection = false;
        _selectionComplete = false;
        _selectionVirtual = default;
        _selectionHost = null;
        _activeOverlay = null;
        NotifySelectionChanged();
    }

    private void EnsureSelectionHost()
    {
        if (!_hasSelection)
        {
            _selectionHost = null;
            return;
        }

        if (_selectionHost is not null &&
            !ScreenGeometry.Intersect(_selectionVirtual, _selectionHost.Monitor.Bounds).IsEmpty)
        {
            return;
        }

        ScreenshotOverlayWindow? best = null;
        long bestArea = 0;

        foreach (var overlay in _overlays)
        {
            var local = ScreenGeometry.Intersect(_selectionVirtual, overlay.Monitor.Bounds);
            var area = (long)local.Width * local.Height;
            if (area > bestArea)
            {
                bestArea = area;
                best = overlay;
            }
        }

        _selectionHost = best;
    }

    private void NotifySelectionChanged()
    {
        foreach (var overlay in _overlays.ToArray())
        {
            overlay.ApplySharedSelection();
        }
    }

    internal bool IsCrossMonitorSelection
    {
        get
        {
            if (!_hasSelection)
            {
                return false;
            }

            var count = 0;
            foreach (var overlay in _overlays)
            {
                if (!ScreenGeometry.Intersect(_selectionVirtual, overlay.Monitor.Bounds).IsEmpty)
                {
                    count++;
                }
            }

            return count > 1;
        }
    }

    internal bool CanPinSelection =>
        _mode == CaptureSessionMode.Screenshot &&
        !_completing &&
        !_disposed &&
        _hasSelection &&
        _selectionComplete &&
        _selectionHost is { HasSelection: true };

    internal bool TryPinSelection()
    {
        if (!CanPinSelection)
        {
            return false;
        }

        _selectionHost!.RequestExternalAction(CaptureCommand.Pin);
        return true;
    }

    internal BitmapSource? CreateSelectionBitmap(bool includeAnnotations)
    {
        if (!_hasSelection || _selectionVirtual.Width <= 0 || _selectionVirtual.Height <= 0)
        {
            return null;
        }

        var scale = _selectionHost?.Monitor.Scale ?? 1.0;
        if (scale <= 0.1)
        {
            scale = 1.0;
        }

        var dipWidth = _selectionVirtual.Width / scale;
        var dipHeight = _selectionVirtual.Height / scale;

        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            // 跨屏选区可能包含显示器之间的空隙，空隙按黑色输出。
            context.DrawRectangle(Brushes.Black, null, new Rect(0, 0, dipWidth, dipHeight));

            foreach (var overlay in _overlays)
            {
                var local = ScreenGeometry.Intersect(_selectionVirtual, overlay.Monitor.Bounds);
                if (local.Width <= 0 || local.Height <= 0)
                {
                    continue;
                }

                var image = overlay.CreateLocalSelectionBitmap(
                    includeAnnotations && ReferenceEquals(_selectionHost, overlay));
                if (image is null)
                {
                    continue;
                }

                context.DrawImage(image, new Rect(
                    (local.Left - _selectionVirtual.Left) / scale,
                    (local.Top - _selectionVirtual.Top) / scale,
                    local.Width / scale,
                    local.Height / scale));
            }
        }

        var bitmap = new RenderTargetBitmap(
            _selectionVirtual.Width,
            _selectionVirtual.Height,
            96.0 * scale,
            96.0 * scale,
            PixelFormats.Pbgra32);
        bitmap.Render(visual);
        bitmap.Freeze();
        return bitmap;
    }

    internal async void RequestAction(ScreenshotOverlayWindow overlay, CaptureCommand command)
    {
        if (_completing || _disposed || !_hasSelection)
        {
            return;
        }

        _completing = true;

        var selection = _selectionVirtual;
        var monitor = overlay.Monitor;
        var crossMonitor = IsCrossMonitorSelection;
        BitmapSource? image = null;

        if (command != CaptureCommand.StartRecording)
        {
            try
            {
                image = CreateSelectionBitmap(
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
                    command is CaptureCommand.Copy or CaptureCommand.Save or CaptureCommand.Pin &&
                    !crossMonitor)
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

                try
                {
                    frozen = ScreenCaptureService.Capture(monitor.Bounds);
                }
                catch (Exception ex)
                {
                    Log.Error($"预捕获显示器 {monitor} 失败，将使用实时捕获", ex);
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
            }

            var cursorMonitor = MonitorService.GetMonitorAtCursor();
            var active = _overlays.FirstOrDefault(o => o.Monitor.Handle == cursorMonitor.Handle)
                         ?? _overlays.FirstOrDefault(o => o.Monitor.IsPrimary)
                         ?? _overlays[0];

            _activeOverlay = active;

            var pendingFirstFrames = new HashSet<ScreenshotOverlayWindow>(_overlays);
            var revealStarted = false;

            void StartReveal()
            {
                if (revealStarted || _disposed)
                {
                    return;
                }

                revealStarted = true;
                _revealFallbackTimer?.Stop();
                _revealFallbackTimer = null;

                // 所有显示器先以近乎透明的透明度预合成一帧，等冻结画面和候选框都就绪后
                // 再在同一帧一起完全显示，避免逐屏启动或第一帧空白造成的闪烁。
                foreach (var overlay in _overlays.ToArray())
                {
                    overlay.PrepareReveal();
                }

                var warmup = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(80) };
                warmup.Tick += (_, _) =>
                {
                    warmup.Stop();

                    if (ReferenceEquals(_revealWarmupTimer, warmup))
                    {
                        _revealWarmupTimer = null;
                    }

                    if (_disposed)
                    {
                        return;
                    }

                    foreach (var overlay in _overlays.ToArray())
                    {
                        overlay.CommitReveal();
                    }
                };

                _revealWarmupTimer = warmup;
                warmup.Start();
            }

            foreach (var overlay in _overlays.ToArray())
            {
                var pending = overlay;
                pending.ContentRendered += (_, _) =>
                {
                    if (!pendingFirstFrames.Remove(pending))
                    {
                        return;
                    }

                    if (pendingFirstFrames.Count == 0)
                    {
                        StartReveal();
                    }
                };

                overlay.PrepareAndShow();
            }

            active.ActivateForSelection();

            if (!revealStarted)
            {
                _revealFallbackTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(300) };
                _revealFallbackTimer.Tick += (_, _) =>
                {
                    _revealFallbackTimer?.Stop();
                    _revealFallbackTimer = null;
                    StartReveal();
                };
                _revealFallbackTimer.Start();
            }
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    private void CloseAllOverlays()
    {
        foreach (var overlay in _overlays.ToArray())
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
        _selectionHost = null;
        _hasSelection = false;
        _selectionComplete = false;
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
        _revealFallbackTimer?.Stop();
        _revealFallbackTimer = null;
        _revealWarmupTimer?.Stop();
        _revealWarmupTimer = null;

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
