using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
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
    private const int MaxLineViews = 500;

    private static readonly List<OcrResultWindow> OpenWindows = new();

    private readonly BitmapSource _image;
    private readonly PhysicalRect _anchor;
    private readonly ScreenMonitor _monitor;
    private readonly OcrService _ocrService;
    private readonly OcrSettings _settings;
    private readonly OcrImageSource _source;
    private readonly DispatcherTimer _statusTimer;
    private OcrOutcome? _outcome;
    private string _text = string.Empty;
    private IReadOnlyList<string> _lines = Array.Empty<string>();
    private OcrEngineKind _engine;
    private bool _lineView;
    private bool _lineViewsDirty;
    private bool _loaded;
    private bool _closed;
    private bool _suppressEngineChange;
    private int _recognitionVersion;

    internal OcrResultWindow(
        BitmapSource image,
        PhysicalRect anchor,
        ScreenMonitor monitor,
        OcrService ocrService,
        OcrSettings settings,
        OcrImageSource source)
    {
        InitializeComponent();

        _image = image;
        _anchor = anchor;
        _monitor = monitor;
        _ocrService = ocrService;
        _settings = settings.Clone();
        _settings.Normalize();
        _source = source;
        _engine = _settings.Engine;

        _statusTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1800) };
        _statusTimer.Tick += (_, _) =>
        {
            _statusTimer.Stop();
            ResetStatus();
        };

        ThumbnailImage.Source = _image;
        SubtitleText.Text = source switch
        {
            OcrImageSource.Pin => "贴图原图 · 按行识别 · 不包含标注",
            _ => "截图选区 · 按行识别"
        };

        _suppressEngineChange = true;
        NativeEngineRadio.IsChecked = _engine == OcrEngineKind.Native;
        ModelEngineRadio.IsChecked = _engine == OcrEngineKind.Model;
        _suppressEngineChange = false;

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
        _loaded = true;
        PlaceNearAnchor();

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

        await RunRecognitionAsync();
    }

    private async Task RunRecognitionAsync()
    {
        var version = ++_recognitionVersion;

        if (SearchBar.Visibility == Visibility.Visible)
        {
            CloseSearch(focusRoot: false);
        }

        ShowLoading();
        var outcome = await _ocrService.RecognizeAsync(_image, _settings, _engine);

        if (_closed || version != _recognitionVersion)
        {
            return;
        }

        ApplyOutcome(outcome);
        UpdateLayout();

        _ = Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (!_closed && version == _recognitionVersion)
                {
                    PlaceNearAnchor();
                }
            }));
    }

    private void ShowLoading()
    {
        _outcome = null;
        _text = string.Empty;
        _lines = Array.Empty<string>();
        _lineViewsDirty = true;
        LoadingPanel.Visibility = Visibility.Visible;
        TextCard.Visibility = Visibility.Collapsed;
        LinesCard.Visibility = Visibility.Collapsed;
        EmptyState.Visibility = Visibility.Collapsed;
        EmptyRetryButton.Visibility = Visibility.Collapsed;
        WarningBorder.Visibility = Visibility.Collapsed;
        EngineChip.Visibility = Visibility.Collapsed;
        CopyAllButton.IsEnabled = false;
        RetryButton.IsEnabled = false;
        SearchButton.IsEnabled = false;
        ViewToggleButton.IsEnabled = false;
        CountText.Text = "识别中…";
        LoadingHintText.Text = _engine == OcrEngineKind.Model
            ? "PP-OCRv6 tiny · 首次使用会准备本地模型"
            : "Windows 原生 · 系统离线引擎";
        ResetStatus();
    }

    private void ApplyOutcome(OcrOutcome outcome)
    {
        _outcome = outcome;
        _engine = outcome.Engine;
        LoadingPanel.Visibility = Visibility.Collapsed;

        var unavailable = outcome.Status is OcrStatus.NoLanguage or OcrStatus.Failed;
        EngineChip.Visibility = Visibility.Visible;
        EngineText.Text = unavailable
            ? "识别不可用"
            : string.IsNullOrWhiteSpace(outcome.EngineLabel)
                ? "Windows 原生"
                : outcome.EngineLabel;
        EngineChip.Background = (Brush)FindResource(
            unavailable ? "OcrWarningFillBrush" : "OcrChipFillBrush");
        EngineChip.BorderBrush = (Brush)FindResource(
            unavailable ? "OcrWarningBorderBrush" : "OcrChipBorderBrush");
        EngineText.Foreground = (Brush)FindResource(
            unavailable ? "OcrWarningTextBrush" : "OcrChipTextBrush");

        var showWarning = outcome.Status is OcrStatus.Success or OcrStatus.Empty &&
                          !string.IsNullOrWhiteSpace(outcome.Message);

        if (showWarning)
        {
            WarningBorder.Visibility = Visibility.Visible;
            WarningText.Text = outcome.Message;
        }
        else
        {
            WarningBorder.Visibility = Visibility.Collapsed;
        }

        _lineViewsDirty = true;

        if (outcome.Status == OcrStatus.Success)
        {
            _text = outcome.Text;
            _lines = outcome.Lines;
            ResultTextBox.Text = _text;
            var characters = _text.Count(character => !char.IsWhiteSpace(character));
            CountText.Text = $"{_lines.Count} 行 · {characters} 字 · {outcome.ElapsedMilliseconds} ms";
            EmptyState.Visibility = Visibility.Collapsed;
            EmptyRetryButton.Visibility = Visibility.Collapsed;
            CopyAllButton.IsEnabled = _text.Length > 0;
            RetryButton.IsEnabled = true;
            SearchButton.IsEnabled = _text.Length > 0;
            ViewToggleButton.IsEnabled = _text.Length > 0;
            UpdateViewUi();
            ResetStatus();

            if (_settings.AutoCopy && _text.Length > 0)
            {
                TryCopyText(_text, "已自动复制全部文字");
            }

            return;
        }

        _text = string.Empty;
        _lines = Array.Empty<string>();
        ResultTextBox.Text = string.Empty;
        CountText.Text = outcome.Status switch
        {
            OcrStatus.Empty => "未识别到文字",
            OcrStatus.NoLanguage => "识别语言不可用",
            _ => "识别失败"
        };
        CopyAllButton.IsEnabled = false;
        RetryButton.IsEnabled = true;
        SearchButton.IsEnabled = false;
        ViewToggleButton.IsEnabled = false;
        TextCard.Visibility = Visibility.Collapsed;
        LinesCard.Visibility = Visibility.Collapsed;
        EmptyState.Visibility = Visibility.Visible;
        EmptyRetryButton.Visibility = Visibility.Visible;
        EmptyRetryButton.Content = _engine == OcrEngineKind.Native
            ? "用模型识别重试"
            : "用原生识别重试";

        var warningState = outcome.Status is OcrStatus.NoLanguage or OcrStatus.Failed;
        EmptyStateGlyph.Text = warningState ? "!" : "文";
        EmptyStateIcon.Background = (Brush)FindResource(
            warningState ? "OcrWarningFillBrush" : "OcrChipFillBrush");
        EmptyStateIcon.BorderBrush = (Brush)FindResource(
            warningState ? "OcrWarningBorderBrush" : "OcrChipBorderBrush");
        EmptyStateGlyph.Foreground = (Brush)FindResource(
            warningState ? "OcrWarningTextBrush" : "OcrChipTextBrush");

        EmptyText.Text = outcome.Status switch
        {
            OcrStatus.Empty => outcome.Engine == OcrEngineKind.Model
                ? "没有识别到文字。可以切回「原生」再试一次，或选择更清晰、对比度更高的区域。"
                : "没有识别到文字。可以切换到「模型」再试一次，或选择更清晰、对比度更高的区域。",
            OcrStatus.NoLanguage => string.IsNullOrWhiteSpace(outcome.Message)
                ? OcrService.ChineseLanguageGuidance
                : outcome.Message,
            _ => "这次没有识别成功。" + Environment.NewLine + Environment.NewLine +
                 (string.IsNullOrWhiteSpace(outcome.Message)
                     ? OcrService.GeneralGuidance
                     : outcome.Message)
        };

        ResetStatus();
    }

    private void UpdateViewUi()
    {
        var success = _outcome?.Status == OcrStatus.Success;

        if (!success)
        {
            TextCard.Visibility = Visibility.Collapsed;
            LinesCard.Visibility = Visibility.Collapsed;
            return;
        }

        if (_lineView && _lineViewsDirty)
        {
            BuildLineViews(_lines);
            _lineViewsDirty = false;
        }

        TextCard.Visibility = _lineView ? Visibility.Collapsed : Visibility.Visible;
        LinesCard.Visibility = _lineView ? Visibility.Visible : Visibility.Collapsed;
        ViewToggleButton.Content = _lineView ? "文本" : "逐行";
        ViewToggleButton.ToolTip = _lineView ? "切换到可选中文本" : "切换到逐行复制";
    }

    private void BuildLineViews(IReadOnlyList<string> lines)
    {
        LinesPanel.Children.Clear();

        var textBrush = new SolidColorBrush(Color.FromRgb(0x1D, 0x1D, 0x1F));
        var numberBrush = new SolidColorBrush(Color.FromRgb(0xA8, 0xA8, 0xAD));
        var iconBrush = new SolidColorBrush(Color.FromRgb(0xB0, 0xB0, 0xB5));
        var hintBrush = new SolidColorBrush(Color.FromRgb(0x8E, 0x8E, 0x93));
        textBrush.Freeze();
        numberBrush.Freeze();
        iconBrush.Freeze();
        hintBrush.Freeze();

        var iconGeometry = Geometry.Parse(
            "M8 8h9a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H8a1 1 0 0 1-1-1V9a1 1 0 0 1 1-1Z M5 16H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h11a1 1 0 0 1 1 1v1");
        iconGeometry.Freeze();

        var count = Math.Min(lines.Count, MaxLineViews);

        for (var index = 0; index < count; index++)
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
                Margin = new Thickness(2, 3, 0, 0),
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

        if (lines.Count > count)
        {
            LinesPanel.Children.Add(new TextBlock
            {
                Text = $"还有 {lines.Count - count} 行，切换回「文本」视图查看和复制。",
                FontSize = 10.5,
                Foreground = hintBrush,
                Margin = new Thickness(10, 8, 10, 10)
            });
        }
    }

    private void LineButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not Button { Tag: string text })
        {
            return;
        }

        TryCopyText(text, "已复制这一行");
    }

    private void CopyAllButton_Click(object sender, RoutedEventArgs e)
    {
        if (_text.Length == 0)
        {
            return;
        }

        TryCopyText(_text, "已复制全部文字");
    }

    private void RetryButton_Click(object sender, RoutedEventArgs e)
    {
        if (_outcome is null)
        {
            return;
        }

        _ = RunRecognitionAsync();
    }

    private void EmptyRetryButton_Click(object sender, RoutedEventArgs e)
    {
        if (_engine == OcrEngineKind.Native)
        {
            ModelEngineRadio.IsChecked = true;
        }
        else
        {
            NativeEngineRadio.IsChecked = true;
        }
    }

    private bool TryCopyText(string text, string successMessage)
    {
        try
        {
            ImageClipboard.SetText(text);
            ShowStatus(successMessage);
            return true;
        }
        catch (Exception ex)
        {
            Log.Error("复制 OCR 文本失败", ex);
            ShowStatus("复制失败，剪贴板可能被占用", isError: true);
            return false;
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
        StatusText.Text = _outcome?.Status == OcrStatus.Success
            ? "Esc 关闭 · 可选择文本 · Ctrl+F 搜索"
            : "Esc 关闭";
        StatusText.Foreground = (Brush)FindResource("OcrTextTertiaryBrush");
    }

    private void ModeRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (_suppressEngineChange ||
            sender is not RadioButton { Tag: string tag } ||
            !Enum.TryParse<OcrEngineKind>(tag, out var engine) ||
            engine == _engine)
        {
            return;
        }

        _engine = engine;

        if (_loaded)
        {
            _ = RunRecognitionAsync();
        }
    }

    private void ViewToggleButton_Click(object sender, RoutedEventArgs e)
    {
        if (_outcome?.Status != OcrStatus.Success)
        {
            return;
        }

        if (!_lineView && SearchBar.Visibility == Visibility.Visible)
        {
            CloseSearch(focusRoot: false);
        }

        SetLineView(!_lineView);
    }

    private void SetLineView(bool lineView)
    {
        if (_lineView == lineView)
        {
            return;
        }

        _lineView = lineView;
        UpdateViewUi();
        UpdateLayout();
        PlaceNearAnchor();
    }

    private void ThumbnailButton_Changed(object sender, RoutedEventArgs e)
    {
        ThumbnailCard.Visibility = ThumbnailButton.IsChecked == true
            ? Visibility.Visible
            : Visibility.Collapsed;

        if (!_loaded || _closed)
        {
            return;
        }

        UpdateLayout();
        PlaceNearAnchor();
    }

    private void SearchButton_Click(object sender, RoutedEventArgs e)
    {
        if (SearchBar.Visibility == Visibility.Visible)
        {
            CloseSearch(focusRoot: true);
        }
        else
        {
            OpenSearch();
        }
    }

    private void OpenSearch()
    {
        if (_outcome?.Status != OcrStatus.Success || _text.Length == 0)
        {
            return;
        }

        if (_lineView)
        {
            SetLineView(false);
        }

        SearchBar.Visibility = Visibility.Visible;
        UpdateLayout();

        if (_loaded)
        {
            PlaceNearAnchor();
        }

        _ = Dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() =>
            {
                if (!_closed)
                {
                    SearchBox.Focus();
                    SearchBox.SelectAll();
                }
            }));

        UpdateSearch(selectFirst: true);
    }

    private void CloseSearch(bool focusRoot)
    {
        SearchBar.Visibility = Visibility.Collapsed;
        SearchBox.Text = string.Empty;
        SearchStatusText.Text = string.Empty;
        SearchPlaceholder.Visibility = Visibility.Visible;

        if (focusRoot || ResultTextBox.IsKeyboardFocusWithin || SearchBox.IsKeyboardFocusWithin)
        {
            Keyboard.Focus(Root);
        }

        if (_loaded)
        {
            UpdateLayout();
            PlaceNearAnchor();
        }
    }

    private void CloseSearchButton_Click(object sender, RoutedEventArgs e) => CloseSearch(focusRoot: true);

    private void SearchBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        SearchPlaceholder.Visibility = string.IsNullOrEmpty(SearchBox.Text)
            ? Visibility.Visible
            : Visibility.Collapsed;

        if (SearchBar.Visibility == Visibility.Visible)
        {
            UpdateSearch(selectFirst: true);
        }
    }

    private void SearchBox_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            NavigateSearch(!Keyboard.Modifiers.HasFlag(ModifierKeys.Shift));
        }
    }

    private void SearchPreviousButton_Click(object sender, RoutedEventArgs e) => NavigateSearch(false);

    private void SearchNextButton_Click(object sender, RoutedEventArgs e) => NavigateSearch(true);

    private void UpdateSearch(bool selectFirst)
    {
        var query = SearchBox.Text;

        if (string.IsNullOrEmpty(query) || _text.Length == 0)
        {
            SearchStatusText.Text = string.Empty;
            ResultTextBox.Select(0, 0);
            return;
        }

        var total = CountMatches(_text, query);

        if (total == 0)
        {
            SearchStatusText.Text = "无匹配";
            ResultTextBox.Select(0, 0);
            return;
        }

        if (selectFirst)
        {
            var index = _text.IndexOf(query, StringComparison.CurrentCultureIgnoreCase);

            if (index >= 0)
            {
                SelectSearchMatch(index, query.Length, total);
            }
        }
    }

    private void NavigateSearch(bool forward)
    {
        var query = SearchBox.Text;

        if (string.IsNullOrEmpty(query) || _text.Length == 0)
        {
            return;
        }

        var total = CountMatches(_text, query);

        if (total == 0)
        {
            SearchStatusText.Text = "无匹配";
            return;
        }

        var start = ResultTextBox.SelectionStart;
        var length = ResultTextBox.SelectionLength;
        var index = -1;

        if (forward)
        {
            var from = Math.Min(_text.Length, start + Math.Max(1, length));
            index = _text.IndexOf(query, from, StringComparison.CurrentCultureIgnoreCase);

            if (index < 0)
            {
                index = _text.IndexOf(query, 0, StringComparison.CurrentCultureIgnoreCase);
            }
        }
        else
        {
            var from = start - 1;
            index = from >= 0
                ? _text.LastIndexOf(query, from, StringComparison.CurrentCultureIgnoreCase)
                : -1;

            if (index < 0)
            {
                index = _text.LastIndexOf(query, StringComparison.CurrentCultureIgnoreCase);
            }
        }

        if (index >= 0)
        {
            SelectSearchMatch(index, query.Length, total);
        }
    }

    private void SelectSearchMatch(int index, int length, int total)
    {
        ResultTextBox.Select(index, length);
        var line = ResultTextBox.GetLineIndexFromCharacterIndex(index);

        if (line >= 0)
        {
            ResultTextBox.ScrollToLine(line);
        }

        var ordinal = CountMatches(_text[..index], SearchBox.Text) + 1;
        SearchStatusText.Text = $"{ordinal}/{total}";
    }

    private static int CountMatches(string text, string query)
    {
        if (query.Length == 0)
        {
            return 0;
        }

        var count = 0;
        var index = 0;

        while (index < text.Length &&
               (index = text.IndexOf(query, index, StringComparison.CurrentCultureIgnoreCase)) >= 0)
        {
            count++;
            index += Math.Max(1, query.Length);
        }

        return count;
    }

    private void PlaceNearAnchor()
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
        var left = _anchor.Left;
        var top = _anchor.Bottom + margin;

        if (top + windowRect.Height > area.Bottom - margin)
        {
            top = _anchor.Top - windowRect.Height - margin;
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

            if (SearchBar.Visibility == Visibility.Visible)
            {
                CloseSearch(focusRoot: true);
            }
            else
            {
                Close();
            }

            return;
        }

        if (e.Key == Key.F && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;

            if (SearchBar.Visibility == Visibility.Visible)
            {
                SearchBox.Focus();
                SearchBox.SelectAll();
            }
            else
            {
                OpenSearch();
            }

            return;
        }

        if (e.Key == Key.F3)
        {
            e.Handled = true;

            if (SearchBar.Visibility == Visibility.Visible)
            {
                NavigateSearch(!Keyboard.Modifiers.HasFlag(ModifierKeys.Shift));
            }
            else
            {
                OpenSearch();
            }

            return;
        }

        if (e.Key == Key.C &&
            Keyboard.Modifiers.HasFlag(ModifierKeys.Control) &&
            Keyboard.Modifiers.HasFlag(ModifierKeys.Shift))
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
}
