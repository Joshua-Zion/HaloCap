using System;
using System.Collections.Generic;
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
    private const double HandleHitRadius = 9;
    private const int MinSelectionSizePx = 8;

    private enum DragKind
    {
        None,
        Create,
        Move,
        Resize
    }

    private readonly ScreenshotSession _session;
    private readonly ScreenMonitor _monitor;
    private readonly CaptureSessionMode _mode;
    private readonly DrawingBitmap? _frozenBitmap;
    private readonly BitmapSource? _frozenSource;
    private readonly List<AnnotationStroke> _strokes = new();

    private IntPtr _handle;
    private bool _closed;
    private bool _dragging;
    private bool _hasSelection;
    private DragKind _dragKind;
    private int _resizeMask;
    private PhysicalRect _dragOrigin;
    private PhysicalRect _dragStartPx;
    private PhysicalRect _startPx;
    private PhysicalRect _selectionPx;
    private double _mapScaleX = 1.0;
    private double _mapScaleY = 1.0;
    private TranslateTransform? _toolbarOffset;

    private bool _annotating;
    private AnnotationTool _annotationTool = AnnotationTool.Pen;
    private Color _annotationColor = AnnotationDefaults.Palette[0];
    private AnnotationStroke? _liveStroke;
    private bool _drawingAnnotation;
    private TextBox? _textEditor;
    private Point _textPoint;

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
        UpdateAnnotationToolVisuals();
        UpdateAnnotationColorVisuals();
        SourceInitialized += (_, _) => ApplyOverlayWindowStyle();

        Loaded += (_, _) =>
        {
            RefreshMapping();
            ResetToIdle();
        };
        SizeChanged += (_, _) =>
        {
            RefreshMapping();
            UpdateAnnotationFrame();
            PositionToolbars(animate: false);
        };
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

    internal BitmapSource? CreateSelectionBitmap(bool includeAnnotations)
    {
        if (_frozenBitmap is null || !_hasSelection)
        {
            return null;
        }

        var scale = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        var cropped = ScreenCaptureService.CropToBitmapSource(
            _frozenBitmap,
            _selectionPx,
            96.0 * scale);

        return includeAnnotations && _strokes.Count > 0
            ? AnnotationRenderer.Compose(cropped, _strokes)
            : cropped;
    }

    internal BitmapSource ComposeAnnotations(BitmapSource image)
    {
        return _strokes.Count > 0
            ? AnnotationRenderer.Compose(image, _strokes)
            : image;
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
        CancelTextEditor();

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
                HintText.Text = "拖动选择录屏区域 · 可调整选区 · Enter 开始录制 · Esc 取消";
                PinButton.Visibility = Visibility.Collapsed;
                CopyButton.Visibility = Visibility.Collapsed;
                SaveButton.Visibility = Visibility.Collapsed;
                OcrButton.Visibility = Visibility.Collapsed;
                AnnotateButton.Visibility = Visibility.Collapsed;
                StartRecordButton.Visibility = Visibility.Visible;
                break;

            case CaptureSessionMode.Ocr:
                accent = Color.FromRgb(0xB0, 0x8C, 0xFF);
                HintText.Text = "拖动选择要识别的区域 · 松开后立即识别 · Esc 取消";
                AnnotateButton.Visibility = Visibility.Collapsed;
                break;

            case CaptureSessionMode.Pin:
                accent = Color.FromRgb(0xF5, 0xB9, 0x63);
                HintText.Text = "拖动选择贴图区域 · 松开后立即贴图 · Esc 取消";
                AnnotateButton.Visibility = Visibility.Collapsed;
                break;

            default:
                accent = Color.FromRgb(0x7C, 0xA8, 0xFF);
                HintText.Text = "拖动选择区域 · 可拖动/缩放选区 · 标注 (Space) · Enter 复制 · Esc 取消";
                break;
        }

        var brush = new SolidColorBrush(accent);
        brush.Freeze();
        SelectionBorder.BorderBrush = brush;
        CornerTopLeft.Fill = brush;
        CornerTopRight.Fill = brush;
        CornerBottomLeft.Fill = brush;
        CornerBottomRight.Fill = brush;
        EdgeTop.Fill = brush;
        EdgeBottom.Fill = brush;
        EdgeLeft.Fill = brush;
        EdgeRight.Fill = brush;
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

        if (AnnotationBar.Visibility == Visibility.Visible && AnnotationBar.IsMouseOver)
        {
            return;
        }

        if (IsWithinTextEditor(e.OriginalSource))
        {
            return;
        }

        if (_textEditor is not null)
        {
            CommitTextEditor();
        }

        e.Handled = true;
        Activate();
        MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
        Keyboard.Focus(Root);

        var point = e.GetPosition(Root);

        if (_annotating && _hasSelection)
        {
            BeginAnnotationStroke(point);
            return;
        }

        if (_hasSelection && !_dragging)
        {
            var mask = HitTestSelectionHandle(point);

            if (mask != 0)
            {
                _dragKind = DragKind.Resize;
                _resizeMask = mask;
                _dragOrigin = _selectionPx;
                _dragStartPx = ToLocalPhysical(point);
                _dragging = true;
                Mouse.Capture(Root);
                return;
            }

            if (IsInsideSelection(point))
            {
                _dragKind = DragKind.Move;
                _dragOrigin = _selectionPx;
                _dragStartPx = ToLocalPhysical(point);
                _dragging = true;
                Mouse.Capture(Root);
                return;
            }
        }

        _session.NotifySelectionStarted(this);

        ClearAnnotations();
        SetAnnotationMode(false);

        _dragKind = DragKind.Create;
        _dragging = true;
        _hasSelection = false;
        _selectionPx = default;

        var physical = ToLocalPhysical(point);
        _startPx = new PhysicalRect(
            Math.Clamp(physical.Left, 0, _monitor.Bounds.Width),
            Math.Clamp(physical.Top, 0, _monitor.Bounds.Height),
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

        if (_drawingAnnotation)
        {
            UpdateAnnotationStroke(point);
            e.Handled = true;
            return;
        }

        if (_dragging)
        {
            var current = ToLocalPhysical(point);

            switch (_dragKind)
            {
                case DragKind.Create:
                    _selectionPx = ScreenGeometry.NormalizeSelection(
                        _startPx.Left,
                        _startPx.Top,
                        current.Left,
                        current.Top,
                        _monitor.Bounds.Width,
                        _monitor.Bounds.Height);
                    break;

                case DragKind.Move:
                    ApplyMove(current);
                    break;

                case DragKind.Resize:
                    ApplyResize(current);
                    break;
            }

            UpdateSelectionVisual(showSize: true);
            UpdateAnnotationFrame();
            PositionToolbars(animate: false);
            e.Handled = true;
            return;
        }

        if (_hasSelection)
        {
            if (!_annotating &&
                (Toolbar.Visibility != Visibility.Visible || !Toolbar.IsMouseOver) &&
                (AnnotationBar.Visibility != Visibility.Visible || !AnnotationBar.IsMouseOver))
            {
                UpdateSelectionCursor(point);
            }

            HideCrosshair();
            return;
        }

        if (Toolbar.Visibility != Visibility.Visible || !Toolbar.IsMouseOver)
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
        if (_closed)
        {
            return;
        }

        if (_drawingAnnotation)
        {
            e.Handled = true;
            FinishAnnotationStroke();
            return;
        }

        if (!_dragging)
        {
            return;
        }

        e.Handled = true;
        _dragging = false;
        Mouse.Capture(null);

        var kind = _dragKind;
        _dragKind = DragKind.None;

        if (kind != DragKind.Create)
        {
            UpdateSelectionVisual(showSize: true);
            PositionToolbars(animate: false);
            return;
        }

        if (!ScreenGeometry.IsUsableSelection(_selectionPx))
        {
            ResetToIdle();
            return;
        }

        _hasSelection = true;
        UpdateSelectionVisual(showSize: true);
        UpdateAnnotationFrame();
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
        if (_closed || _textEditor is not null)
        {
            return;
        }

        if (e.Key == Key.Escape)
        {
            e.Handled = true;

            if (_annotating)
            {
                SetAnnotationMode(false);
                return;
            }

            _session.CancelAll();
            return;
        }

        if (e.Key == Key.Space && _mode == CaptureSessionMode.Screenshot && _hasSelection)
        {
            e.Handled = true;
            SetAnnotationMode(!_annotating);
            return;
        }

        if (_annotating && e.Key == Key.Z && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            UndoAnnotation();
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

        CommitTextEditor();
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

        CommitTextEditor();
        _session.RequestAction(this, command);
    }

    private void ResetToIdle()
    {
        if (_closed)
        {
            return;
        }

        _dragging = false;
        _dragKind = DragKind.None;
        _hasSelection = false;
        _selectionPx = default;

        ClearAnnotations();
        SetAnnotationMode(false);

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
        AnnotationLayer.Visibility = Visibility.Collapsed;
        EditorLayer.Children.Clear();
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
        SetCornersVisible(!_annotating);
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
        SetCornerPosition(EdgeTop, left + (selectionWidth / 2), top);
        SetCornerPosition(EdgeBottom, left + (selectionWidth / 2), bottom);
        SetCornerPosition(EdgeLeft, left, top + (selectionHeight / 2));
        SetCornerPosition(EdgeRight, right, top + (selectionHeight / 2));
        SetCornersVisible(!_annotating && (_hasSelection || _dragging));

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
        EdgeTop.Visibility = state;
        EdgeBottom.Visibility = state;
        EdgeLeft.Visibility = state;
        EdgeRight.Visibility = state;
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

    private void ApplyMove(PhysicalRect current)
    {
        var dx = current.Left - _dragStartPx.Left;
        var dy = current.Top - _dragStartPx.Top;

        _selectionPx = ScreenGeometry.MoveWithin(
            _dragOrigin,
            dx,
            dy,
            _monitor.Bounds.Width,
            _monitor.Bounds.Height);
    }

    private void ApplyResize(PhysicalRect current)
    {
        _selectionPx = ScreenGeometry.ResizeEdges(
            _dragOrigin,
            _resizeMask,
            current.Left,
            current.Top,
            _monitor.Bounds.Width,
            _monitor.Bounds.Height,
            MinSelectionSizePx);
    }

    private int HitTestSelectionHandle(Point dip)
    {
        if (!_hasSelection)
        {
            return 0;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var right = left + (_selectionPx.Width / _mapScaleX);
        var bottom = top + (_selectionPx.Height / _mapScaleY);

        var nearLeft = Math.Abs(dip.X - left) <= HandleHitRadius &&
                       dip.Y >= top - HandleHitRadius &&
                       dip.Y <= bottom + HandleHitRadius;
        var nearRight = Math.Abs(dip.X - right) <= HandleHitRadius &&
                        dip.Y >= top - HandleHitRadius &&
                        dip.Y <= bottom + HandleHitRadius;
        var nearTop = Math.Abs(dip.Y - top) <= HandleHitRadius &&
                      dip.X >= left - HandleHitRadius &&
                      dip.X <= right + HandleHitRadius;
        var nearBottom = Math.Abs(dip.Y - bottom) <= HandleHitRadius &&
                         dip.X >= left - HandleHitRadius &&
                         dip.X <= right + HandleHitRadius;

        return (nearLeft ? 1 : 0) |
               (nearRight ? 2 : 0) |
               (nearTop ? 4 : 0) |
               (nearBottom ? 8 : 0);
    }

    private bool IsInsideSelection(Point dip)
    {
        if (!_hasSelection)
        {
            return false;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var right = left + (_selectionPx.Width / _mapScaleX);
        var bottom = top + (_selectionPx.Height / _mapScaleY);

        return dip.X >= left && dip.X <= right && dip.Y >= top && dip.Y <= bottom;
    }

    private void UpdateSelectionCursor(Point dip)
    {
        var mask = HitTestSelectionHandle(dip);

        Root.Cursor = mask switch
        {
            1 | 4 or 2 | 8 => Cursors.SizeNWSE,
            2 | 4 or 1 | 8 => Cursors.SizeNESW,
            1 or 2 => Cursors.SizeWE,
            4 or 8 => Cursors.SizeNS,
            _ => IsInsideSelection(dip) ? Cursors.SizeAll : Cursors.Cross
        };
    }

    private void ShowToolbar()
    {
        Toolbar.Visibility = Visibility.Visible;
        PositionToolbars(animate: false);

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

    private void PositionToolbars(bool animate)
    {
        _ = animate;

        if (Toolbar.Visibility != Visibility.Visible || !_hasSelection)
        {
            return;
        }

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

        if (AnnotationBar.Visibility == Visibility.Visible)
        {
            PositionAnnotationBar(top, desired.Height, left);
        }
    }

    private void PositionAnnotationBar(double mainTop, double mainHeight, double mainLeft)
    {
        AnnotationBar.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var desired = AnnotationBar.DesiredSize;
        var below = mainTop + mainHeight + 8;
        var above = mainTop - desired.Height - 8;

        var top = below + desired.Height <= Root.ActualHeight - 8 ? below : above;
        var left = Math.Clamp(
            mainLeft,
            8,
            Math.Max(8, Root.ActualWidth - desired.Width - 8));
        top = Math.Clamp(top, 8, Math.Max(8, Root.ActualHeight - desired.Height - 8));
        AnnotationBar.Margin = new Thickness(left, top, 0, 0);
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
        AnnotationBar.Visibility = Visibility.Collapsed;
    }

    private void UpdateCrosshair(Point point)
    {
        if (_hasSelection || _dragging || _annotating)
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
        if (_closed || _hasSelection || _dragging || _annotating)
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

    private void UpdateAnnotationFrame()
    {
        if (!_hasSelection)
        {
            AnnotationLayer.Visibility = Visibility.Collapsed;
            return;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var width = Math.Max(0, _selectionPx.Width / _mapScaleX);
        var height = Math.Max(0, _selectionPx.Height / _mapScaleY);

        AnnotationLayer.Visibility = Visibility.Visible;
        Canvas.SetLeft(AnnotationView, left);
        Canvas.SetTop(AnnotationView, top);
        AnnotationView.Width = width;
        AnnotationView.Height = height;
        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void AnnotateButton_Click(object sender, RoutedEventArgs e)
    {
        e.Handled = true;
        SetAnnotationMode(!_annotating);
    }

    private void SetAnnotationMode(bool enabled)
    {
        if (enabled && (!_hasSelection || _mode != CaptureSessionMode.Screenshot))
        {
            return;
        }

        if (_annotating == enabled)
        {
            return;
        }

        _annotating = enabled;
        AnnotationBar.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;

        if (!enabled)
        {
            CancelTextEditor();
        }

        SetCornersVisible(_hasSelection && !enabled);
        AnnotateButton.Background = enabled
            ? UiColors.Brush("#337CA8FF")
            : Brushes.Transparent;

        Root.Cursor = Cursors.Cross;
        HideCrosshair();

        if (_hasSelection)
        {
            UpdateAnnotationFrame();
            PositionToolbars(animate: false);
        }

        Keyboard.Focus(Root);
    }

    private void AnnotationTool_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        if (!Enum.TryParse<AnnotationTool>(tag, out var tool))
        {
            return;
        }

        _annotationTool = tool;
        UpdateAnnotationToolVisuals();
        Keyboard.Focus(Root);
    }

    private void AnnotationColor_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        _annotationColor = UiColors.Parse(tag);
        UpdateAnnotationColorVisuals();
        Keyboard.Focus(Root);
    }

    private void AnnotationAction_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        switch (tag)
        {
            case "Undo":
                UndoAnnotation();
                break;

            case "Clear":
                ClearAnnotations();
                break;

            case "Done":
                SetAnnotationMode(false);
                break;
        }

        Keyboard.Focus(Root);
    }

    private void UpdateAnnotationToolVisuals()
    {
        var selected = UiColors.Brush("#33FFFFFF");

        var buttons = new (Button Button, AnnotationTool Tool)[]
        {
            (ToolPenButton, AnnotationTool.Pen),
            (ToolRectangleButton, AnnotationTool.Rectangle),
            (ToolEllipseButton, AnnotationTool.Ellipse),
            (ToolArrowButton, AnnotationTool.Arrow),
            (ToolTextButton, AnnotationTool.Text),
            (ToolHighlighterButton, AnnotationTool.Highlighter)
        };

        foreach (var (button, tool) in buttons)
        {
            button.Background = tool == _annotationTool ? selected : Brushes.Transparent;
        }
    }

    private void UpdateAnnotationColorVisuals()
    {
        var buttons = new (Button Button, string Hex)[]
        {
            (ColorRedButton, "#FFFF5A5A"),
            (ColorYellowButton, "#FFFFD166"),
            (ColorGreenButton, "#FF7CE38B"),
            (ColorBlueButton, "#FF6CA8FF"),
            (ColorWhiteButton, "#FFFFFFFF")
        };

        foreach (var (button, hex) in buttons)
        {
            var color = UiColors.Parse(hex);
            button.BorderBrush = color == _annotationColor ? Brushes.White : Brushes.Transparent;
        }
    }

    private void BeginAnnotationStroke(Point rootPoint)
    {
        var point = RootToSelectionDip(rootPoint);

        if (point.X < 0 ||
            point.Y < 0 ||
            point.X > AnnotationView.ActualWidth ||
            point.Y > AnnotationView.ActualHeight)
        {
            return;
        }

        if (_annotationTool == AnnotationTool.Text)
        {
            BeginTextEditor(rootPoint, point);
            return;
        }

        _liveStroke = new AnnotationStroke
        {
            Tool = _annotationTool,
            Color = _annotationColor,
            Thickness = _annotationTool == AnnotationTool.Highlighter
                ? AnnotationDefaults.HighlighterThickness
                : AnnotationDefaults.Thickness
        };

        _liveStroke.Points.Add(point);
        _drawingAnnotation = true;
        Mouse.Capture(Root);
        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void UpdateAnnotationStroke(Point rootPoint)
    {
        if (_liveStroke is null)
        {
            return;
        }

        var point = RootToSelectionDip(rootPoint);

        if (_annotationTool is AnnotationTool.Pen or AnnotationTool.Highlighter)
        {
            var last = _liveStroke.Points[_liveStroke.Points.Count - 1];
            if ((point - last).Length < 0.8)
            {
                return;
            }

            _liveStroke.Points.Add(point);
        }
        else
        {
            if (_liveStroke.Points.Count == 1)
            {
                _liveStroke.Points.Add(point);
            }
            else
            {
                _liveStroke.Points[1] = point;
            }
        }

        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void FinishAnnotationStroke()
    {
        _drawingAnnotation = false;
        Mouse.Capture(null);

        var stroke = _liveStroke;
        _liveStroke = null;

        if (stroke is not null && IsMeaningfulStroke(stroke))
        {
            _strokes.Add(stroke);
        }

        AnnotationView.SetContent(_strokes, null, 1.0);
    }

    private static bool IsMeaningfulStroke(AnnotationStroke stroke)
    {
        if (stroke.Points.Count < 2)
        {
            return false;
        }

        if (stroke.Tool is AnnotationTool.Pen or AnnotationTool.Highlighter)
        {
            return true;
        }

        var vector = stroke.Points[1] - stroke.Points[0];
        return Math.Abs(vector.X) > 2 || Math.Abs(vector.Y) > 2;
    }

    private Point RootToSelectionDip(Point rootPoint)
    {
        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        return new Point(rootPoint.X - left, rootPoint.Y - top);
    }

    private void UndoAnnotation()
    {
        if (_liveStroke is not null)
        {
            _liveStroke = null;
            _drawingAnnotation = false;
            Mouse.Capture(null);
        }
        else if (_strokes.Count > 0)
        {
            _strokes.RemoveAt(_strokes.Count - 1);
        }

        AnnotationView.SetContent(_strokes, null, 1.0);
    }

    private void ClearAnnotations()
    {
        CancelTextEditor();
        _strokes.Clear();
        _liveStroke = null;
        _drawingAnnotation = false;
        AnnotationView.SetContent(_strokes, null, 1.0);
    }

    private void BeginTextEditor(Point rootPoint, Point selectionPoint)
    {
        CancelTextEditor();

        var color = _annotationColor;
        var brush = new SolidColorBrush(color);
        brush.Freeze();

        var box = new TextBox
        {
            MinWidth = 90,
            MaxWidth = 360,
            FontSize = AnnotationDefaults.TextSize,
            FontWeight = FontWeights.SemiBold,
            Padding = new Thickness(5, 3, 5, 3),
            Background = UiColors.Brush("#CC10141C"),
            Foreground = brush,
            BorderBrush = brush,
            BorderThickness = new Thickness(1),
            CaretBrush = brush,
            SelectionBrush = brush,
            AcceptsReturn = false
        };

        box.PreviewKeyDown += TextEditor_PreviewKeyDown;
        box.LostFocus += (_, _) => CommitTextEditor();

        Canvas.SetLeft(box, rootPoint.X);
        Canvas.SetTop(box, rootPoint.Y);
        EditorLayer.Children.Add(box);

        _textEditor = box;
        _textPoint = selectionPoint;
        box.Focus();
    }

    private void TextEditor_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            CommitTextEditor();
            Keyboard.Focus(Root);
        }
        else if (e.Key == Key.Escape)
        {
            e.Handled = true;
            CancelTextEditor();
            Keyboard.Focus(Root);
        }
    }

    private void CommitTextEditor()
    {
        var box = _textEditor;

        if (box is null)
        {
            return;
        }

        _textEditor = null;
        var text = box.Text.Trim();
        EditorLayer.Children.Remove(box);

        if (text.Length == 0)
        {
            return;
        }

        var stroke = new AnnotationStroke
        {
            Tool = AnnotationTool.Text,
            Color = _annotationColor,
            Text = text
        };

        stroke.Points.Add(_textPoint);
        _strokes.Add(stroke);
        AnnotationView.SetContent(_strokes, null, 1.0);
    }

    private void CancelTextEditor()
    {
        var box = _textEditor;

        if (box is null)
        {
            return;
        }

        _textEditor = null;
        EditorLayer.Children.Remove(box);
    }

    private bool IsWithinTextEditor(object? source)
    {
        if (_textEditor is null)
        {
            return false;
        }

        var current = source as DependencyObject;

        while (current is not null)
        {
            if (ReferenceEquals(current, _textEditor))
            {
                return true;
            }

            current = current is Visual
                ? VisualTreeHelper.GetParent(current)
                : LogicalTreeHelper.GetParent(current);
        }

        return false;
    }
}
