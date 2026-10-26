using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;
using DrawingBitmap = System.Drawing.Bitmap;

namespace HaloCap.UI;

public partial class ScreenshotOverlayWindow : Window
{
    private readonly ScreenshotSession _session;
    private readonly ScreenMonitor _monitor;
    private readonly CaptureSessionMode _mode;
    private readonly DrawingBitmap? _frozenBitmap;
    private readonly BitmapSource? _frozenSource;

    private IntPtr _handle;
    private bool _closed;
    private bool _dragging;
    private bool _hasSelection;
    private PhysicalRect _startPx;
    private PhysicalRect _selectionPx;
    private double _mapScaleX = 1.0;
    private double _mapScaleY = 1.0;
    private TranslateTransform? _toolbarOffset;

    internal ScreenshotOverlayWindow(
        ScreenshotSession session,
        ScreenMonitor monitor,
        CaptureSessionMode mode,
        DrawingBitmap? frozenBitmap)
    {
        InitializeComponent();

        _session = session;
        _monitor = monitor;
        _mode = mode;
        _frozenBitmap = frozenBitmap;

        if (frozenBitmap is not null)
        {
            _frozenSource = ScreenCaptureService.ToBitmapSource(frozenBitmap);
            FrozenImage.Source = _frozenSource;
        }
        else
        {
            FrozenImage.Visibility = Visibility.Collapsed;
        }

        ConfigureMode();
        ConfigureWindowBounds();
        RefreshMapping();
        SourceInitialized += (_, _) => ApplyOverlayWindowStyle();

        Loaded += (_, _) =>
        {
            RefreshMapping();
            ResetToIdle();
        };
        SizeChanged += (_, _) => RefreshMapping();
        Closed += (_, _) =>
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }

