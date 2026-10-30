using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.UI;

public partial class SettingsWindow : Window
{
    private enum HotkeySlot
    {
        Radial,
        Screenshot,
        Ocr,
        Record,
        Pin
    }

    private readonly Func<HotkeyDefinition, IReadOnlyDictionary<RadialAction, HotkeyDefinition>, string?> _applyHotkeys;
    private readonly Func<bool, string?> _applyAutoStart;
    private readonly Func<ScreenshotImageFormat, string?> _applyScreenshotFormat;
    private readonly Func<RadialMenuSize, string?> _applyMenuSize;
    private readonly Func<string, string?> _applyRecordingDirectory;
    private readonly Func<bool, string?> _applyConvertFilePathsToImages;
    private readonly Dictionary<HotkeySlot, HotkeyDefinition> _hotkeys = new();
    private readonly Dictionary<HotkeySlot, Button> _hotkeyButtons = new();
    private readonly Dictionary<HotkeySlot, TextBlock> _hotkeyValues = new();
    private HotkeySlot? _capturingSlot;
    private ScreenshotImageFormat _format;
    private RadialMenuSize _menuSize;
    private bool _autoStartEnabled;

    internal bool IsCapturingHotkey => _capturingSlot is not null;

    public SettingsWindow(
        HotkeyDefinition radialHotkey,
        IReadOnlyDictionary<RadialAction, HotkeyDefinition> actionHotkeys,
        bool autoStartEnabled,
        ScreenshotImageFormat screenshotFormat,
        RadialMenuSize menuSize,
        string recordingDirectory,
        bool convertFilePathsToImages,
        Func<HotkeyDefinition, IReadOnlyDictionary<RadialAction, HotkeyDefinition>, string?> applyHotkeys,
        Func<bool, string?> applyAutoStart,
        Func<ScreenshotImageFormat, string?> applyScreenshotFormat,
        Func<RadialMenuSize, string?> applyMenuSize,
        Func<string, string?> applyRecordingDirectory,
        Func<bool, string?> applyConvertFilePathsToImages)
    {
        InitializeComponent();

        _hotkeys[HotkeySlot.Radial] = radialHotkey.Clone();
        _hotkeys[HotkeySlot.Screenshot] = actionHotkeys[RadialAction.Screenshot].Clone();
        _hotkeys[HotkeySlot.Ocr] = actionHotkeys[RadialAction.Ocr].Clone();
        _hotkeys[HotkeySlot.Record] = actionHotkeys[RadialAction.Record].Clone();
        _hotkeys[HotkeySlot.Pin] = actionHotkeys[RadialAction.Pin].Clone();

        _hotkeyButtons[HotkeySlot.Radial] = RadialHotkeyButton;
        _hotkeyButtons[HotkeySlot.Screenshot] = ScreenshotHotkeyButton;
        _hotkeyButtons[HotkeySlot.Ocr] = OcrHotkeyButton;
        _hotkeyButtons[HotkeySlot.Record] = RecordHotkeyButton;
        _hotkeyButtons[HotkeySlot.Pin] = PinHotkeyButton;

        _hotkeyValues[HotkeySlot.Radial] = RadialHotkeyValueText;
        _hotkeyValues[HotkeySlot.Screenshot] = ScreenshotHotkeyValueText;
        _hotkeyValues[HotkeySlot.Ocr] = OcrHotkeyValueText;
        _hotkeyValues[HotkeySlot.Record] = RecordHotkeyValueText;
        _hotkeyValues[HotkeySlot.Pin] = PinHotkeyValueText;

        _applyHotkeys = applyHotkeys;
        _applyAutoStart = applyAutoStart;
        _applyScreenshotFormat = applyScreenshotFormat;
        _applyMenuSize = applyMenuSize;
        _applyRecordingDirectory = applyRecordingDirectory;
        _applyConvertFilePathsToImages = applyConvertFilePathsToImages;
        _autoStartEnabled = autoStartEnabled;
        _format = screenshotFormat;
        _menuSize = menuSize;

        VersionText.Text = AppInfo.VersionLabel;
        RefreshHotkeyFields();
        UpdateHotkeyHint();
        AutoStartToggle.IsChecked = autoStartEnabled;
        UpdateAutoStartUi();
        ConvertFilePathsToImagesToggle.IsChecked = convertFilePathsToImages;

        RecordingDirectoryBox.Text = recordingDirectory ?? string.Empty;
        RecordingDirectoryHint.Text = "留空时保存到 " + AppPaths.DefaultRecordingDirectory;
        PngRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Png;
        JpegRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Jpeg;
        SmallSizeRadio.IsChecked = menuSize == RadialMenuSize.Small;
        MediumSizeRadio.IsChecked = menuSize == RadialMenuSize.Medium;
        LargeSizeRadio.IsChecked = menuSize == RadialMenuSize.Large;

        AutoStartToggle.Checked += AutoStartToggle_Changed;
        AutoStartToggle.Unchecked += AutoStartToggle_Changed;
        PreviewKeyDown += OnPreviewKeyDown;
        Loaded += (_, _) => Activate();

        NavGeneral.IsChecked = true;
    }

