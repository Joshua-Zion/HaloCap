using System;
using System.Collections.Generic;
using System.Linq;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
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
    private const double OutlinePaddingDip = 4;

    private static readonly List<PinWindow> OpenPins = new();

    private readonly BitmapSource _image;
    private readonly PhysicalRect _initialRect;
    private readonly double _monitorScale;
    private readonly ScreenshotImageFormat _format;
    private readonly double _imageDipWidth;
    private readonly double _imageDipHeight;
    private readonly List<AnnotationStroke> _strokes = new();
    private readonly List<List<AnnotationStroke>> _undoStack = new();
    private readonly List<List<AnnotationStroke>> _redoStack = new();
    private const int MaxHistory = 40;
    private readonly DispatcherTimer _hudTimer;
    private IntPtr _handle;
    private PhysicalRect _imageRect;
    private double _zoom = 1.0;
    private double _opacity = 1.0;
    private bool _outlineEnabled;
    private bool _draggingWindow;
    private bool _ocrEnabled;
    private NativeMethods.POINT _dragStartCursor;
    private NativeMethods.RECT _dragStartWindowRect;
    private PhysicalRect _dragStartImageRect;
    private bool _toolbarAbove;

    private bool _annotating;
    private AnnotationTool _annotationTool = AnnotationTool.Pen;
    private AnnotationTool _shapeTool = AnnotationTool.Rectangle;
    private Color _annotationColor = AnnotationDefaults.Palette[0];
    private double _strokeThickness = AnnotationDefaults.Thickness;
    private double _highlighterThickness = AnnotationDefaults.HighlighterThickness;
    private double _textSize = AnnotationDefaults.TextSize;
    private int _mosaicBlockDip = AnnotationDefaults.MosaicBlockDip;
    private double _eraserSize = AnnotationDefaults.EraserSize;
    private bool _fillShapes;
    private bool _textBold = false;
    private bool _textItalic;
    private ArrowHeadMode _arrowHead = ArrowHeadMode.End;
    private bool _eraseHistoryPushed;
    private Point _mosaicStart;
    private Int32Rect _mosaicLastRect;
    private AnnotationStroke? _liveStroke;
    private bool _drawingAnnotation;
    private TextBox? _textEditor;
    private Point _textPoint;

    internal PinWindow(
        BitmapSource image,
        PhysicalRect initialRect,
        double monitorScale,
        ScreenshotImageFormat format,
        bool outlineEnabled = true,
        bool ocrEnabled = true)
    {
        InitializeComponent();

        _image = image;
        _initialRect = initialRect.Width > 0 && initialRect.Height > 0
            ? initialRect
            : new PhysicalRect(initialRect.Left, initialRect.Top, image.PixelWidth, image.PixelHeight);
        _imageRect = _initialRect;
        _monitorScale = monitorScale > 0.1 ? monitorScale : 1.0;
        _format = format;
        _imageDipWidth = image.PixelWidth * 96.0 / (image.DpiX > 0.1 ? image.DpiX : 96.0);
        _imageDipHeight = image.PixelHeight * 96.0 / (image.DpiY > 0.1 ? image.DpiY : 96.0);

        _outlineEnabled = outlineEnabled;
        _ocrEnabled = ocrEnabled;
        PinOutline.Visibility = outlineEnabled ? Visibility.Visible : Visibility.Collapsed;
        OutlineMenuItem.IsChecked = outlineEnabled;
        OcrMenuItem.Visibility = ocrEnabled ? Visibility.Visible : Visibility.Collapsed;
        PinContextMenu.Opened += (_, _) => OutlineMenuItem.IsChecked = _outlineEnabled;

        PreviewImage.Source = _image;
        WindowStartupLocation = WindowStartupLocation.Manual;

        var initialWidth = Math.Max(24, _initialRect.Width);
        var initialHeight = Math.Max(24, _initialRect.Height);
        Width = initialWidth / _monitorScale + OutlinePaddingDip * 2;
        Height = initialHeight / _monitorScale + OutlinePaddingDip * 2;
        Left = _initialRect.Left / _monitorScale - OutlinePaddingDip;
        Top = _initialRect.Top / _monitorScale - OutlinePaddingDip;

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

        Root.SizeChanged += (_, _) => UpdateAnnotationScale();
        UpdateAnnotationToolVisuals();
        UpdateAnnotationColorVisuals();
        UpdateHistoryVisuals();
    }

    internal event Action<bool>? OutlineChanged;

    internal event Action<PinWindow>? OcrRequested;

    internal BitmapSource OcrImage => _image;

    internal PhysicalRect OcrAnchor => _imageRect;

    internal ScreenMonitor OcrMonitor => MonitorService.GetMonitorAt(
        _imageRect.Left + (_imageRect.Width / 2),
        _imageRect.Top + (_imageRect.Height / 2));

    internal static int OpenPinCount => OpenPins.Count;

    internal static void SetOutlineForAll(bool enabled)
    {
        foreach (var pin in OpenPins.ToArray())
        {
            pin.SetOutline(enabled);
        }
    }

    internal static void SetOcrEnabledForAll(bool enabled)
    {
        foreach (var pin in OpenPins.ToArray())
        {
            pin.SetOcrEnabled(enabled);
        }
    }

    private void SetOutline(bool enabled)
    {
        _outlineEnabled = enabled;
        PinOutline.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        OutlineMenuItem.IsChecked = enabled;
    }

    private void SetOcrEnabled(bool enabled)
    {
        _ocrEnabled = enabled;
        OcrMenuItem.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
    }

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
        ApplyImageRect(_initialRect);
        UpdateAnnotationScale();

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

    private void ApplyImageRect(PhysicalRect imageRect)
    {
        _imageRect = imageRect;

        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        if (_handle == IntPtr.Zero)
        {
            return;
        }

        var scale = CurrentDpiScale();
        var padding = (int)Math.Round(OutlinePaddingDip * scale);
        var imageWidth = Math.Max(24, imageRect.Width);
        var imageHeight = Math.Max(24, imageRect.Height);
        var imageWindowWidth = imageWidth + padding * 2;
        var toolbarSize = MeasureToolbar();
        var toolbarHeight = (int)Math.Round(toolbarSize.Height * scale);
        var toolbarWidth = (int)Math.Ceiling(toolbarSize.Width * scale);
        var (windowWidth, sideInset) = FitImageWindow(imageWindowWidth, toolbarWidth);
        var toolbarAbove = toolbarHeight > 0 && ShouldPlaceToolbarAbove(imageRect, toolbarHeight, padding);
        var windowHeight = imageHeight + padding * 2 + toolbarHeight;
        var windowTop = toolbarAbove
            ? imageRect.Top - padding - toolbarHeight
            : imageRect.Top - padding;

        ApplyToolbarPlacement(toolbarAbove);

        ImageHost.Width = imageWindowWidth / scale;
        Width = windowWidth / scale;
        Height = windowHeight / scale;

        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            imageRect.Left - padding - sideInset,
            windowTop,
            windowWidth,
            windowHeight,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    internal static (int WindowWidth, int SideInset) FitImageWindow(int imageWindowWidth, int toolbarWidth) =>
        toolbarWidth <= imageWindowWidth
            ? (imageWindowWidth, 0)
            : (toolbarWidth, (toolbarWidth - imageWindowWidth) / 2);

    private bool ShouldPlaceToolbarAbove(PhysicalRect imageRect, int toolbarHeight, int padding)
    {
        var monitor = MonitorService.GetMonitorAt(
            imageRect.Left + (imageRect.Width / 2),
            imageRect.Top + (imageRect.Height / 2));
        return PreferToolbarAbove(imageRect, toolbarHeight, monitor.WorkArea, padding);
    }

    internal static bool PreferToolbarAbove(
        PhysicalRect imageRect,
        int toolbarHeight,
        PhysicalRect workArea,
        int padding)
    {
        var spaceBelow = workArea.Bottom - (imageRect.Top + imageRect.Height) - padding;
        var spaceAbove = imageRect.Top - workArea.Top - padding;
        return spaceBelow < toolbarHeight && spaceAbove > spaceBelow;
    }

    private void ApplyToolbarPlacement(bool above)
    {
        if (_toolbarAbove == above)
        {
            return;
        }

        _toolbarAbove = above;
        Grid.SetRow(AnnotationBarHost, above ? 0 : 2);
        AnnotationBarHost.Margin = above ? new Thickness(4, 4, 4, 6) : new Thickness(4, 6, 4, 4);

        ToolbarStack.Children.Remove(PinOptionBar);
        if (above)
        {
            PinOptionBar.Margin = new Thickness(0, 0, 0, 6);
            ToolbarStack.Children.Insert(0, PinOptionBar);
        }
        else
        {
            PinOptionBar.Margin = new Thickness(0, 6, 0, 0);
            ToolbarStack.Children.Add(PinOptionBar);
        }
    }

    private double CurrentDpiScale()
    {
        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        var dpi = _handle != IntPtr.Zero ? NativeMethods.GetDpiForWindow(_handle) : 0;
        return dpi > 0 ? dpi / 96.0 : _monitorScale;
    }

    private Size MeasureToolbar()
    {
        if (!_annotating)
        {
            return new Size(0, 0);
        }

        AnnotationBarHost.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        return AnnotationBarHost.DesiredSize;
    }

    private void Root_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left || IsPointerOverAnnotationBar())
        {
            return;
        }

        try
        {
            Activate();
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口可能正在关闭。
        }

        if (e.ClickCount == 2)
        {
            e.Handled = true;
            Close();
            return;
        }

        HideHudImmediately();
        BeginWindowDrag();
    }

    private void BeginWindowDrag()
    {
        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        if (_handle == IntPtr.Zero ||
            !NativeMethods.GetCursorPos(out _dragStartCursor) ||
            !NativeMethods.GetWindowRect(_handle, out _dragStartWindowRect))
        {
            return;
        }

        _dragStartImageRect = _imageRect;
        _draggingWindow = true;
        Mouse.Capture(Root);
    }

    private void ContinueWindowDrag()
    {
        if (!_draggingWindow)
        {
            return;
        }

        if (Mouse.LeftButton != MouseButtonState.Pressed)
        {
            EndWindowDrag();
            return;
        }

        if (!NativeMethods.GetCursorPos(out var cursor))
        {
            return;
        }

        var dx = cursor.X - _dragStartCursor.X;
        var dy = cursor.Y - _dragStartCursor.Y;

        // 直接按物理像素移动，不交给系统拖动，避免贴图在屏幕边缘被拉回。
        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            _dragStartWindowRect.Left + dx,
            _dragStartWindowRect.Top + dy,
            0,
            0,
            NativeMethods.SWP_NOSIZE | NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);

        _imageRect = new PhysicalRect(
            _dragStartImageRect.Left + dx,
            _dragStartImageRect.Top + dy,
            _imageRect.Width,
            _imageRect.Height);
    }

    private void EndWindowDrag()
    {
        if (!_draggingWindow)
        {
            return;
        }

        _draggingWindow = false;

        if (Mouse.Captured == Root)
        {
            Mouse.Capture(null);
        }

        if (_annotating)
        {
            // 拖到屏幕底部后重新判断工具栏位置，必要时翻到图片上方。
            ApplyImageRect(_imageRect);
        }
    }

    private void Window_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (IsPointerOverAnnotationBar())
        {
            return;
        }

        if (_annotating)
        {
            e.Handled = true;
            AdjustBrush(e.Delta > 0 ? 1 : -1);
            return;
        }

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
        if (_imageRect.Width <= 0 || _imageRect.Height <= 0)
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
            fractionX = Math.Clamp((cursor.X - _imageRect.Left) / (double)_imageRect.Width, 0.0, 1.0);
            fractionY = Math.Clamp((cursor.Y - _imageRect.Top) / (double)_imageRect.Height, 0.0, 1.0);
        }

        var newLeft = cursorAvailable
            ? (int)Math.Round(cursor.X - (fractionX * newWidth))
            : _imageRect.Left - ((newWidth - _imageRect.Width) / 2);
        var newTop = cursorAvailable
            ? (int)Math.Round(cursor.Y - (fractionY * newHeight))
            : _imageRect.Top - ((newHeight - _imageRect.Height) / 2);

        _zoom = targetZoom;
        ApplyImageRect(new PhysicalRect(newLeft, newTop, newWidth, newHeight));
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

        var width = Math.Max(24, _initialRect.Width);
        var height = Math.Max(24, _initialRect.Height);
        var centerX = _imageRect.Left + (_imageRect.Width / 2.0);
        var centerY = _imageRect.Top + (_imageRect.Height / 2.0);

        ApplyImageRect(new PhysicalRect(
            (int)Math.Round(centerX - (width / 2.0)),
            (int)Math.Round(centerY - (height / 2.0)),
            width,
            height));

        ShowHud("已恢复实际大小");
    }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (_textEditor is not null)
        {
            return;
        }

        if (e.Key == Key.Space)
        {
            e.Handled = true;
            SetAnnotationMode(!_annotating);
            return;
        }

        if (e.Key == Key.Escape)
        {
            e.Handled = true;
            if (_annotating)
            {
                SetAnnotationMode(false);
            }
            else
            {
                Close();
            }

            return;
        }

        if (!_annotating && e.Key == Key.O && Keyboard.Modifiers == ModifierKeys.None)
        {
            e.Handled = true;
            RequestOcr();
            return;
        }

        if (_annotating)
        {
            if (Keyboard.Modifiers == ModifierKeys.Shift && e.Key is Key.R or Key.E)
            {
                e.Handled = true;
                _fillShapes = true;
                _shapeTool = e.Key == Key.R
                    ? AnnotationTool.Rectangle
                    : AnnotationTool.Ellipse;
                _annotationTool = _shapeTool;
                UpdateAnnotationToolVisuals();
                Keyboard.Focus(Root);
                return;
            }

            if (Keyboard.Modifiers == ModifierKeys.None)
            {
                if (GetToolForKey(e.Key) is { } tool)
                {
                    e.Handled = true;

                    if (tool is AnnotationTool.Rectangle or AnnotationTool.Ellipse)
                    {
                        _shapeTool = tool;
                        _fillShapes = false;
                    }

                    _annotationTool = tool;
                    UpdateAnnotationToolVisuals();
                    Keyboard.Focus(Root);
                    return;
                }

                if (e.Key == Key.F)
                {
                    e.Handled = true;
                    ToggleFill();
                    return;
                }

                if (e.Key == Key.OemOpenBrackets)
                {
                    e.Handled = true;
                    AdjustBrush(-1);
                    return;
                }

                if (e.Key == Key.OemCloseBrackets)
                {
                    e.Handled = true;
                    AdjustBrush(1);
                    return;
                }
            }

            if (_annotationTool == AnnotationTool.Text &&
                Keyboard.Modifiers == ModifierKeys.Control &&
                (e.Key == Key.B || e.Key == Key.I))
            {
                e.Handled = true;

                if (e.Key == Key.B)
                {
                    _textBold = !_textBold;
                }
                else
                {
                    _textItalic = !_textItalic;
                }

                UpdateAnnotationToolVisuals();
                ShowHud(e.Key == Key.B
                    ? (_textBold ? "加粗" : "取消加粗")
                    : (_textItalic ? "斜体" : "取消斜体"));
                return;
            }

            if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control) &&
                (e.Key == Key.Z || e.Key == Key.Y))
            {
                e.Handled = true;

                if (e.Key == Key.Y || Keyboard.Modifiers.HasFlag(ModifierKeys.Shift))
                {
                    RedoAnnotation();
                }
                else
                {
                    UndoAnnotation();
                }

                return;
            }
        }

        switch (e.Key)
        {
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

    private static AnnotationTool? GetToolForKey(Key key) => key switch
    {
        Key.R => AnnotationTool.Rectangle,
        Key.E => AnnotationTool.Ellipse,
        Key.A => AnnotationTool.Arrow,
        Key.P => AnnotationTool.Pen,
        Key.T => AnnotationTool.Text,
        Key.H => AnnotationTool.Highlighter,
        Key.M => AnnotationTool.Mosaic,
        Key.N => AnnotationTool.Step,
        Key.X => AnnotationTool.Eraser,
        _ => null
    };

    private void CopyImage()
    {
        try
        {
            ImageClipboard.SetImage(GetExportImage());
            ShowHud(_strokes.Count > 0 ? "已复制（含标注）" : "已复制到剪贴板");
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
            var path = ImageFileService.SaveWithDialog(GetExportImage(), _format, owner: this);
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

    private BitmapSource GetExportImage()
    {
        if (_strokes.Count == 0)
        {
            return _image;
        }

        RefreshMosaicTiles();
        return AnnotationRenderer.Compose(_image, _strokes);
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

    private double AnnotationScale => Root.ActualWidth > 0.5 && _imageDipWidth > 0.5
        ? Root.ActualWidth / _imageDipWidth
        : 1.0;

    private void UpdateAnnotationScale()
    {
        AnnotationView.SetContent(_strokes, _liveStroke, AnnotationScale);
    }

    private void SetAnnotationMode(bool enabled)
    {
        if (_annotating == enabled)
        {
            return;
        }

        _annotating = enabled;
        AnnotationBarHost.Visibility = enabled ? Visibility.Visible : Visibility.Collapsed;
        ApplyImageRect(_imageRect);

        if (!enabled)
        {
            CancelTextEditor();
            CancelLiveStroke();
        }

        Root.Cursor = enabled ? Cursors.Cross : Cursors.SizeAll;
        UpdateAnnotationToolVisuals();
        UpdateHistoryVisuals();
        Keyboard.Focus(Root);

        if (enabled)
        {
            ShowHud("标注模式 · 滚轮调参数 · X 橡皮 · 空格退出");
        }
    }

    private bool IsPointerOverAnnotationBar() =>
        AnnotationBarHost.Visibility == Visibility.Visible && AnnotationBarHost.IsMouseOver;

    private void Root_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!_annotating || e.ChangedButton != MouseButton.Left || IsPointerOverAnnotationBar())
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

        var display = e.GetPosition(Root);

        if (display.X < 0 ||
            display.Y < 0 ||
            display.X > Root.ActualWidth ||
            display.Y > Root.ActualHeight)
        {
            return;
        }

        e.Handled = true;

        try
        {
            Activate();
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口可能正在关闭。
        }

        HideHudImmediately();

        var imagePoint = ToImagePoint(display);

        if (_annotationTool == AnnotationTool.Eraser)
        {
            BeginErase(imagePoint);
            return;
        }

        if (_annotationTool == AnnotationTool.Text)
        {
            BeginTextEditor(display, imagePoint);
            return;
        }

        if (_annotationTool == AnnotationTool.Step)
        {
            PlaceStep(imagePoint);
            return;
        }

        _liveStroke = new AnnotationStroke
        {
            Tool = _annotationTool,
            Color = _annotationColor,
            Filled = _fillShapes,
            Thickness = _annotationTool == AnnotationTool.Highlighter
                ? _highlighterThickness
                : _strokeThickness,
            TextSize = _textSize,
            Bold = _textBold,
            Italic = _textItalic,
            ArrowHead = _arrowHead
        };

        if (_annotationTool == AnnotationTool.Mosaic)
        {
            _mosaicStart = imagePoint;
            _mosaicLastRect = default;
            _liveStroke.Points.Add(imagePoint);
            _liveStroke.Points.Add(imagePoint);
            UpdateMosaicStroke(_liveStroke, imagePoint);
        }
        else
        {
            _liveStroke.Points.Add(imagePoint);
        }

        _drawingAnnotation = true;
        Mouse.Capture(Root);
        UpdateAnnotationScale();
    }

    private void Root_PreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (_draggingWindow)
        {
            e.Handled = true;
            ContinueWindowDrag();
            return;
        }

        if (!_annotating || !_drawingAnnotation)
        {
            return;
        }

        var point = ToImagePoint(e.GetPosition(Root));

        if (_annotationTool == AnnotationTool.Eraser)
        {
            e.Handled = true;
            EraseAt(point);
            return;
        }

        if (_liveStroke is null)
        {
            return;
        }

        var shift = Keyboard.Modifiers.HasFlag(ModifierKeys.Shift);

        if (_annotationTool == AnnotationTool.Mosaic)
        {
            UpdateMosaicStroke(
                _liveStroke,
                shift ? AnnotationGeometry.ConstrainSquare(_mosaicStart, point) : point);
            e.Handled = true;
            return;
        }

        if (_annotationTool is AnnotationTool.Pen or AnnotationTool.Highlighter)
        {
            if (shift)
            {
                if (_liveStroke.Points.Count > 2)
                {
                    _liveStroke.Points.RemoveRange(2, _liveStroke.Points.Count - 2);
                }

                if (_liveStroke.Points.Count == 1)
                {
                    _liveStroke.Points.Add(point);
                }
                else
                {
                    _liveStroke.Points[1] = point;
                }
            }
            else
            {
                var last = _liveStroke.Points[_liveStroke.Points.Count - 1];
                if ((point - last).Length < 0.8)
                {
                    return;
                }

                _liveStroke.Points.Add(point);
            }
        }
        else
        {
            var end = point;

            if (shift && _liveStroke.Points.Count > 0)
            {
                end = _annotationTool == AnnotationTool.Arrow
                    ? AnnotationGeometry.SnapToAngle(_liveStroke.Points[0], point)
                    : AnnotationGeometry.ConstrainSquare(_liveStroke.Points[0], point);
            }

            if (_liveStroke.Points.Count == 1)
            {
                _liveStroke.Points.Add(end);
            }
            else
            {
                _liveStroke.Points[1] = end;
            }
        }

        e.Handled = true;
        UpdateAnnotationScale();
    }

    private void Root_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_draggingWindow)
        {
            e.Handled = true;
            EndWindowDrag();
            return;
        }

        if (!_drawingAnnotation)
        {
            return;
        }

        e.Handled = true;
        _drawingAnnotation = false;
        _eraseHistoryPushed = false;
        Mouse.Capture(null);

        var stroke = _liveStroke;
        _liveStroke = null;

        if (stroke is not null && IsMeaningfulStroke(stroke))
        {
            PushHistory();
            _strokes.Add(stroke);
            ShowHud("已添加标注 · Ctrl+Z 撤销");
        }

        UpdateAnnotationScale();
        UpdateHistoryVisuals();
    }

    private void BeginErase(Point imagePoint)
    {
        _drawingAnnotation = true;
        _eraseHistoryPushed = false;
        Mouse.Capture(Root);
        EraseAt(imagePoint);
    }

    private void EraseAt(Point imagePoint)
    {
        var radius = _eraserSize / 2;
        var removed = false;

        for (var i = _strokes.Count - 1; i >= 0; i--)
        {
            if (!AnnotationHitTester.HitTest(_strokes[i], imagePoint, radius))
            {
                continue;
            }

            if (!_eraseHistoryPushed)
            {
                PushHistory();
                _eraseHistoryPushed = true;
            }

            _strokes.RemoveAt(i);
            removed = true;
        }

        if (!removed)
        {
            return;
        }

        RefreshMosaicTiles();
        UpdateAnnotationScale();
        UpdateHistoryVisuals();
        ShowHud("已擦除标注 · Ctrl+Z 撤销");
    }

    private void PlaceStep(Point imagePoint)
    {
        var radius = AnnotationDefaults.StepRadius;
        var maxX = Math.Max(radius, _imageDipWidth - radius);
        var maxY = Math.Max(radius, _imageDipHeight - radius);
        var center = new Point(
            Math.Clamp(imagePoint.X, radius, maxX),
            Math.Clamp(imagePoint.Y, radius, maxY));

        var number = 1;

        foreach (var stroke in _strokes)
        {
            if (stroke.Tool == AnnotationTool.Step &&
                int.TryParse(stroke.Text, out var existing) &&
                existing >= number)
            {
                number = existing + 1;
            }
        }

        var step = new AnnotationStroke
        {
            Tool = AnnotationTool.Step,
            Color = _annotationColor,
            Text = number.ToString()
        };

        step.Points.Add(center);
        PushHistory();
        _strokes.Add(step);
        UpdateAnnotationScale();
        UpdateHistoryVisuals();
        ShowHud($"序号 {number}");
    }

    private void UpdateMosaicStroke(AnnotationStroke stroke, Point current)
    {
        var pixelsPerDip = _image.DpiX > 0.1 ? _image.DpiX / 96.0 : 1.0;
        var rect = AnnotationGeometry.NormalizeRect(_mosaicStart, current);

        if (rect.Width < 0.5 || rect.Height < 0.5)
        {
            return;
        }

        var left = (int)Math.Round(rect.Left * pixelsPerDip);
        var top = (int)Math.Round(rect.Top * pixelsPerDip);
        var right = (int)Math.Round(rect.Right * pixelsPerDip);
        var bottom = (int)Math.Round(rect.Bottom * pixelsPerDip);
        var limit = new PhysicalRect(0, 0, _image.PixelWidth, _image.PixelHeight);
        var blockPixels = Math.Max(4, (int)Math.Round(_mosaicBlockDip * pixelsPerDip));
        var snapped = MosaicRenderer.SnapToBlocks(left, top, right, bottom, limit, blockPixels);

        if (snapped.Width <= 0 || snapped.Height <= 0)
        {
            return;
        }

        if (snapped.Equals(_mosaicLastRect) && stroke.Mosaic is not null)
        {
            return;
        }

        var tile = CreateMosaicTile(snapped, blockPixels, _strokes, pixelsPerDip);

        if (tile is null)
        {
            return;
        }

        _mosaicLastRect = snapped;
        stroke.Mosaic = tile;
        stroke.MosaicBlockPixels = blockPixels;
        stroke.Points[0] = new Point(snapped.X / pixelsPerDip, snapped.Y / pixelsPerDip);
        stroke.Points[1] = new Point(
            (snapped.X + snapped.Width) / pixelsPerDip,
            (snapped.Y + snapped.Height) / pixelsPerDip);
        UpdateAnnotationScale();
    }

    private BitmapSource? CreateMosaicTile(
        Int32Rect snapped,
        int blockPixels,
        IReadOnlyList<AnnotationStroke> priorStrokes,
        double pixelsPerDip)
    {
        var transform = new Matrix(
            pixelsPerDip,
            0,
            0,
            pixelsPerDip,
            -snapped.X,
            -snapped.Y);

        return MosaicRenderer.CreateCompositedTile(
            _image,
            snapped,
            priorStrokes,
            transform,
            blockPixels);
    }

    private bool RefreshMosaicTiles()
    {
        var pixelsPerDip = _image.DpiX > 0.1 ? _image.DpiX / 96.0 : 1.0;
        var limit = new PhysicalRect(0, 0, _image.PixelWidth, _image.PixelHeight);
        var prior = new List<AnnotationStroke>();
        var changed = false;

        foreach (var stroke in _strokes)
        {
            if (stroke.Tool == AnnotationTool.Mosaic &&
                stroke.Points.Count >= 2 &&
                stroke.MosaicBlockPixels > 0)
            {
                var left = (int)Math.Round(stroke.Points[0].X * pixelsPerDip);
                var top = (int)Math.Round(stroke.Points[0].Y * pixelsPerDip);
                var right = (int)Math.Round(stroke.Points[1].X * pixelsPerDip);
                var bottom = (int)Math.Round(stroke.Points[1].Y * pixelsPerDip);
                var snapped = MosaicRenderer.SnapToBlocks(
                    left,
                    top,
                    right,
                    bottom,
                    limit,
                    stroke.MosaicBlockPixels);

                if (snapped.Width > 0 && snapped.Height > 0)
                {
                    var tile = CreateMosaicTile(snapped, stroke.MosaicBlockPixels, prior, pixelsPerDip);

                    if (tile is not null)
                    {
                        stroke.Mosaic = tile;
                        stroke.Points[0] = new Point(snapped.X / pixelsPerDip, snapped.Y / pixelsPerDip);
                        stroke.Points[1] = new Point(
                            (snapped.X + snapped.Width) / pixelsPerDip,
                            (snapped.Y + snapped.Height) / pixelsPerDip);
                        changed = true;
                    }
                }
            }

            prior.Add(stroke);
        }

        return changed;
    }

    private void AdjustBrush(int direction)
    {
        if (!_annotating)
        {
            return;
        }

        string label;

        switch (_annotationTool)
        {
            case AnnotationTool.Text:
                _textSize = Math.Clamp(
                    _textSize + (direction * 2),
                    AnnotationDefaults.MinTextSize,
                    AnnotationDefaults.MaxTextSize);
                label = $"字号 {_textSize:0}";
                break;

            case AnnotationTool.Highlighter:
                _highlighterThickness = Math.Clamp(
                    _highlighterThickness + (direction * 2),
                    AnnotationDefaults.MinHighlighterThickness,
                    AnnotationDefaults.MaxHighlighterThickness);
                label = $"荧光笔 {_highlighterThickness:0}";
                break;

            case AnnotationTool.Mosaic:
                _mosaicBlockDip = Math.Clamp(
                    _mosaicBlockDip + (direction * 2),
                    AnnotationDefaults.MinMosaicBlockDip,
                    AnnotationDefaults.MaxMosaicBlockDip);
                _mosaicLastRect = default;
                label = $"马赛克 {_mosaicBlockDip}";
                break;

            case AnnotationTool.Step:
                return;

            case AnnotationTool.Eraser:
                _eraserSize = Math.Clamp(
                    _eraserSize + (direction * 2),
                    AnnotationDefaults.MinEraserSize,
                    AnnotationDefaults.MaxEraserSize);
                label = $"橡皮 {_eraserSize:0}";
                break;

            default:
                _strokeThickness = Math.Clamp(
                    _strokeThickness + direction,
                    AnnotationDefaults.MinThickness,
                    AnnotationDefaults.MaxThickness);
                label = $"粗细 {_strokeThickness:0}";
                break;
        }

        if (_liveStroke is not null &&
            _annotationTool is not AnnotationTool.Mosaic and not AnnotationTool.Text)
        {
            _liveStroke.Thickness = _annotationTool == AnnotationTool.Highlighter
                ? _highlighterThickness
                : _strokeThickness;
            UpdateAnnotationScale();
        }

        ShowHud(label);
        UpdateAnnotationToolVisuals();
    }

    private void AnnotationTool_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        if (tag == "Shape")
        {
            _annotationTool = _shapeTool;
            UpdateAnnotationToolVisuals();
            Keyboard.Focus(Root);
            return;
        }

        if (!Enum.TryParse<AnnotationTool>(tag, out var tool))
        {
            return;
        }

        if (tool is AnnotationTool.Rectangle or AnnotationTool.Ellipse)
        {
            _shapeTool = tool;
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

            case "Redo":
                RedoAnnotation();
                break;

            case "Clear":
                ClearAnnotationStrokes();
                break;

            case "Close":
            case "Done":
                SetAnnotationMode(false);
                break;
        }

        Keyboard.Focus(Root);
    }

    private void AnnotateMenuItem_Click(object sender, RoutedEventArgs e)
    {
        SetAnnotationMode(!_annotating);
    }

    private void OcrMenuItem_Click(object sender, RoutedEventArgs e) => RequestOcr();

    private void RequestOcr()
    {
        if (!_ocrEnabled)
        {
            return;
        }

        OcrRequested?.Invoke(this);
    }

    private void UpdateAnnotationToolVisuals()
    {
        var accent = UiColors.Brush("#FF2F7CF6");
        var normal = UiColors.Brush("#FF3C4250");
        var isShape = _annotationTool is AnnotationTool.Rectangle or AnnotationTool.Ellipse;

        var shapeActive = _annotationTool is
            AnnotationTool.Rectangle or AnnotationTool.Ellipse;
        ToolShapeButton.Background = shapeActive ? accent : Brushes.Transparent;
        ToolShapeButton.Foreground = shapeActive ? Brushes.White : normal;

        var buttons = new (Button Button, AnnotationTool Tool)[]
        {
            (ToolArrowButton, AnnotationTool.Arrow),
            (ToolPenButton, AnnotationTool.Pen),
            (ToolHighlighterButton, AnnotationTool.Highlighter),
            (ToolEraserButton, AnnotationTool.Eraser),
            (ToolTextButton, AnnotationTool.Text),
            (ToolMosaicButton, AnnotationTool.Mosaic),
            (ToolStepButton, AnnotationTool.Step)
        };

        foreach (var (button, tool) in buttons)
        {
            var active = tool == _annotationTool;
            button.Background = active ? accent : Brushes.Transparent;
            button.Foreground = active ? Brushes.White : normal;
        }

        var showColors = _annotationTool is not (AnnotationTool.Mosaic or AnnotationTool.Eraser);
        PinColorGroup.Visibility = showColors ? Visibility.Visible : Visibility.Collapsed;
        PinColorSeparator.Visibility = showColors && _annotationTool != AnnotationTool.Step
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinShapeKindGroup.Visibility = isShape ? Visibility.Visible : Visibility.Collapsed;
        PinShapeGroup.Visibility = isShape ? Visibility.Visible : Visibility.Collapsed;
        PinBrushGroup.Visibility = _annotationTool is AnnotationTool.Pen or AnnotationTool.Arrow
            or AnnotationTool.Highlighter or AnnotationTool.Rectangle or AnnotationTool.Ellipse
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinArrowGroup.Visibility = _annotationTool == AnnotationTool.Arrow
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinTextGroup.Visibility = _annotationTool == AnnotationTool.Text
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinTextStyleGroup.Visibility = _annotationTool == AnnotationTool.Text
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinMosaicGroup.Visibility = _annotationTool == AnnotationTool.Mosaic
            ? Visibility.Visible
            : Visibility.Collapsed;
        PinEraserGroup.Visibility = _annotationTool == AnnotationTool.Eraser
            ? Visibility.Visible
            : Visibility.Collapsed;

        SetOptionButtonState(ShapeRectangleButton, _annotationTool == AnnotationTool.Rectangle, accent, normal);
        SetOptionButtonState(ShapeEllipseButton, _annotationTool == AnnotationTool.Ellipse, accent, normal);
        SetOptionButtonState(ShapeOutlineButton, !_fillShapes, accent, normal);
        SetOptionButtonState(ShapeFillButton, _fillShapes, accent, normal);

        SetOptionButtonState(ArrowEndButton, _arrowHead == ArrowHeadMode.End, accent, normal);
        SetOptionButtonState(ArrowBothButton, _arrowHead == ArrowHeadMode.Both, accent, normal);
        SetOptionButtonState(ArrowLineButton, _arrowHead == ArrowHeadMode.None, accent, normal);
        SetOptionButtonState(BoldToggleButton, _textBold, accent, normal);
        SetOptionButtonState(ItalicToggleButton, _textItalic, accent, normal);

        BrushValueText.Text = (_annotationTool == AnnotationTool.Highlighter
            ? _highlighterThickness
            : _strokeThickness).ToString("0");
        TextValueText.Text = _textSize.ToString("0");
        MosaicValueText.Text = _mosaicBlockDip.ToString();
        EraserValueText.Text = _eraserSize.ToString("0");
    }

    private static void SetOptionButtonState(Button button, bool active, Brush accent, Brush normal)
    {
        button.Background = active ? accent : Brushes.Transparent;
        button.Foreground = active ? Brushes.White : normal;
    }

    private void ShapeKind_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        var shape = tag == "Ellipse" ? AnnotationTool.Ellipse : AnnotationTool.Rectangle;
        _shapeTool = shape;
        _annotationTool = shape;
        UpdateAnnotationToolVisuals();
        ShowHud(tag == "Ellipse" ? "椭圆" : "矩形");
        Keyboard.Focus(Root);
    }

    private void ShapeStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        _fillShapes = tag == "Fill";
        UpdateAnnotationToolVisuals();
        ShowHud(_fillShapes ? "实心填充" : "空心");
        Keyboard.Focus(Root);
    }

    private void ToggleFill()
    {
        _fillShapes = !_fillShapes;
        UpdateAnnotationToolVisuals();
        ShowHud(_fillShapes ? "实心填充" : "空心");
    }

    private void ArrowStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element ||
            element.Tag is not string tag ||
            !Enum.TryParse<ArrowHeadMode>(tag, out var mode))
        {
            return;
        }

        e.Handled = true;
        _arrowHead = mode;
        UpdateAnnotationToolVisuals();
        ShowHud(mode switch
        {
            ArrowHeadMode.Both => "双向箭头",
            ArrowHeadMode.None => "直线",
            _ => "末端箭头"
        });
        Keyboard.Focus(Root);
    }

    private void TextStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;

        switch (tag)
        {
            case "Bold":
                _textBold = !_textBold;
                break;

            case "Italic":
                _textItalic = !_textItalic;
                break;
        }

        if (_textEditor is not null)
        {
            _textEditor.FontWeight = _textBold ? FontWeights.Bold : FontWeights.Normal;
            _textEditor.FontStyle = _textItalic ? FontStyles.Italic : FontStyles.Normal;
        }

        UpdateAnnotationToolVisuals();
        ShowHud(tag == "Bold"
            ? (_textBold ? "加粗" : "取消加粗")
            : (_textItalic ? "斜体" : "取消斜体"));
        Keyboard.Focus(Root);
    }

    private void OptionStep_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        AdjustBrush(tag == "Up" ? 1 : -1);
        Keyboard.Focus(Root);
    }

    private void UpdateAnnotationColorVisuals()
    {
        var activeRing = UiColors.Brush("#FF2A303B");

        var buttons = new (Button Button, string Hex)[]
        {
            (ColorRedButton, "#FFFF5A5A"),
            (ColorOrangeButton, "#FFFFA94D"),
            (ColorYellowButton, "#FFFFD166"),
            (ColorGreenButton, "#FF7CE38B"),
            (ColorBlueButton, "#FF6CA8FF"),
            (ColorPurpleButton, "#FFB08CFF"),
            (ColorDarkButton, "#FF242933"),
            (ColorWhiteButton, "#FFFFFFFF")
        };

        foreach (var (button, hex) in buttons)
        {
            var color = UiColors.Parse(hex);
            button.BorderBrush = color == _annotationColor ? activeRing : Brushes.Transparent;
        }
    }

    private void UpdateHistoryVisuals()
    {
        UndoButton.IsEnabled = _undoStack.Count > 0 || _liveStroke is not null;
        RedoButton.IsEnabled = _redoStack.Count > 0;
        ClearButton.IsEnabled = _strokes.Count > 0 || _liveStroke is not null;
    }

    private void PushHistory()
    {
        _undoStack.Add(new List<AnnotationStroke>(_strokes));

        if (_undoStack.Count > MaxHistory)
        {
            _undoStack.RemoveAt(0);
        }

        _redoStack.Clear();
        UpdateHistoryVisuals();
    }

    private void UndoAnnotation()
    {
        if (_liveStroke is not null)
        {
            CancelLiveStroke();
            return;
        }

        if (_undoStack.Count == 0)
        {
            return;
        }

        _redoStack.Add(new List<AnnotationStroke>(_strokes));
        var previous = _undoStack[_undoStack.Count - 1];
        _undoStack.RemoveAt(_undoStack.Count - 1);
        RestoreStrokes(previous);
    }

    private void RedoAnnotation()
    {
        if (_redoStack.Count == 0)
        {
            return;
        }

        _undoStack.Add(new List<AnnotationStroke>(_strokes));
        var next = _redoStack[_redoStack.Count - 1];
        _redoStack.RemoveAt(_redoStack.Count - 1);
        RestoreStrokes(next);
    }

    private void RestoreStrokes(List<AnnotationStroke> snapshot)
    {
        _strokes.Clear();
        _strokes.AddRange(snapshot);
        RefreshMosaicTiles();
        UpdateAnnotationScale();
        UpdateHistoryVisuals();
    }

    private void CancelLiveStroke()
    {
        _liveStroke = null;
        _drawingAnnotation = false;
        _eraseHistoryPushed = false;

        if (Mouse.Captured == Root)
        {
            Mouse.Capture(null);
        }

        UpdateAnnotationScale();
        UpdateHistoryVisuals();
    }

    private void ClearAnnotationStrokes()
    {
        if (_strokes.Count == 0 && _liveStroke is null)
        {
            return;
        }

        PushHistory();
        CancelTextEditor();
        CancelLiveStroke();
        _strokes.Clear();
        UpdateAnnotationScale();
        UpdateHistoryVisuals();
    }

    private void BeginTextEditor(Point displayPoint, Point imagePoint)
    {
        CancelTextEditor();

        var color = _annotationColor;
        var brush = new SolidColorBrush(color);
        brush.Freeze();

        var box = new TextBox
        {
            MinWidth = 80,
            MaxWidth = 320,
            FontSize = _textSize * AnnotationScale,
            FontWeight = _textBold ? FontWeights.Bold : FontWeights.Normal,
            FontStyle = _textItalic ? FontStyles.Italic : FontStyles.Normal,
            Padding = new Thickness(5, 3, 5, 3),
            Background = UiColors.Brush("#CC10141C"),
            Foreground = brush,
            BorderBrush = brush,
            BorderThickness = new Thickness(1),
            CaretBrush = brush,
            SelectionBrush = brush,
            FocusVisualStyle = null,
            AcceptsReturn = false
        };

        box.PreviewKeyDown += TextEditor_PreviewKeyDown;
        box.LostFocus += (_, _) => CommitTextEditor();

        Canvas.SetLeft(box, displayPoint.X);
        Canvas.SetTop(box, displayPoint.Y);
        EditorLayer.Children.Add(box);

        _textEditor = box;
        _textPoint = imagePoint;
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
            Text = text,
            TextSize = _textSize,
            Bold = _textBold,
            Italic = _textItalic
        };

        stroke.Points.Add(_textPoint);
        PushHistory();
        _strokes.Add(stroke);
        UpdateAnnotationScale();
        UpdateHistoryVisuals();
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

    private Point ToImagePoint(Point displayPoint)
    {
        var scale = AnnotationScale;
        return new Point(displayPoint.X / scale, displayPoint.Y / scale);
    }

    private static bool IsMeaningfulStroke(AnnotationStroke stroke)
    {
        if (stroke.Tool is AnnotationTool.Eraser)
        {
            return false;
        }

        if (stroke.Tool == AnnotationTool.Step)
        {
            return stroke.Points.Count > 0;
        }

        if (stroke.Tool == AnnotationTool.Mosaic)
        {
            if (stroke.Mosaic is null || stroke.Points.Count < 2)
            {
                return false;
            }

            var mosaicRect = AnnotationGeometry.NormalizeRect(stroke.Points[0], stroke.Points[1]);
            return mosaicRect.Width >= 4 && mosaicRect.Height >= 4;
        }

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

    private void CopyMenuItem_Click(object sender, RoutedEventArgs e) => CopyImage();    private void SaveMenuItem_Click(object sender, RoutedEventArgs e) => SaveImage();

    private void OutlineMenuItem_Click(object sender, RoutedEventArgs e)
    {
        var enabled = OutlineMenuItem.IsChecked;
        SetOutlineForAll(enabled);
        OutlineChanged?.Invoke(enabled);
    }

    private void ResetMenuItem_Click(object sender, RoutedEventArgs e) => ResetSizeAndOpacity();

    private void CloseMenuItem_Click(object sender, RoutedEventArgs e) => Close();

    private void CloseAllMenuItem_Click(object sender, RoutedEventArgs e) => CloseAllPins();
}
