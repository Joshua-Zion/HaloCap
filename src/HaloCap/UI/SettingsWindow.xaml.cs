using System;
using System.IO;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.UI;

public partial class SettingsWindow : Window
{
    private readonly Func<HotkeyDefinition, string?> _applyHotkey;
    private readonly Func<bool, string?> _applyAutoStart;
    private readonly Func<ScreenshotImageFormat, string?> _applyScreenshotFormat;
    private readonly Func<RadialMenuSize, string?> _applyMenuSize;
    private readonly Func<string, string?> _applyRecordingDirectory;
    private HotkeyDefinition _hotkey;
    private ScreenshotImageFormat _format;
    private RadialMenuSize _menuSize;
    private bool _autoStartEnabled;
    private bool _capturingHotkey;

    public SettingsWindow(
        HotkeyDefinition hotkey,
        bool autoStartEnabled,
        ScreenshotImageFormat screenshotFormat,
        RadialMenuSize menuSize,
        string recordingDirectory,
        Func<HotkeyDefinition, string?> applyHotkey,
        Func<bool, string?> applyAutoStart,
        Func<ScreenshotImageFormat, string?> applyScreenshotFormat,
        Func<RadialMenuSize, string?> applyMenuSize,
        Func<string, string?> applyRecordingDirectory)
    {
        InitializeComponent();

        _hotkey = hotkey.Clone();
        _applyHotkey = applyHotkey;
        _applyAutoStart = applyAutoStart;
        _applyScreenshotFormat = applyScreenshotFormat;
        _applyMenuSize = applyMenuSize;
        _applyRecordingDirectory = applyRecordingDirectory;
        _autoStartEnabled = autoStartEnabled;
        _format = screenshotFormat;
        _menuSize = menuSize;

        HotkeyValueText.Text = _hotkey.DisplayText;
        UpdateHotkeyHint();
        AutoStartToggle.IsChecked = autoStartEnabled;
        UpdateAutoStartUi();

        RecordingDirectoryBox.Text = recordingDirectory ?? string.Empty;
        RecordingDirectoryHint.Text = "留空时保存到：" + AppPaths.DefaultRecordingDirectory;
        PngRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Png;
        JpegRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Jpeg;
        SmallSizeRadio.IsChecked = menuSize == RadialMenuSize.Small;
        MediumSizeRadio.IsChecked = menuSize == RadialMenuSize.Medium;
        LargeSizeRadio.IsChecked = menuSize == RadialMenuSize.Large;

        AutoStartToggle.Checked += AutoStartToggle_Changed;
        AutoStartToggle.Unchecked += AutoStartToggle_Changed;
        PreviewKeyDown += OnPreviewKeyDown;
        Loaded += (_, _) => Activate();
    }

    private void HotkeyButton_Click(object sender, RoutedEventArgs e)
    {
        if (_capturingHotkey)
        {
            CancelHotkeyCapture();
            return;
        }

        BeginHotkeyCapture();
    }

    private void BeginHotkeyCapture()
    {
        _capturingHotkey = true;
        HotkeyValueText.Text = "请按下快捷键…";
        HotkeyValueText.Foreground = (Brush)FindResource("AccentBrush");
        HotkeyHint.Text = "等待输入：单键或组合键均可，Esc 取消";
        SaveButton.IsEnabled = false;
        ClearError();
        HotkeyButton.Focus();
    }

    private void CompleteHotkeyCapture()
    {
        _capturingHotkey = false;
        HotkeyValueText.Text = _hotkey.DisplayText;
        HotkeyValueText.Foreground = (Brush)FindResource("TextPrimaryBrush");
        UpdateHotkeyHint();
        SaveButton.IsEnabled = true;
        SaveButton.Focus();
    }

    private void CancelHotkeyCapture()
    {
        _capturingHotkey = false;
        HotkeyValueText.Text = _hotkey.DisplayText;
        HotkeyValueText.Foreground = (Brush)FindResource("TextPrimaryBrush");
        UpdateHotkeyHint();
        SaveButton.IsEnabled = true;
        ClearError();
    }

    private void UpdateHotkeyHint()
    {
        HotkeyHint.Text = _hotkey.IsSingleKey
            ? $"单键「{HotkeyDefinition.GetKeyName(_hotkey.Key)}」会被独占，其他应用将无法再输入该键。"
            : "点击输入框后按下快捷键；支持单键与组合键，Esc 取消";
    }

    private void OnPreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (_capturingHotkey)
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

            _hotkey = candidate;
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
        AutoStartStatusText.Text = _autoStartEnabled ? "已开启" : "已关闭";
        AutoStartStatusText.Foreground = (Brush)FindResource(
            _autoStartEnabled ? "AccentBrush" : "TextTertiaryBrush");
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

    private void RecordingDirectoryBox_TextChanged(object sender, System.Windows.Controls.TextChangedEventArgs e)
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
            Description = "选择录屏文件保存文件夹",
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
        if (_capturingHotkey)
        {
            return;
        }

        ClearError();
        SaveButton.IsEnabled = false;

        try
        {
            var hotkeyError = _applyHotkey(_hotkey.Clone());
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

            DialogResult = true;
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
        ErrorText.Visibility = Visibility.Visible;
        ErrorText.BringIntoView();
    }

    private void ClearError()
    {
        ErrorText.Text = string.Empty;
        ErrorText.Visibility = Visibility.Collapsed;
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