    private static string HotkeySlotLabel(HotkeySlot slot) => slot switch
    {
        HotkeySlot.Radial => "呼出光圈",
        HotkeySlot.Screenshot => "截图",
        HotkeySlot.Ocr => "文字识别",
        HotkeySlot.Record => "录屏",
        HotkeySlot.Pin => "贴图",
        _ => "快捷键"
    };

    private void RefreshHotkeyFields()
    {
        foreach (var pair in _hotkeyValues)
        {
            pair.Value.Text = _hotkeys[pair.Key].DisplayText;
            pair.Value.Foreground = (Brush)FindResource("TextPrimaryBrush");
        }
    }

    private void RefreshHotkeyField(HotkeySlot slot)
    {
        _hotkeyValues[slot].Text = _hotkeys[slot].DisplayText;
        _hotkeyValues[slot].Foreground = (Brush)FindResource("TextPrimaryBrush");
    }

    private void NavItem_Checked(object sender, RoutedEventArgs e)
    {
        if (sender is not RadioButton { Uid: { } key })
        {
            return;
        }

        if (_capturingSlot is not null)
        {
            CancelHotkeyCapture();
        }

        GeneralPane.Visibility = key == "general" ? Visibility.Visible : Visibility.Collapsed;
        CapturePane.Visibility = key == "capture" ? Visibility.Visible : Visibility.Collapsed;
        RecordingPane.Visibility = key == "recording" ? Visibility.Visible : Visibility.Collapsed;
        AboutPane.Visibility = key == "about" ? Visibility.Visible : Visibility.Collapsed;
        SaveBar.Visibility = key == "about" ? Visibility.Collapsed : Visibility.Visible;
        SaveButton.IsDefault = key != "about";
        ClearError();
    }

