using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.UI;

/// <summary>
/// 录屏开始前的倒计时浮层：不抢焦点、不进入捕获，倒数结束才真正开始采集。
/// </summary>
public partial class RecordingCountdownWindow : Window
{
    private readonly PhysicalRect _anchor;
    private readonly int _seconds;
    private readonly TaskCompletionSource _finished =
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private IntPtr _handle;
    private DispatcherTimer? _timer;
    private int _remaining;
    private bool _closed;

    internal RecordingCountdownWindow(PhysicalRect anchor, int seconds)
    {
        InitializeComponent();

        _anchor = anchor;
        _seconds = Math.Max(1, seconds);
        _remaining = _seconds;
        CountText.Text = _remaining.ToString();

        SourceInitialized += (_, _) =>
        {
            _handle = new WindowInteropHelper(this).EnsureHandle();
            ApplyNonActivatingStyles(_handle);
            PlaceWindow(_handle);
        };

        Closed += (_, _) =>
        {
            _closed = true;
            _timer?.Stop();
            _timer = null;
            _finished.TrySetResult();
        };
    }

    internal Task StartAsync()
    {
        CountText.Text = _remaining.ToString();
        Show();
        PlayTick();

        _timer = new DispatcherTimer { Interval = TimeSpan.FromSeconds(1) };
        _timer.Tick += (_, _) =>
        {
            _remaining--;

            if (_remaining <= 0)
            {
                Close();
                return;
            }

            CountText.Text = _remaining.ToString();
            PlayTick();
        };
        _timer.Start();

        return _finished.Task;
    }

    internal void Cancel()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        _timer?.Stop();
        _timer = null;

        try
        {
            Close();
        }
        catch (Exception ex)
        {
            Log.Error("关闭录屏倒计时失败", ex);
        }

        _finished.TrySetResult();
    }

    private void PlayTick()
    {
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        var scale = new ScaleTransform(1.28, 1.28);
        Card.RenderTransformOrigin = new Point(0.5, 0.5);
        Card.RenderTransform = scale;

        Card.BeginAnimation(
            UIElement.OpacityProperty,
            new DoubleAnimation(0.55, 1.0, TimeSpan.FromMilliseconds(160)) { EasingFunction = ease });

        scale.BeginAnimation(
            ScaleTransform.ScaleXProperty,
            new DoubleAnimation(1.28, 1.0, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
        scale.BeginAnimation(
            ScaleTransform.ScaleYProperty,
            new DoubleAnimation(1.28, 1.0, TimeSpan.FromMilliseconds(220)) { EasingFunction = ease });
    }

    private static void ApplyNonActivatingStyles(IntPtr handle)
    {
        if (handle == IntPtr.Zero)
        {
            return;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE).ToInt64();
        exStyle |= NativeMethods.WS_EX_NOACTIVATE | NativeMethods.WS_EX_TOOLWINDOW;
        NativeMethods.SetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE, new IntPtr(exStyle));

        try
        {
            NativeMethods.SetWindowDisplayAffinity(handle, NativeMethods.WDA_EXCLUDEFROMCAPTURE);
        }
        catch (Exception ex)
        {
            Log.Error("设置倒计时捕获排除失败", ex);
        }

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE |
            NativeMethods.SWP_NOSIZE |
            NativeMethods.SWP_NOACTIVATE |
            NativeMethods.SWP_FRAMECHANGED);
    }

    private void PlaceWindow(IntPtr handle)
    {
        if (handle == IntPtr.Zero || _anchor.IsEmpty)
        {
            return;
        }

        var dpi = GetDpiScale();
        var physicalWidth = (int)Math.Round(Width * dpi);
        var physicalHeight = (int)Math.Round(Height * dpi);

        var left = _anchor.Left + ((_anchor.Width - physicalWidth) / 2);
        var top = _anchor.Top + ((_anchor.Height - physicalHeight) / 2);

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            left,
            top,
            0,
            0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private double GetDpiScale()
    {
        try
        {
            var dpi = VisualTreeHelper.GetDpi(this);
            return dpi.DpiScaleX > 0.1 ? dpi.DpiScaleX : 1.0;
        }
        catch (Exception ex)
        {
            Log.Error("读取倒计时窗口 DPI 失败", ex);
            return 1.0;
        }
    }
}
