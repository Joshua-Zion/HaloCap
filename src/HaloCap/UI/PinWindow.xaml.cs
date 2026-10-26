using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.UI;

public partial class PinWindow : Window
{
    private const double MinZoom = 0.12;
    private const double MaxZoom = 8.0;

    private static readonly List<PinWindow> OpenPins = new();

    private readonly BitmapSource _image;
    private readonly PhysicalRect _initialRect;
    private readonly double _monitorScale;
    private readonly ScreenshotImageFormat _format;
    private readonly DispatcherTimer _hudTimer;
    private IntPtr _handle;
    private double _zoom = 1.0;
    private double _opacity = 1.0;

    internal PinWindow(
        BitmapSource image,
        PhysicalRect initialRect,
        double monitorScale,
        ScreenshotImageFormat format)
    {
        InitializeComponent();

        _image = image;
        _initialRect = initialRect.Width > 0 && initialRect.Height > 0
            ? initialRect
            : new PhysicalRect(initialRect.Left, initialRect.Top, image.PixelWidth, image.PixelHeight);
        _monitorScale = monitorScale > 0.1 ? monitorScale : 1.0;
        _format = format;

        PreviewImage.Source = _image;
        WindowStartupLocation = WindowStartupLocation.Manual;

        var initialWidth = Math.Max(24, _initialRect.Width);
        var initialHeight = Math.Max(24, _initialRect.Height);
        Width = initialWidth / _monitorScale;
        Height = initialHeight / _monitorScale;
        Left = _initialRect.Left / _monitorScale;
        Top = _initialRect.Top / _monitorScale;

        _hudTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1250) };
        _hudTimer.Tick += (_, _) =>
        {
            _hudTimer.Stop();
            FadeHudOut();
        };

        OpenPins.Add(this);
        Closed += (_, _) =>
        {
            _hudTimer.Stop();
            OpenPins.Remove(this);
        };
    }

    internal static int OpenPinCount => OpenPins.Count;

    internal static void CloseAllPins()
    {
        foreach (var pin in OpenPins.ToArray())
        {
            try
            {
                pin.Close();
            }
            catch (Exception ex)
            {
                Log.Error("关闭贴图窗口失败", ex);
            }
        }
    }

    internal void ShowPin()
    {
        Opacity = 0;

        Show();
        ApplyPhysicalRect(_initialRect);

        try
        {
            Activate();
            MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口创建期间可能被用户关闭。
        }

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });
    }

    private void ApplyPhysicalRect(PhysicalRect rect)
    {
        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        if (_handle == IntPtr.Zero)
        {
            return;
        }

        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            rect.Left,
            rect.Top,
            Math.Max(1, rect.Width),
            Math.Max(1, rect.Height),
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private void Root_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left)
        {
            return;
        }

        if (e.ClickCount == 2)
        {
            e.Handled = true;
            Close();
            return;
        }

        HideHudImmediately();

        try
        {
            DragMove();
        }
        catch (InvalidOperationException)
        {
            // 鼠标在拖动开始前已经释放。
        }
    }

    private void Window_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if ((Keyboard.Modifiers & ModifierKeys.Control) != 0)
        {
            ChangeOpacity(e.Delta);
        }
        else
        {
            ZoomAtCursor(e.Delta);
        }

        e.Handled = true;
    }

    private void ZoomAtCursor(int wheelDelta)
    {
        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        if (!NativeMethods.GetWindowRect(_handle, out var currentRect) ||
            currentRect.Width <= 0 ||
            currentRect.Height <= 0)
        {
            return;
        }

        var baseWidth = Math.Max(1, _initialRect.Width);
        var baseHeight = Math.Max(1, _initialRect.Height);
        var minZoom = Math.Max(MinZoom, Math.Max(40.0 / baseWidth, 40.0 / baseHeight));
        var factor = wheelDelta > 0 ? 1.12 : 1.0 / 1.12;
        var targetZoom = Math.Clamp(_zoom * factor, minZoom, MaxZoom);

        if (Math.Abs(targetZoom - _zoom) < 0.0005)
        {
            return;
        }

        var newWidth = Math.Max(40, (int)Math.Round(baseWidth * targetZoom));
        var newHeight = Math.Max(40, (int)Math.Round(baseHeight * targetZoom));

        var cursorAvailable = NativeMethods.GetCursorPos(out var cursor);
        var fractionX = 0.5;
        var fractionY = 0.5;

        if (cursorAvailable)
        {
            fractionX = Math.Clamp((cursor.X - currentRect.Left) / (double)currentRect.Width, 0.0, 1.0);
            fractionY = Math.Clamp((cursor.Y - currentRect.Top) / (double)currentRect.Height, 0.0, 1.0);
        }

        var newLeft = cursorAvailable
            ? (int)Math.Round(cursor.X - (fractionX * newWidth))
            : currentRect.Left - ((newWidth - currentRect.Width) / 2);
        var newTop = cursorAvailable
            ? (int)Math.Round(cursor.Y - (fractionY * newHeight))
            : currentRect.Top - ((newHeight - currentRect.Height) / 2);

        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            newLeft,
            newTop,
            newWidth,
            newHeight,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);

        _zoom = targetZoom;

        var dpi = NativeMethods.GetDpiForWindow(_handle);
        var scale = dpi > 0 ? dpi / 96.0 : _monitorScale;
        if (scale > 0.1)
        {
            Width = newWidth / scale;
            Height = newHeight / scale;
        }

        ShowHud($"缩放 {_zoom * 100:0}%");
    }

    private void ChangeOpacity(int wheelDelta)
    {
        BeginAnimation(OpacityProperty, null);
        _opacity = Math.Clamp(_opacity + (wheelDelta > 0 ? 0.08 : -0.08), 0.15, 1.0);
        Opacity = _opacity;
        ShowHud($"不透明度 {_opacity * 100:0}%");
    }

    private void ResetSizeAndOpacity()
    {
        _zoom = 1.0;
        _opacity = 1.0;
        BeginAnimation(OpacityProperty, null);
        Opacity = 1.0;
        ApplyPhysicalRect(_initialRect);

        var dpi = NativeMethods.GetDpiForWindow(_handle);
        var scale = dpi > 0 ? dpi / 96.0 : _monitorScale;
        Width = Math.Max(24, _initialRect.Width) / scale;
        Height = Math.Max(24, _initialRect.Height) / scale;

        ShowHud("已恢复初始大小");
    }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        switch (e.Key)
        {
            case Key.Escape:
                e.Handled = true;
                Close();
                break;

            case Key.C when Keyboard.Modifiers.HasFlag(ModifierKeys.Control):
                e.Handled = true;
                CopyImage();
                break;

            case Key.S when Keyboard.Modifiers.HasFlag(ModifierKeys.Control):
                e.Handled = true;
                SaveImage();
                break;

            case Key.D0 when Keyboard.Modifiers.HasFlag(ModifierKeys.Control):
                e.Handled = true;
                ResetSizeAndOpacity();
                break;

            case Key.NumPad0 when Keyboard.Modifiers.HasFlag(ModifierKeys.Control):
                e.Handled = true;
                ResetSizeAndOpacity();
                break;
        }
    }

    private void CopyImage()
    {
        try
        {
            ImageClipboard.SetImage(_image);
            ShowHud("已复制到剪贴板");
        }
        catch (Exception ex)
        {
            Log.Error("贴图复制失败", ex);
            ShowHud("复制失败，剪贴板可能被占用");
        }
    }

    private void SaveImage()
    {
        try
        {
            var path = ImageFileService.SaveWithDialog(_image, _format, owner: this);
            if (path is not null)
            {
                ShowHud("图片已保存");
            }
        }
        catch (Exception ex)
        {
            Log.Error("贴图保存失败", ex);
            ShowHud("保存失败：" + ex.Message);
        }
    }

    private void ShowHud(string text)
    {
        HudText.Text = text;
        Hud.BeginAnimation(OpacityProperty, null);
        Hud.Opacity = 1;
        _hudTimer.Stop();
        _hudTimer.Start();
    }

    private void FadeHudOut()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(180))
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
        };

        fade.Completed += (_, _) => Hud.Opacity = 0;
        Hud.BeginAnimation(OpacityProperty, fade);
    }

    private void HideHudImmediately()
    {
        _hudTimer.Stop();
        Hud.BeginAnimation(OpacityProperty, null);
        Hud.Opacity = 0;
    }

    private void CopyMenuItem_Click(object sender, RoutedEventArgs e) => CopyImage();

    private void SaveMenuItem_Click(object sender, RoutedEventArgs e) => SaveImage();

    private void ResetMenuItem_Click(object sender, RoutedEventArgs e) => ResetSizeAndOpacity();

    private void CloseMenuItem_Click(object sender, RoutedEventArgs e) => Close();

    private void CloseAllMenuItem_Click(object sender, RoutedEventArgs e) => CloseAllPins();
}