    private void HotkeyButton_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement { Tag: string tag } ||
            !Enum.TryParse<HotkeySlot>(tag, out var slot))
        {
            return;
        }

        if (_capturingSlot == slot)
        {
            CancelHotkeyCapture();
            return;
        }

        if (_capturingSlot is not null)
        {
            CancelHotkeyCapture();
        }

        StartHotkeyCapture(slot);
    }

    internal void BeginHotkeyCapture() => StartHotkeyCapture(HotkeySlot.Radial);

    private void StartHotkeyCapture(HotkeySlot slot)
    {
        _capturingSlot = slot;
        var value = _hotkeyValues[slot];
        value.Text = "请按下快捷键…";
        value.Foreground = (Brush)FindResource("AccentBrush");
        HotkeyHint.Text = "等待按键…  按 Esc 取消";
        SaveButton.IsEnabled = false;
        ClearError();
        _hotkeyButtons[slot].Focus();
    }

    private void CompleteHotkeyCapture()
    {
        if (_capturingSlot is not { } slot)
        {
            return;
        }

        _capturingSlot = null;
        RefreshHotkeyField(slot);
        UpdateHotkeyHint();
        SaveButton.IsEnabled = true;
        SaveButton.Focus();
    }

    private void CancelHotkeyCapture()
    {
        if (_capturingSlot is not { } slot)
        {
            return;
        }

        _capturingSlot = null;
        RefreshHotkeyField(slot);
        UpdateHotkeyHint();
        SaveButton.IsEnabled = true;
        ClearError();
    }

    private void UpdateHotkeyHint()
    {
        var singleKeys = new List<string>();
        foreach (var pair in _hotkeys)
        {
            if (pair.Value.IsSingleKey)
            {
                singleKeys.Add(HotkeySlotLabel(pair.Key));
            }
        }

        HotkeyHint.Text = singleKeys.Count > 0
            ? $"{string.Join("、", singleKeys)} 使用单键，会独占该键并优先于输入法；点击对应按键可重新设置。"
            : "单击按键区域后按下新的组合键；单键会独占该键并优先于输入法，Esc 取消";
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (_capturingSlot is { } slot)
        {
            e.Handled = true;

            var key = e.Key == Key.System ? e.SystemKey : e.Key;
            if (key == Key.Escape)
            {
                CancelHotkeyCapture();
                return;
            }

            if (key == Key.None || HotkeyDefinition.IsModifierKey(key))
            {
                return;
            }

            var modifiers = Keyboard.Modifiers & HotkeyDefinition.AllowedModifiers;
            var candidate = new HotkeyDefinition(modifiers, key);
            if (!candidate.IsValid)
            {
                ShowError(modifiers == ModifierKeys.None
                    ? "该单键不适合用作全局快捷键，请改用 F1–F24、字母、数字，或按住 Ctrl / Alt / Shift 再按主键。"
                    : "无法使用该组合键，请换一个。");
                return;
            }

            foreach (var pair in _hotkeys)
            {
                if (pair.Key != slot &&
                    pair.Value.Modifiers == candidate.Modifiers &&
                    pair.Value.Key == candidate.Key)
                {
                    ShowError($"该快捷键已用于「{HotkeySlotLabel(pair.Key)}」，请换一个。");
                    return;
                }
            }

            _hotkeys[slot] = candidate;
            CompleteHotkeyCapture();
            ClearError();
            return;
        }

        if (e.Key == Key.Escape)
        {
            e.Handled = true;
            Close();
        }
    }

    private void AutoStartToggle_Changed(object sender, RoutedEventArgs e)
    {
        _autoStartEnabled = AutoStartToggle.IsChecked == true;
        UpdateAutoStartUi();
        ClearError();
    }

    private void UpdateAutoStartUi()
    {
        AutoStartToggle.ToolTip = _autoStartEnabled ? "关闭开机自启" : "开启开机自启";
    }

    private void FormatRadio_Checked(object sender, RoutedEventArgs e)
    {
        _format = JpegRadio.IsChecked == true
            ? ScreenshotImageFormat.Jpeg
            : ScreenshotImageFormat.Png;
        ClearError();
    }

    private void MenuSizeRadio_Checked(object sender, RoutedEventArgs e)
    {
        _menuSize = SmallSizeRadio.IsChecked == true
            ? RadialMenuSize.Small
            : LargeSizeRadio.IsChecked == true
                ? RadialMenuSize.Large
                : RadialMenuSize.Medium;
        ClearError();
    }

    private void RecordingDirectoryBox_TextChanged(object sender, TextChangedEventArgs e)
    {
        ClearError();
    }

    private void BrowseButton_Click(object sender, RoutedEventArgs e)
    {
        var initial = RecordingDirectoryBox.Text.Trim();

        if (initial.Length == 0)
        {
            initial = AppPaths.DefaultRecordingDirectory;
        }

        try
        {
            initial = Environment.ExpandEnvironmentVariables(initial);
        }
        catch
        {
            // 使用下方回退目录。
        }

        if (!Directory.Exists(initial))
        {
            initial = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        }

        if (!Directory.Exists(initial))
        {
            initial = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
        }

        using var dialog = new System.Windows.Forms.FolderBrowserDialog
        {
            Description = "选择录屏保存文件夹",
            ShowNewFolderButton = true,
            SelectedPath = initial
        };

        if (dialog.ShowDialog() == System.Windows.Forms.DialogResult.OK)
        {
            RecordingDirectoryBox.Text = dialog.SelectedPath;
        }
    }

    private void SaveButton_Click(object sender, RoutedEventArgs e)
    {
        if (_capturingSlot is not null)
        {
            return;
        }

        ClearError();
        SaveButton.IsEnabled = false;

        try
        {
            var actionHotkeys = new Dictionary<RadialAction, HotkeyDefinition>
            {
                [RadialAction.Screenshot] = _hotkeys[HotkeySlot.Screenshot].Clone(),
                [RadialAction.Ocr] = _hotkeys[HotkeySlot.Ocr].Clone(),
                [RadialAction.Record] = _hotkeys[HotkeySlot.Record].Clone(),
                [RadialAction.Pin] = _hotkeys[HotkeySlot.Pin].Clone()
            };

            var hotkeyError = _applyHotkeys(_hotkeys[HotkeySlot.Radial].Clone(), actionHotkeys);
            if (!string.IsNullOrEmpty(hotkeyError))
            {
                ShowError(hotkeyError);
                return;
            }

            var autoStartError = _applyAutoStart(_autoStartEnabled);
            if (!string.IsNullOrEmpty(autoStartError))
            {
                ShowError("快捷键已更新；" + autoStartError + " 请修正后重试。");
                return;
            }

            var formatError = _applyScreenshotFormat(_format);
            if (!string.IsNullOrEmpty(formatError))
            {
                ShowError("快捷键与自启设置已更新；" + formatError + " 请修正后重试。");
                return;
            }

            var sizeError = _applyMenuSize(_menuSize);
            if (!string.IsNullOrEmpty(sizeError))
            {
                ShowError("其他设置已更新；" + sizeError + " 请修正后重试。");
                return;
            }

            var directoryError = _applyRecordingDirectory(RecordingDirectoryBox.Text.Trim());
            if (!string.IsNullOrEmpty(directoryError))
            {
                ShowError("其他设置已更新；" + directoryError + " 请修正后重试。");
                return;
            }

            var convertFilePathsToImagesError = _applyConvertFilePathsToImages(ConvertFilePathsToImagesToggle.IsChecked == true);
            if (!string.IsNullOrEmpty(convertFilePathsToImagesError))
            {
                ShowError("其他设置已更新；" + convertFilePathsToImagesError + " 请修正后重试。");
                return;
            }

            Close();
        }
        catch (Exception ex)
        {
            ShowError("保存设置失败：" + ex.Message);
        }
        finally
        {
            SaveButton.IsEnabled = true;
        }
    }

    private void ShowError(string message)
    {
        ErrorText.Text = message;
        ErrorBanner.Visibility = Visibility.Visible;
        FooterHint.Visibility = Visibility.Collapsed;
    }

    private void ClearError()
    {
        ErrorText.Text = string.Empty;
        ErrorBanner.Visibility = Visibility.Collapsed;
        FooterHint.Visibility = Visibility.Visible;
    }

    private void CancelButton_Click(object sender, RoutedEventArgs e) => Close();

    private void CloseButton_Click(object sender, RoutedEventArgs e) => Close();

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
}
