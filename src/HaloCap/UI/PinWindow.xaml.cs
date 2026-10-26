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

    private static readonly List<PinWindow> OpenPins = new();

    private readonly BitmapSource _image;
    private readonly PhysicalRect _initialRect;
    private readonly double _monitorScale;
    private readonly ScreenshotImageFormat _format;
    private readonly double _imageDipWidth;
    private readonly double _imageDipHeight;
    private readonly List<AnnotationStroke> _strokes = new();
    private readonly DispatcherTimer _hudTimer;
    private IntPtr _handle;
    private double _zoom = 1.0;
    private double _opacity = 1.0;

    private bool _annotating;
    private AnnotationTool _annotationTool = AnnotationTool.Pen;
    private Color _annotationColor = AnnotationDefaults.Palette[0];
    private AnnotationStroke? _liveStroke;
    private bool _drawingAnnotation;
    private TextBox? _textEditor;
    private Point _textPoint;

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
        _imageDipWidth = image.PixelWidth * 96.0 / (image.DpiX > 0.1 ? image.DpiX : 96.0);
        _imageDipHeight = image.PixelHeight * 96.0 / (image.DpiY > 0.1 ? image.DpiY : 96.0);

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

        Root.SizeChanged += (_, _) => UpdateAnnotationScale();
        UpdateAnnotationToolVisuals();
        UpdateAnnotationColorVisuals();
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
        if (_annotating)
        {
            e.Handled = true;
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

        if (_annotating && e.Key == Key.Z && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            UndoAnnotation();
            return;
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
        return _strokes.Count > 0
            ? AnnotationRenderer.Compose(_image, _strokes)
            : _image;
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

        if (!enabled)
        {
            CancelTextEditor();
        }

        Root.Cursor = enabled ? Cursors.Cross : Cursors.SizeAll;
        Keyboard.Focus(Root);

        if (enabled)
        {
            ShowHud("标注模式 · 空格或完成退出");
        }
    }

    private void Root_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (!_annotating || e.ChangedButton != MouseButton.Left)
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

        if (_annotationTool == AnnotationTool.Text)
        {
            BeginTextEditor(display, imagePoint);
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

        _liveStroke.Points.Add(imagePoint);
        _drawingAnnotation = true;
        Mouse.Capture(Root);
        UpdateAnnotationScale();
    }

    private void Root_PreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (!_annotating)
        {
            return;
        }

        if (!_drawingAnnotation || _liveStroke is null)
        {
            return;
        }

        var point = ToImagePoint(e.GetPosition(Root));

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

        e.Handled = true;
        UpdateAnnotationScale();
    }

    private void Root_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (!_drawingAnnotation)
        {
            return;
        }

        e.Handled = true;
        _drawingAnnotation = false;
        Mouse.Capture(null);

        var stroke = _liveStroke;
        _liveStroke = null;

        if (stroke is not null && IsMeaningfulStroke(stroke))
        {
            _strokes.Add(stroke);
            ShowHud("已添加标注 · Ctrl+Z 撤销");
        }

        UpdateAnnotationScale();
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

    private void AnnotateMenuItem_Click(object sender, RoutedEventArgs e)
    {
        SetAnnotationMode(!_annotating);
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

        UpdateAnnotationScale();
    }

    private void ClearAnnotations()
    {
        CancelTextEditor();
        _strokes.Clear();
        _liveStroke = null;
        _drawingAnnotation = false;
        UpdateAnnotationScale();
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
            FontSize = AnnotationDefaults.TextSize * AnnotationScale,
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
            Text = text
        };

        stroke.Points.Add(_textPoint);
        _strokes.Add(stroke);
        UpdateAnnotationScale();
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

    private void ResetMenuItem_Click(object sender, RoutedEventArgs e) => ResetSizeAndOpacity();

    private void CloseMenuItem_Click(object sender, RoutedEventArgs e) => Close();

    private void CloseAllMenuItem_Click(object sender, RoutedEventArgs e) => CloseAllPins();
}
