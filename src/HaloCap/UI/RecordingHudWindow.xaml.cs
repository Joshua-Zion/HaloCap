using System;
using System.Diagnostics;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.UI;

public partial class RecordingHudWindow : Window
{
    private readonly ScreenMonitor _monitor;
    private readonly PhysicalRect _selection;
    private readonly DispatcherTimer _timer;
    private readonly Stopwatch _clock = new();
    private IntPtr _handle;
    private bool _prepared;
    private bool _closed;

    internal RecordingHudWindow(ScreenMonitor monitor, PhysicalRect selection)
    {
        InitializeComponent();

        _monitor = monitor;
        _selection = selection;

        _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(200) };
        _timer.Tick += (_, _) => UpdateElapsed();

        Closed += (_, _) =>
        {
            _closed = true;
            _timer.Stop();
            _clock.Stop();
        };
    }

    internal event EventHandler? StopRequested;

    internal void PrepareForCapture()
    {
        if (_prepared || _closed)
        {
            return;
        }

        _handle = new WindowInteropHelper(this).EnsureHandle();
        ApplyNonActivatingStyles(_handle);

        try
        {
            NativeMethods.SetWindowDisplayAffinity(_handle, NativeMethods.WDA_EXCLUDEFROMCAPTURE);
        }
        catch (Exception ex)
        {
            Log.Error("设置录制浮层捕获排除失败", ex);
        }

        _prepared = true;
    }

    internal void ShowHud()
    {
        if (_closed)
        {
            return;
        }

        Opacity = 0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = 0;
        Top = 0;

        PrepareForCapture();
        Show();

        try
        {
            NativeMethods.SetWindowDisplayAffinity(_handle, NativeMethods.WDA_EXCLUDEFROMCAPTURE);
        }
        catch (Exception ex)
        {
            Log.Error("设置录制浮层捕获排除失败", ex);
        }

        ApplyNonActivatingStyles(_handle);
        PlaceWindow(_handle);

        _clock.Start();
        UpdateElapsed();
        _timer.Start();

        var pulse = new DoubleAnimation(1.0, 0.35, TimeSpan.FromMilliseconds(720))
        {
            AutoReverse = true,
            RepeatBehavior = RepeatBehavior.Forever,
            EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
        };
        PulseDot.BeginAnimation(OpacityProperty, pulse);

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });
    }

    internal void CloseHud()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        _timer.Stop();
        _clock.Stop();
        Close();
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
        if (!NativeMethods.GetWindowRect(handle, out var windowRect))
        {
            return;
        }

        var area = _monitor.WorkArea;
        var width = windowRect.Width;
        var height = windowRect.Height;
        const int margin = 10;

        var left = _selection.Left + ((_selection.Width - width) / 2);
        var top = _selection.Top - height - margin;

        if (top < area.Top + 4)
        {
            top = _selection.Bottom + margin;
        }

        if (top + height > area.Bottom - 4)
        {
            top = _selection.Top + margin;
        }

        left = Math.Clamp(left, area.Left + 4, Math.Max(area.Left + 4, area.Right - width - 4));
        top = Math.Clamp(top, area.Top + 4, Math.Max(area.Top + 4, area.Bottom - height - 4));

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            left,
            top,
            0,
            0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private void UpdateElapsed()
    {
        var elapsed = _clock.Elapsed;
        TimerText.Text = elapsed.TotalHours >= 1
            ? $"{(int)elapsed.TotalHours}:{elapsed.Minutes:00}:{elapsed.Seconds:00}"
            : $"{elapsed.Minutes:00}:{elapsed.Seconds:00}";
    }

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        StopButton.IsEnabled = false;
        StopRequested?.Invoke(this, EventArgs.Empty);
    }
}
