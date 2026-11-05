using System;
using System.Collections.Generic;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using HaloCap.Diagnostics;
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
    private readonly Func<bool, string?>? _applyPinGlow;
    private readonly Func<OcrSettings, string?>? _applyOcrSettings;
    private readonly OcrSettings _ocrSettings;
    private readonly Func<RecordingSettings, string?>? _applyRecordingSettings;
    private readonly RecordingSettings _recordingSettings;
    private readonly Dictionary<HotkeySlot, HotkeyDefinition> _hotkeys = new();
    private readonly Dictionary<HotkeySlot, Button> _hotkeyButtons = new();
    private readonly Dictionary<HotkeySlot, TextBlock> _hotkeyValues = new();
    private HotkeySlot? _capturingSlot;
    private ScreenshotImageFormat _format;
    private RadialMenuSize _menuSize;
    private OcrEngineKind _ocrEngine;
    private bool _autoStartEnabled;
    private bool _suppressOcrUi;
    private bool _suppressRecordingUi;

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
        Func<bool, string?> applyConvertFilePathsToImages,
        OcrSettings? ocrSettings = null,
        Func<OcrSettings, string?>? applyOcrSettings = null,
        RecordingSettings? recordingSettings = null,
        Func<RecordingSettings, string?>? applyRecordingSettings = null,
        bool pinGlowEnabled = true,
        Func<bool, string?>? applyPinGlow = null)
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
        _applyPinGlow = applyPinGlow;
        _applyOcrSettings = applyOcrSettings;
        _ocrSettings = (ocrSettings ?? new OcrSettings()).Clone();
        _ocrSettings.Normalize();
        _ocrEngine = _ocrSettings.Engine;
        _applyRecordingSettings = applyRecordingSettings;
        _recordingSettings = (recordingSettings ?? new RecordingSettings()).Clone();
        _recordingSettings.Normalize();
        _autoStartEnabled = autoStartEnabled;
        _format = screenshotFormat;
        _menuSize = menuSize;

        VersionText.Text = AppInfo.VersionLabel;
        RefreshHotkeyFields();
        UpdateHotkeyHint();
        AutoStartToggle.IsChecked = autoStartEnabled;
        UpdateAutoStartUi();
        ConvertFilePathsToImagesToggle.IsChecked = convertFilePathsToImages;
        PinGlowToggle.IsChecked = pinGlowEnabled;

        RecordingDirectoryBox.Text = recordingDirectory ?? string.Empty;
        RecordingDirectoryHint.Text = "留空时保存到 " + AppPaths.DefaultRecordingDirectory;
        PngRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Png;
        JpegRadio.IsChecked = screenshotFormat == ScreenshotImageFormat.Jpeg;
        SmallSizeRadio.IsChecked = menuSize == RadialMenuSize.Small;
        MediumSizeRadio.IsChecked = menuSize == RadialMenuSize.Medium;
        LargeSizeRadio.IsChecked = menuSize == RadialMenuSize.Large;

        _suppressOcrUi = true;
        OcrNativeRadio.IsChecked = _ocrEngine == OcrEngineKind.Native;
        OcrModelRadio.IsChecked = _ocrEngine == OcrEngineKind.Model;
        OcrAutoCopyToggle.IsChecked = _ocrSettings.AutoCopy;
        PinOcrToggle.IsChecked = _ocrSettings.PinOcrEnabled;
        _suppressOcrUi = false;

        ApplyRecordingUi();

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

        HotkeyPane.Visibility = key == "hotkeys" ? Visibility.Visible : Visibility.Collapsed;
        GeneralPane.Visibility = key == "general" ? Visibility.Visible : Visibility.Collapsed;
        CapturePane.Visibility = key == "capture" ? Visibility.Visible : Visibility.Collapsed;
        OcrPane.Visibility = key == "ocr" ? Visibility.Visible : Visibility.Collapsed;
        RecordingPane.Visibility = key == "recording" ? Visibility.Visible : Visibility.Collapsed;
        AboutPane.Visibility = key == "about" ? Visibility.Visible : Visibility.Collapsed;
        SaveBar.Visibility = key == "about" ? Visibility.Collapsed : Visibility.Visible;
        SaveButton.IsDefault = key != "about";
        ClearError();
    }

    /// <summary>从「关于」等位置跳到某个面板，省得让人自己去侧栏里找。</summary>
    private void SelectPane(string uid)
    {
        foreach (var child in NavHost.Children)
        {
            if (child is RadioButton { Uid: { } key } item && key == uid)
            {
                item.IsChecked = true;
                return;
            }
        }
    }

    private void GoToHotkeyPane_Click(object sender, RoutedEventArgs e) => SelectPane("hotkeys");

    private void OpenDataFolder_Click(object sender, RoutedEventArgs e)
    {
        var kind = (sender as FrameworkElement)?.Tag as string;
        var path = kind switch
        {
            "logs" => Log.LogDirectory,
            "models" => OcrModelAssets.ModelsDirectory,
            _ => Path.GetDirectoryName(SettingsStore.FilePath) ?? Log.AppDataDirectory
        };

        try
        {
            if (!Directory.Exists(path))
            {
                ShowError(kind == "models"
                    ? "还没用过模型引擎，暂时还没有解压出模型文件。"
                    : "这个目录还不存在。");
                return;
            }

            System.Diagnostics.Process.Start(
                new System.Diagnostics.ProcessStartInfo("explorer.exe", "\"" + path + "\"")
                {
                    UseShellExecute = true
                });
            ClearError();
        }
        catch (Exception ex)
        {
            Log.Error("打开本地数据目录失败", ex);
            ShowError("无法打开目录：" + ex.Message);
        }
    }

    private void CopyDiagnostics_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Clipboard.SetText(CapabilityReport.Text);
            ClearError();
            FooterHint.Text = "环境摘要已复制，可直接粘贴给排查的人";
        }
        catch (Exception ex)
        {
            Log.Error("复制环境摘要失败", ex);
            ShowError("复制失败：" + ex.Message);
        }
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

    private void OcrEngineRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (_suppressOcrUi ||
            sender is not RadioButton { Tag: string tag } ||
            !Enum.TryParse<OcrEngineKind>(tag, out var engine))
        {
            return;
        }

        _ocrEngine = engine;
        ClearError();
    }

    private OcrSettings BuildOcrSettings() => new()
    {
        Engine = _ocrEngine,
        AutoCopy = OcrAutoCopyToggle.IsChecked == true,
        PinOcrEnabled = PinOcrToggle.IsChecked == true
    };

    /// <summary>隐藏光标要 Windows 10 2004+；更早的系统只能一直带上光标，界面得把这件事说清楚。</summary>
    private static bool SupportsCursorToggle =>
        WindowsCompatibility.SupportsCaptureToggle(WindowsCompatibility.CurrentBuild);

    private void ApplyRecordingUi()
    {
        _suppressRecordingUi = true;
        RecordResolutionOriginalRadio.IsChecked = _recordingSettings.Resolution == RecordingResolution.Original;
        RecordResolution2160Radio.IsChecked = _recordingSettings.Resolution == RecordingResolution.P2160;
        RecordResolution1440Radio.IsChecked = _recordingSettings.Resolution == RecordingResolution.P1440;
        RecordResolution1080Radio.IsChecked = _recordingSettings.Resolution == RecordingResolution.P1080;
        RecordResolution720Radio.IsChecked = _recordingSettings.Resolution == RecordingResolution.P720;
        RecordFps30Radio.IsChecked = _recordingSettings.FrameRate == RecordingFrameRate.Thirty;
        RecordFps60Radio.IsChecked = _recordingSettings.FrameRate == RecordingFrameRate.Sixty;
        RecordQualityHighRadio.IsChecked = _recordingSettings.Quality == RecordingQuality.High;
        RecordQualityBalancedRadio.IsChecked = _recordingSettings.Quality == RecordingQuality.Balanced;
        RecordQualityCompactRadio.IsChecked = _recordingSettings.Quality == RecordingQuality.Compact;
        RecordRangeRegionRadio.IsChecked = _recordingSettings.Range == RecordingRange.Region;
        RecordRangeWindowRadio.IsChecked = _recordingSettings.Range == RecordingRange.Window;
        RecordCountdownOffRadio.IsChecked = _recordingSettings.Countdown == RecordingCountdown.Off;
        RecordCountdown3Radio.IsChecked = _recordingSettings.Countdown == RecordingCountdown.ThreeSeconds;
        RecordCountdown5Radio.IsChecked = _recordingSettings.Countdown == RecordingCountdown.FiveSeconds;
        RecordCursorToggle.IsChecked = _recordingSettings.RecordCursor;
        RecordCursorToggle.IsEnabled = SupportsCursorToggle;

        if (!SupportsCursorToggle)
        {
            RecordCursorDescText.Text = WindowsCompatibility.DescribeCursorFallback(
                WindowsCompatibility.CurrentBuild);
        }

        SystemAudioToggle.IsChecked = _recordingSettings.SystemAudio;
        MicrophoneToggle.IsChecked = _recordingSettings.Microphone;
        AudioBitrate128Radio.IsChecked = _recordingSettings.AudioBitrate == RecordingAudioBitrate.K128;
        AudioBitrate192Radio.IsChecked = _recordingSettings.AudioBitrate == RecordingAudioBitrate.K192;
        _suppressRecordingUi = false;

        UpdateRecordingSummary();
    }

    private void RecordOptionRadio_Checked(object sender, RoutedEventArgs e)
    {
        if (_suppressRecordingUi || sender is not RadioButton { Tag: string tag })
        {
            return;
        }

        switch (((RadioButton)sender).GroupName)
        {
            case "RecordResolution" when Enum.TryParse<RecordingResolution>(tag, out var resolution):
                _recordingSettings.Resolution = resolution;
                break;
            case "RecordFrameRate" when Enum.TryParse<RecordingFrameRate>(tag, out var frameRate):
                _recordingSettings.FrameRate = frameRate;
                break;
            case "RecordQuality" when Enum.TryParse<RecordingQuality>(tag, out var quality):
                _recordingSettings.Quality = quality;
                break;
            case "RecordRange" when Enum.TryParse<RecordingRange>(tag, out var range):
                _recordingSettings.Range = range;
                break;
            case "RecordCountdown" when Enum.TryParse<RecordingCountdown>(tag, out var countdown):
                _recordingSettings.Countdown = countdown;
                break;
            case "RecordAudioBitrate" when Enum.TryParse<RecordingAudioBitrate>(tag, out var audioBitrate):
                _recordingSettings.AudioBitrate = audioBitrate;
                break;
            default:
                return;
        }

        ClearError();
        UpdateRecordingSummary();
    }

    private RecordingSettings BuildRecordingSettings() => new()
    {
        Resolution = _recordingSettings.Resolution,
        FrameRate = _recordingSettings.FrameRate,
        Quality = _recordingSettings.Quality,
        RecordCursor = RecordCursorToggle.IsChecked == true,
        Countdown = _recordingSettings.Countdown,
        Range = _recordingSettings.Range,
        SystemAudio = SystemAudioToggle.IsChecked == true,
        Microphone = MicrophoneToggle.IsChecked == true,
        AudioBitrate = _recordingSettings.AudioBitrate
    };

    private void UpdateRecordingSummary()
    {
        var preview = _recordingSettings.ResolveOutputSize(1920, 1080);
        var bitrate = _recordingSettings.ComputeBitrate(preview.Width, preview.Height);
        var megabits = bitrate / 1_000_000.0;

        FrameRateDescText.Text = _recordingSettings.FrameRate == RecordingFrameRate.Sixty
            ? "60fps 更顺滑，高负载时建议改回 30fps"
            : "30fps 足够流畅，也更省磁盘";

        QualityDescText.Text = _recordingSettings.Quality switch
        {
            RecordingQuality.High => "高：细节最多，文件明显更大",
            RecordingQuality.Compact => "紧凑：文件最小，快速移动的画面可能出现块状",
            _ => "均衡：日常分享与录课够用，码率自动计算"
        };

        var audio = (SystemAudioToggle.IsChecked == true, MicrophoneToggle.IsChecked == true) switch
        {
            (true, true) => "系统声音+麦克风",
            (true, false) => "仅系统声音",
            (false, true) => "仅麦克风",
            _ => "静音"
        };

        RecordingSummaryText.Text =
            $"{(_recordingSettings.Range == RecordingRange.Window ? "录制窗口" : "录制区域")}" +
            $" · 1080p 选区输出 {preview.Width}×{preview.Height}" +
            $" · 视频约 {megabits:0.#} Mbps · {audio} {_recordingSettings.AudioBitrateValue / 1000} kbps" +
            $" · {_recordingSettings.FramesPerSecond}fps" +
            $" · {(_recordingSettings.CountdownSeconds > 0 ? $"倒计时 {_recordingSettings.CountdownSeconds} 秒" : "不倒计时")}" +
            $" · {(SupportsCursorToggle
                ? (_recordingSettings.RecordCursor ? "含光标" : "不含光标")
                : "含光标（系统版本不支持隐藏）")}";
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
            var ocrSettings = BuildOcrSettings();
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

            if (_applyPinGlow is not null)
            {
                var glowError = _applyPinGlow(PinGlowToggle.IsChecked == true);

                if (!string.IsNullOrEmpty(glowError))
                {
                    ShowError("其他设置已更新；" + glowError + " 请修正后重试。");
                    return;
                }
            }

            if (_applyOcrSettings is not null)
            {
                var ocrError = _applyOcrSettings(ocrSettings);

                if (!string.IsNullOrEmpty(ocrError))
                {
                    ShowError("其他设置已更新；" + ocrError + " 请修正后重试。");
                    return;
                }
            }

            if (_applyRecordingSettings is not null)
            {
                var recordingError = _applyRecordingSettings(BuildRecordingSettings());

                if (!string.IsNullOrEmpty(recordingError))
                {
                    ShowError("其他设置已更新；" + recordingError + " 请修正后重试。");
                    return;
                }
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