            _frozenBitmap?.Dispose();
            FrozenImage.Source = null;
        };
    }

    internal CaptureSessionMode Mode => _mode;

    internal ScreenMonitor Monitor => _monitor;

    internal bool HasSelection => _hasSelection;

    internal PhysicalRect LocalSelection => _selectionPx;

    internal PhysicalRect VirtualSelection => ScreenGeometry.ToVirtual(_selectionPx, _monitor.Bounds);

    internal void PrepareAndShow()
    {
        if (_closed)
        {
            return;
        }

        var helper = new WindowInteropHelper(this);
        _handle = helper.EnsureHandle();

        Show();
        ApplyPhysicalBounds();

        Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (_closed)
                {
                    return;
                }

                ApplyPhysicalBounds();
                RefreshMapping();
                ResetToIdle();
                PositionCrosshairFromCursor();
            }));
    }

    internal void ActivateForSelection()
    {
        if (_closed)
        {
            return;
        }

        try
        {
            Activate();
            MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口可能已被其他显示器事件关闭。
        }

        PositionCrosshairFromCursor();
    }

    internal BitmapSource? CreateSelectionBitmap()
    {
        if (_frozenBitmap is null || !_hasSelection)
        {
            return null;
        }

        return ScreenCaptureService.CropToBitmapSource(
            _frozenBitmap,
            _selectionPx,
            96.0 * (_monitor.Scale > 0.1 ? _monitor.Scale : 1.0));
    }

    internal void ClearSelectionFromSession()
    {
        if (_closed || _dragging)
        {
            return;
        }

        ResetToIdle();
    }

    internal void CloseOverlay()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放鼠标捕获时的异常。
        }

        Close();
    }

    private void ConfigureWindowBounds()
    {
        var scale = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = _monitor.Bounds.Left / scale;
        Top = _monitor.Bounds.Top / scale;
        Width = Math.Max(1, _monitor.Bounds.Width / scale);
        Height = Math.Max(1, _monitor.Bounds.Height / scale);

        DimFull.Width = Math.Max(0, Width);
        DimFull.Height = Math.Max(0, Height);
        Canvas.SetLeft(DimFull, 0);
        Canvas.SetTop(DimFull, 0);
    }

    private void ApplyOverlayWindowStyle()
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero)
        {
            return;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE).ToInt64();
        exStyle |= NativeMethods.WS_EX_TOOLWINDOW;
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

    private void ApplyPhysicalBounds()
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
            _monitor.Bounds.Left,
            _monitor.Bounds.Top,
            _monitor.Bounds.Width,
            _monitor.Bounds.Height,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private void ConfigureMode()
    {
        Color accent;

        switch (_mode)
        {
            case CaptureSessionMode.Record:
                accent = Color.FromRgb(0xFF, 0x7A, 0x7A);
                HintText.Text = "拖动选择录屏区域 · 松开后可开始录制 · Esc 取消";
                PinButton.Visibility = Visibility.Collapsed;
                CopyButton.Visibility = Visibility.Collapsed;
                SaveButton.Visibility = Visibility.Collapsed;
                OcrButton.Visibility = Visibility.Collapsed;
                StartRecordButton.Visibility = Visibility.Visible;
                break;

            case CaptureSessionMode.Ocr:
                accent = Color.FromRgb(0xB0, 0x8C, 0xFF);
                HintText.Text = "拖动选择要识别的区域 · 松开后立即识别 · Esc 取消";
                break;

            case CaptureSessionMode.Pin:
                accent = Color.FromRgb(0xF5, 0xB9, 0x63);
                HintText.Text = "拖动选择贴图区域 · 松开后立即贴图 · Esc 取消";
                break;

            default:
                accent = Color.FromRgb(0x7C, 0xA8, 0xFF);
                HintText.Text = "拖动选择截图区域 · Enter 复制 · Esc 取消";
                break;
        }

        var brush = new SolidColorBrush(accent);
        brush.Freeze();
        SelectionBorder.BorderBrush = brush;
        CornerTopLeft.Fill = brush;
        CornerTopRight.Fill = brush;
        CornerBottomLeft.Fill = brush;
        CornerBottomRight.Fill = brush;
        HintDot.Fill = brush;
    }

    private void RefreshMapping()
    {
        if (Root.ActualWidth > 0.5)
        {
            _mapScaleX = _monitor.Bounds.Width / Root.ActualWidth;
        }
        else
        {
            _mapScaleX = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        }

        if (Root.ActualHeight > 0.5)
        {
            _mapScaleY = _monitor.Bounds.Height / Root.ActualHeight;
        }
        else
        {
            _mapScaleY = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        }

        DimFull.Width = Math.Max(0, Root.ActualWidth);
        DimFull.Height = Math.Max(0, Root.ActualHeight);
        Canvas.SetLeft(DimFull, 0);
        Canvas.SetTop(DimFull, 0);
    }

    private void Root_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (_closed || e.ChangedButton != MouseButton.Left)
        {
            return;
        }

        if (Toolbar.Visibility == Visibility.Visible && Toolbar.IsMouseOver)
        {
            return;
        }

        e.Handled = true;
        Activate();
        MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
        Keyboard.Focus(Root);
        _session.NotifySelectionStarted(this);

        _dragging = true;
        _hasSelection = false;
        _selectionPx = default;

        var point = ToLocalPhysical(e.GetPosition(Root));
        _startPx = new PhysicalRect(
            Math.Clamp(point.Left, 0, _monitor.Bounds.Width),
            Math.Clamp(point.Top, 0, _monitor.Bounds.Height),
            0,
            0);

        HideToolbar();
        SizeBadge.Visibility = Visibility.Collapsed;
        HintChip.Visibility = Visibility.Collapsed;
        HideCrosshair();
        DimFull.Visibility = Visibility.Collapsed;
        ShowEmptySelectionChrome();

        Mouse.Capture(Root);
    }

    private void Root_PreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (_closed)
        {
            return;
        }

        var point = e.GetPosition(Root);

        if (_dragging)
        {
            var current = ToLocalPhysical(point);
            _selectionPx = ScreenGeometry.NormalizeSelection(
                _startPx.Left,
                _startPx.Top,
                current.Left,
                current.Top,
                _monitor.Bounds.Width,
                _monitor.Bounds.Height);

            UpdateSelectionVisual(showSize: true);
            e.Handled = true;
            return;
        }

        if (!_hasSelection && (Toolbar.Visibility != Visibility.Visible || !Toolbar.IsMouseOver))
        {
            UpdateCrosshair(point);
        }
        else
        {
            HideCrosshair();
        }
    }

    private void Root_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_closed || !_dragging)
        {
            return;
        }

        e.Handled = true;
        _dragging = false;
        Mouse.Capture(null);

        if (!ScreenGeometry.IsUsableSelection(_selectionPx))
        {
            ResetToIdle();
            return;
        }

        _hasSelection = true;
        UpdateSelectionVisual(showSize: true);
        HintChip.Visibility = Visibility.Collapsed;

        if (_mode == CaptureSessionMode.Ocr)
        {
            _session.RequestAction(this, CaptureCommand.Ocr);
        }
        else if (_mode == CaptureSessionMode.Pin)
        {
            _session.RequestAction(this, CaptureCommand.Pin);
        }
        else
        {
            ShowToolbar();
            Keyboard.Focus(Root);
        }
    }

    private void Root_PreviewMouseRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (_closed)
        {
            return;
        }

        e.Handled = true;
        _session.CancelAll();
    }

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (_closed)
        {
            return;
        }

        if (e.Key == Key.Escape)
        {
            e.Handled = true;
            _session.CancelAll();
            return;
        }

        if (!_hasSelection)
        {
            return;
        }

        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            RequestDefaultAction();
            return;
        }

        if (e.Key == Key.C && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            if (_mode != CaptureSessionMode.Record)
            {
                _session.RequestAction(this, CaptureCommand.Copy);
            }
            return;
        }

        if (e.Key == Key.S && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            if (_mode != CaptureSessionMode.Record)
            {
                _session.RequestAction(this, CaptureCommand.Save);
            }
        }
    }

    private void ActionButton_Click(object sender, RoutedEventArgs e)
    {
        if (_closed || sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        var command = tag switch
        {
            "Copy" => CaptureCommand.Copy,
            "Save" => CaptureCommand.Save,
            "Pin" => CaptureCommand.Pin,
            "Ocr" => CaptureCommand.Ocr,
            "StartRecording" => CaptureCommand.StartRecording,
            _ => CaptureCommand.Cancel
        };

        if (command == CaptureCommand.Cancel)
        {
            _session.CancelAll();
            return;
        }

        if (!_hasSelection)
        {
            return;
        }

        _session.RequestAction(this, command);
    }

    private void RequestDefaultAction()
    {
        var command = _mode switch
        {
            CaptureSessionMode.Record => CaptureCommand.StartRecording,
            CaptureSessionMode.Ocr => CaptureCommand.Ocr,
            CaptureSessionMode.Pin => CaptureCommand.Pin,
            _ => CaptureCommand.Copy
        };

        _session.RequestAction(this, command);
    }

    private void ResetToIdle()
    {
        if (_closed)
        {
            return;
        }

        _dragging = false;
        _hasSelection = false;
        _selectionPx = default;

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放捕获异常。
        }

        HideToolbar();
        HideCrosshair();
        SizeBadge.Visibility = Visibility.Collapsed;
        SelectionBorder.Visibility = Visibility.Collapsed;
        SetCornersVisible(false);
        DimFull.Visibility = Visibility.Visible;
        DimTop.Visibility = Visibility.Collapsed;
        DimBottom.Visibility = Visibility.Collapsed;
        DimLeft.Visibility = Visibility.Collapsed;
        DimRight.Visibility = Visibility.Collapsed;
        HintChip.Visibility = Visibility.Visible;

        PositionCrosshairFromCursor();
    }

    private void ShowEmptySelectionChrome()
    {
        SelectionBorder.Visibility = Visibility.Visible;
        SetCornersVisible(true);
        UpdateSelectionVisual(showSize: false);
    }

    private void UpdateSelectionVisual(bool showSize)
    {
        var width = Root.ActualWidth;
        var height = Root.ActualHeight;

        if (width <= 0.5 || height <= 0.5)
        {
            return;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var selectionWidth = Math.Max(0, _selectionPx.Width / _mapScaleX);
        var selectionHeight = Math.Max(0, _selectionPx.Height / _mapScaleY);
        var right = Math.Min(width, left + selectionWidth);
        var bottom = Math.Min(height, top + selectionHeight);

        left = Math.Clamp(left, 0, width);
        top = Math.Clamp(top, 0, height);
        selectionWidth = Math.Max(0, right - left);
        selectionHeight = Math.Max(0, bottom - top);

        DimFull.Visibility = Visibility.Collapsed;
        UpdateDimRect(DimTop, 0, 0, width, top);
        UpdateDimRect(DimBottom, 0, bottom, width, Math.Max(0, height - bottom));
        UpdateDimRect(DimLeft, 0, top, left, selectionHeight);
        UpdateDimRect(DimRight, right, top, Math.Max(0, width - right), selectionHeight);

        SelectionBorder.Visibility = Visibility.Visible;
        SelectionBorder.Width = selectionWidth;
        SelectionBorder.Height = selectionHeight;
        Canvas.SetLeft(SelectionBorder, left);
        Canvas.SetTop(SelectionBorder, top);

        SetCornerPosition(CornerTopLeft, left, top);
        SetCornerPosition(CornerTopRight, right, top);
        SetCornerPosition(CornerBottomLeft, left, bottom);
        SetCornerPosition(CornerBottomRight, right, bottom);
        SetCornersVisible(true);

        if (showSize)
        {
            UpdateSizeBadge(left, top, selectionWidth, selectionHeight);
        }
    }

    private static void UpdateDimRect(
        FrameworkElement rectangle,
        double left,
        double top,
        double width,
        double height)
    {
        var visible = width > 0.5 && height > 0.5;
        rectangle.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        rectangle.Width = Math.Max(0, width);
        rectangle.Height = Math.Max(0, height);
        Canvas.SetLeft(rectangle, left);
        Canvas.SetTop(rectangle, top);
    }

    private static void SetCornerPosition(FrameworkElement corner, double x, double y)
    {
        Canvas.SetLeft(corner, x - (corner.Width / 2));
        Canvas.SetTop(corner, y - (corner.Height / 2));
    }

    private void SetCornersVisible(bool visible)
    {
        var state = visible ? Visibility.Visible : Visibility.Collapsed;
        CornerTopLeft.Visibility = state;
        CornerTopRight.Visibility = state;
        CornerBottomLeft.Visibility = state;
        CornerBottomRight.Visibility = state;
    }

    private void UpdateSizeBadge(double left, double top, double selectionWidth, double selectionHeight)
    {
        SizeText.Text = $"{_selectionPx.Width} × {_selectionPx.Height}";
        SizeBadge.Visibility = Visibility.Visible;
        SizeBadge.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

        var badge = SizeBadge.DesiredSize;
        var badgeTop = top - badge.Height - 8;
        if (badgeTop < 6)
        {
            badgeTop = top + 9;
        }

        var badgeLeft = Math.Clamp(left, 6, Math.Max(6, Root.ActualWidth - badge.Width - 6));
        badgeTop = Math.Clamp(badgeTop, 6, Math.Max(6, Root.ActualHeight - badge.Height - 6));
        SizeBadge.Margin = new Thickness(badgeLeft, badgeTop, 0, 0);
    }

    private void ShowToolbar()
    {
        Toolbar.Visibility = Visibility.Visible;
        Toolbar.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));

        var desired = Toolbar.DesiredSize;
        var left = _selectionPx.Left / _mapScaleX;
        var top = (_selectionPx.Top + _selectionPx.Height) / _mapScaleY + 12;
        var selectionTop = _selectionPx.Top / _mapScaleY;
        var rootWidth = Root.ActualWidth;
        var rootHeight = Root.ActualHeight;

        if (top + desired.Height > rootHeight - 8)
        {
            top = selectionTop - desired.Height - 12;
        }

        if (top < 8)
        {
            top = Math.Clamp(
                (_selectionPx.Top + _selectionPx.Height) / _mapScaleY - desired.Height - 10,
                8,
                Math.Max(8, rootHeight - desired.Height - 8));
        }

        left = Math.Clamp(left, 8, Math.Max(8, rootWidth - desired.Width - 8));
        top = Math.Clamp(top, 8, Math.Max(8, rootHeight - desired.Height - 8));
        Toolbar.Margin = new Thickness(left, top, 0, 0);

        var offset = new TranslateTransform(0, 6);
        _toolbarOffset = offset;
        Toolbar.RenderTransform = offset;
        Toolbar.Opacity = 0;

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Toolbar.BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(120)) { EasingFunction = ease });
        offset.BeginAnimation(
            TranslateTransform.YProperty,
            new DoubleAnimation(6, 0, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });
    }

    private void HideToolbar()
    {
        Toolbar.BeginAnimation(OpacityProperty, null);
        if (_toolbarOffset is not null)
        {
            _toolbarOffset.BeginAnimation(TranslateTransform.YProperty, null);
            _toolbarOffset = null;
        }

        Toolbar.Opacity = 1;
        Toolbar.Visibility = Visibility.Collapsed;
    }

    private void UpdateCrosshair(Point point)
    {
        if (_hasSelection || _dragging)
        {
            HideCrosshair();
            return;
        }

        if (point.X < 0 ||
            point.Y < 0 ||
            point.X > Root.ActualWidth ||
            point.Y > Root.ActualHeight)
        {
            HideCrosshair();
            return;
        }

        CrossVertical.Visibility = Visibility.Visible;
        CrossHorizontal.Visibility = Visibility.Visible;
        CrossVertical.Height = Root.ActualHeight;
        CrossHorizontal.Width = Root.ActualWidth;
        Canvas.SetLeft(CrossVertical, Math.Round(point.X));
        Canvas.SetTop(CrossVertical, 0);
        Canvas.SetLeft(CrossHorizontal, 0);
        Canvas.SetTop(CrossHorizontal, Math.Round(point.Y));
    }

    private void HideCrosshair()
    {
        CrossVertical.Visibility = Visibility.Collapsed;
        CrossHorizontal.Visibility = Visibility.Collapsed;
    }

    private void PositionCrosshairFromCursor()
    {
        if (_closed || _hasSelection || _dragging)
        {
            return;
        }

        if (!NativeMethods.GetCursorPos(out var cursor))
        {
            return;
        }

        var point = new Point(
            (cursor.X - _monitor.Bounds.Left) / _mapScaleX,
            (cursor.Y - _monitor.Bounds.Top) / _mapScaleY);

        UpdateCrosshair(point);
    }

    private PhysicalRect ToLocalPhysical(Point dipPoint)
    {
        var x = (int)Math.Round(dipPoint.X * _mapScaleX);
        var y = (int)Math.Round(dipPoint.Y * _mapScaleY);
        return new PhysicalRect(x, y, 0, 0);
    }
}
