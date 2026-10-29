using System;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.UI;

/// <summary>
/// 录制期间标示录制区域的边框：点击穿透、不抢焦点，并通过捕获排除避免被录进视频。
/// </summary>
public partial class RecordingFrameWindow : Window
{
    private readonly PhysicalRect _crop;
    private IntPtr _handle;
    private bool _prepared;
    private bool _closed;

    internal RecordingFrameWindow(PhysicalRect crop)
    {
        InitializeComponent();

        _crop = crop;
        Closed += (_, _) => _closed = true;
    }

    internal void PrepareForCapture()
    {
        if (_prepared || _closed)
        {
            return;
        }

        _handle = new WindowInteropHelper(this).EnsureHandle();
        ApplyStyles();

        try
        {
            NativeMethods.SetWindowDisplayAffinity(_handle, NativeMethods.WDA_EXCLUDEFROMCAPTURE);
        }
        catch (Exception ex)
        {
            Log.Error("设置录制边框捕获排除失败", ex);
        }

        _prepared = true;
    }

    internal void ShowFrame()
    {
        if (_closed)
        {
            return;
        }

        PrepareForCapture();
        Opacity = 0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = 0;
        Top = 0;

        Show();
        ApplyStyles();
        PlaceWindow();

        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(160))
            {
                EasingFunction = new QuadraticEase { EasingMode = EasingMode.EaseOut }
            });
    }

    internal void CloseFrame()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        Close();
    }

    private void ApplyStyles()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(_handle, NativeMethods.GWL_EXSTYLE).ToInt64();
        exStyle |= NativeMethods.WS_EX_NOACTIVATE | NativeMethods.WS_EX_TOOLWINDOW | NativeMethods.WS_EX_TRANSPARENT;
        NativeMethods.SetWindowLongPtr(_handle, NativeMethods.GWL_EXSTYLE, new IntPtr(exStyle));

        try
        {
            NativeMethods.SetWindowDisplayAffinity(_handle, NativeMethods.WDA_EXCLUDEFROMCAPTURE);
        }
        catch (Exception ex)
        {
            Log.Error("设置录制边框捕获排除失败", ex);
        }

        NativeMethods.SetWindowPos(
            _handle,
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

    private void PlaceWindow()
    {
        if (_handle == IntPtr.Zero)
        {
            return;
        }

        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            _crop.Left,
            _crop.Top,
            _crop.Width,
            _crop.Height,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }
}
