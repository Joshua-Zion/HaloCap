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
using System.Windows.Shapes;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.UI;

public partial class OcrResultWindow : Window
{
    private static readonly List<OcrResultWindow> OpenWindows = new();

    private readonly BitmapSource _image;
    private readonly PhysicalRect _selection;
    private readonly ScreenMonitor _monitor;
    private readonly DispatcherTimer _statusTimer;
    private IReadOnlyList<string> _lines = Array.Empty<string>();
    private bool _closed;

    internal OcrResultWindow(BitmapSource image, PhysicalRect selection, ScreenMonitor monitor)
    {
        InitializeComponent();

        _image = image;
        _selection = selection;
        _monitor = monitor;

        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1600) };
        _statusTimer.Tick += (_, _) =>
        {
            _statusTimer.Stop();
            ResetStatus();
        };

        OpenWindows.Add(this);
        Closed += (_, _) =>
        {
            _closed = true;
            _statusTimer.Stop();
            OpenWindows.Remove(this);
        };

        Loaded += OnLoaded;
    }

    internal static void CloseAllWindows()
    {
        foreach (var window in OpenWindows.ToArray())
        {
            try
            {
                window.Close();
            }
            catch (Exception ex)
            {
                Log.Error("关闭 OCR 结果窗口失败", ex);
            }
        }
    }

    private async void OnLoaded(object sender, RoutedEventArgs e)
    {
        PlaceNearSelection();

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });

        try
        {
            Activate();
            MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口可能在显示过程中被关闭。
        }

        var outcome = await OcrService.RecognizeAsync(_image);

        if (_closed)
        {
            return;
        }

        LoadingPanel.Visibility = Visibility.Collapsed;
        ApplyOutcome(outcome);
        UpdateLayout();

        _ = Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (!_closed)
                {
                    PlaceNearSelection();
                }
            }));
    }

    private void ApplyOutcome(OcrOutcome outcome)
    {
        var unavailable = outcome.Status is OcrStatus.NoLanguage or OcrStatus.Failed;
        LanguageChip.Visibility = Visibility.Visible;
        LanguageText.Text = unavailable ? "识别不可用" : DescribeLanguage(outcome.LanguageTag);
        LanguageChip.Background = (Brush)FindResource(unavailable ? "OcrWarningFillBrush" : "OcrChipFillBrush");
        LanguageChip.BorderBrush = (Brush)FindResource(unavailable ? "OcrWarningBorderBrush" : "OcrChipBorderBrush");
        LanguageText.Foreground = (Brush)FindResource(unavailable ? "OcrWarningTextBrush" : "OcrChipTextBrush");
        var chineseAvailable = outcome.ChineseAvailable;

        var showBanner =
            !outcome.ChineseAvailable &&
            outcome.Status is OcrStatus.Success or OcrStatus.Empty &&
            !string.IsNullOrWhiteSpace(outcome.Message);

        if (showBanner)
        {
            WarningBorder.Visibility = Visibility.Visible;
            WarningText.Text = outcome.Message;
        }
        else
        {
            WarningBorder.Visibility = Visibility.Collapsed;
        }

        if (outcome.Status == OcrStatus.Success)
        {
            _lines = outcome.Lines;
            BuildLineViews(_lines);
            CountText.Text = $"共 {_lines.Count} 行 · 可逐行复制";
            CopyAllButton.IsEnabled = true;
            LinesCard.Visibility = Visibility.Visible;
            EmptyText.Visibility = Visibility.Collapsed;
            ResetStatus();
            return;
        }

        _lines = Array.Empty<string>();
        CountText.Text = chineseAvailable ? "未识别到文字" : "识别语言组件不可用";
        CopyAllButton.IsEnabled = false;
        LinesCard.Visibility = Visibility.Collapsed;

        EmptyText.Text = outcome.Status switch
        {
            OcrStatus.Empty => "没有识别到文字。试试选择更清晰、对比度更高的区域。",
            OcrStatus.NoLanguage => "离线文字识别不可用。\n\n" +
                                    (string.IsNullOrWhiteSpace(outcome.Message)
                                        ? OcrService.ChineseLanguageGuidance
                                        : outcome.Message),
            _ => "这次没有识别成功。\n\n" +
                 (string.IsNullOrWhiteSpace(outcome.Message)
                     ? OcrService.ChineseLanguageGuidance
                     : outcome.Message)
        };
        EmptyText.Visibility = Visibility.Visible;
        ResetStatus();
    }

    private void BuildLineViews(IReadOnlyList<string> lines)
    {
        LinesPanel.Children.Clear();

        var textBrush = new SolidColorBrush(Color.FromRgb(0x1D, 0x1D, 0x1F));
        var numberBrush = new SolidColorBrush(Color.FromRgb(0xA8, 0xA8, 0xAD));
        var iconBrush = new SolidColorBrush(Color.FromRgb(0xB0, 0xB0, 0xB5));
        textBrush.Freeze();
        numberBrush.Freeze();
        iconBrush.Freeze();

        var iconGeometry = Geometry.Parse(
            "M8 8h9a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H8a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1Z M5 16H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h11a1 1 0 0 1 1 1v1");
        iconGeometry.Freeze();

        for (var index = 0; index < lines.Count; index++)
        {
            var line = lines[index];
            var button = new Button
            {
                Style = (Style)FindResource("LineButtonStyle"),
                Tag = line,
                ToolTip = "点击复制这一行"
            };

            var grid = new Grid();
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(26) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });

            var number = new TextBlock
            {
                Text = (index + 1).ToString("00"),
                FontSize = 9.5,
                FontFamily = new FontFamily("Consolas, Microsoft YaHei UI"),
                Foreground = numberBrush,
                Margin = new Thickness(2, 4, 0, 0),
                VerticalAlignment = VerticalAlignment.Top
            };

            var text = new TextBlock
            {
                Text = line,
                FontSize = 12.5,
                Foreground = textBrush,
                TextWrapping = TextWrapping.Wrap,
                LineHeight = 19
            };

            var iconPath = new Path
            {
                Data = iconGeometry,
                Stroke = iconBrush,
                StrokeThickness = 1.2,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                StrokeLineJoin = PenLineJoin.Round,
                Fill = Brushes.Transparent
            };

            var icon = new Viewbox
            {
                Width = 13,
                Height = 13,
                Child = iconPath,
                Margin = new Thickness(12, 3, 2, 0),
                VerticalAlignment = VerticalAlignment.Top,
                Opacity = 0.75
            };

            Grid.SetColumn(number, 0);
            Grid.SetColumn(text, 1);
            Grid.SetColumn(icon, 2);
            grid.Children.Add(number);
            grid.Children.Add(text);
            grid.Children.Add(icon);

            button.Content = grid;
            button.Click += LineButton_Click;
            LinesPanel.Children.Add(button);
        }
    }

    private void LineButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string text })
        {
            return;
        }

        try
        {
            ImageClipboard.SetText(text);
            ShowStatus("已复制这一行");
        }
        catch (Exception ex)
        {
            Log.Error("复制 OCR 行失败", ex);
            ShowStatus("复制失败，剪贴板可能被占用", isError: true);
        }
    }

    private void CopyAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (_lines.Count == 0)
        {
            return;
        }

        try
        {
            ImageClipboard.SetText(string.Join(Environment.NewLine, _lines));
            ShowStatus("已复制全部文字");
        }
        catch (Exception ex)
        {
            Log.Error("复制 OCR 全文失败", ex);
            ShowStatus("复制失败，剪贴板可能被占用", isError: true);
        }
    }

    private void ShowStatus(string message, bool isError = false)
    {
        StatusText.Text = message;
        StatusText.Foreground = (Brush)FindResource(isError ? "OcrDangerBrush" : "OcrAccentBrush");
        _statusTimer.Stop();
        _statusTimer.Start();
    }

    private void ResetStatus()
    {
        StatusText.Text = _lines.Count > 0
            ? "Esc 关闭 · 单击任意一行复制"
            : "Esc 关闭";
        StatusText.Foreground = (Brush)FindResource("OcrTextTertiaryBrush");
    }

    private void PlaceNearSelection()
    {
        if (_closed)
        {
            return;
        }

        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero || !NativeMethods.GetWindowRect(handle, out var windowRect))
        {
            return;
        }

        var area = _monitor.WorkArea;
        const int margin = 12;
        var left = _selection.Left;
        var top = _selection.Bottom + margin;

        if (top + windowRect.Height > area.Bottom - margin)
        {
            top = _selection.Top - windowRect.Height - margin;
        }

        if (top + windowRect.Height > area.Bottom - margin)
        {
            top = area.Bottom - windowRect.Height - margin;
        }

        if (top < area.Top + margin)
        {
            top = area.Top + margin;
        }

        if (left + windowRect.Width > area.Right - margin)
        {
            left = area.Right - windowRect.Width - margin;
        }

        if (left < area.Left + margin)
        {
            left = area.Left + margin;
        }

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            left,
            top,
            windowRect.Width,
            windowRect.Height,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private void Window_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Escape)
        {
            e.Handled = true;
            Close();
            return;
        }

        if (e.Key == Key.C && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            CopyAllButton_Click(this, new RoutedEventArgs());
        }
    }

    private void TitleBar_MouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (e.ChangedButton != MouseButton.Left || e.ButtonState != MouseButtonState.Pressed)
        {
            return;
        }

        try
        {
            DragMove();
        }
        catch (InvalidOperationException)
        {
            // 鼠标在拖动开始前已经释放。
        }
    }

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

    private static string DescribeLanguage(string languageTag)
    {
        if (string.IsNullOrWhiteSpace(languageTag))
        {
            return "自动选择";
        }

        if (languageTag.StartsWith("zh-Hans", StringComparison.OrdinalIgnoreCase) ||
            string.Equals(languageTag, "zh-CN", StringComparison.OrdinalIgnoreCase))
        {
            return "中文（简体）";
        }

        if (languageTag.StartsWith("zh", StringComparison.OrdinalIgnoreCase))
        {
            return "中文（" + languageTag + "）";
        }

        if (languageTag.StartsWith("en", StringComparison.OrdinalIgnoreCase))
        {
            return "English";
        }

        return languageTag;
    }
}
