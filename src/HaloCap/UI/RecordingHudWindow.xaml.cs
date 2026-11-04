using System;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.UI;

public partial class RecordingHudWindow : Window
{
    private static readonly Brush RecordingBrush = new SolidColorBrush(Color.FromRgb(0xE0, 0x31, 0x2B));
    private static readonly Brush PausedBrush = new SolidColorBrush(Color.FromRgb(0xE0, 0x8A, 0x00));

    private readonly ScreenMonitor _monitor;
    private PhysicalRect _anchor;
    private IntPtr _handle;
    private bool _prepared;
    private bool _closed;
    private bool _pausedShown;

    internal RecordingHudWindow(ScreenMonitor monitor, PhysicalRect anchor)
    {
        InitializeComponent();

        _monitor = monitor;
        _anchor = anchor;

        Closed += (_, _) =>
        {
            _closed = true;
            PulseDot.BeginAnimation(OpacityProperty, null);
        };
    }

    internal event EventHandler? StopRequested;

    internal event EventHandler? PauseRequested;

    internal event EventHandler? ResumeRequested;

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
        StartPulse();

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });
    }

    /// <summary>窗口录制时跟随目标窗口移动，保持浮层贴在窗口上方。</summary>
    internal void UpdateAnchor(PhysicalRect anchor)
    {
        if (_closed || anchor.Equals(_anchor))
        {
            return;
        }

        _anchor = anchor;

        if (IsVisible)
        {
            PlaceWindow(_handle);
        }
    }

    internal void UpdateState(RecordingHudState state)
    {
        if (_closed)
        {
            return;
        }

        var elapsed = state.Elapsed;

        TimerText.Text = elapsed.TotalHours >= 1
            ? $"{(int)elapsed.TotalHours}:{elapsed.Minutes:00}:{elapsed.Seconds:00}"
            : $"{elapsed.Minutes:00}:{elapsed.Seconds:00}";

        InfoText.Text = state.Info;
        UpdateLevels(state);

        if (state.Paused != _pausedShown)
        {
            _pausedShown = state.Paused;
            StateText.Text = state.Paused ? "PAUSE" : "REC";
            StateText.Foreground = state.Paused ? PausedBrush : RecordingBrush;
            PauseLabel.Text = state.Paused ? "继续" : "暂停";
            PauseButton.ToolTip = state.Paused ? "继续录制" : "暂停录制";
            System.Windows.Automation.AutomationProperties.SetName(
                PauseButton,
                state.Paused ? "继续录制" : "暂停录制");

            if (state.Paused)
            {
                StopPulse();
            }
            else
            {
                StartPulse();
            }
        }
    }

    private void UpdateLevels(RecordingHudState state)
    {
        var showSystem = state.ShowSystemLevel;
        var showMic = state.ShowMicLevel;

        SystemMeterTrack.Visibility = showSystem ? Visibility.Visible : Visibility.Collapsed;
        MicMeterTrack.Visibility = showMic ? Visibility.Visible : Visibility.Collapsed;
        LevelRow.Visibility = showSystem || showMic ? Visibility.Visible : Visibility.Collapsed;

        if (showSystem)
        {
            SystemMeter.Width = MeterWidth(state.SystemLevel);
        }

        if (showMic)
        {
            MicMeter.Width = MeterWidth(state.MicLevel);
        }
    }

    private static double MeterWidth(float level)
    {
        const double track = 46;
        var value = level < 0f ? 0f : level > 1f ? 1f : level;
        return Math.Round(track * Math.Sqrt(value) * 10) / 10;
    }

    internal void CloseHud()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        PulseDot.BeginAnimation(OpacityProperty, null);
        Close();
    }

    private void StartPulse()
    {
        PulseDot.BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(1.0, 0.35, TimeSpan.FromMilliseconds(720))
            {
                AutoReverse = true,
                RepeatBehavior = RepeatBehavior.Forever,
                EasingFunction = new SineEase { EasingMode = EasingMode.EaseInOut }
            });
    }

    private void StopPulse()
    {
        PulseDot.BeginAnimation(OpacityProperty, null);
        PulseDot.Opacity = 0.55;
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

        var left = _anchor.Left + ((_anchor.Width - width) / 2);
        var top = _anchor.Top - height - margin;

        if (top < area.Top + 4)
        {
            top = _anchor.Bottom + margin;
        }

        if (top + height > area.Bottom - 4)
        {
            top = _anchor.Top + margin;
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

    private void StopButton_Click(object sender, RoutedEventArgs e)
    {
        StopButton.IsEnabled = false;
        PauseButton.IsEnabled = false;
        StopRequested?.Invoke(this, EventArgs.Empty);
    }

    private void PauseButton_Click(object sender, RoutedEventArgs e)
    {
        if (_pausedShown)
        {
            ResumeRequested?.Invoke(this, EventArgs.Empty);
            return;
        }

        PauseRequested?.Invoke(this, EventArgs.Empty);
    }
}

/// <summary>录制浮层一次刷新需要的状态。</summary>
internal readonly record struct RecordingHudState(
    TimeSpan Elapsed,
    bool Paused,
    string Info,
    bool ShowSystemLevel,
    float SystemLevel,
    bool ShowMicLevel,
    float MicLevel);
