using System;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Models;
using HaloCap.Services;
using HaloCap.UI;
using Forms = System.Windows.Forms;

namespace HaloCap;

public partial class App : Application
{
    private const string DisplayName = "HaloCap 光圈";

    private readonly AutoStartService _autoStart = new();
    private SingleInstanceService? _singleInstance;
    private HotkeyService? _hotkeyService;
    private TrayIconService? _tray;
    private RadialMenuController? _radial;
    private RecordingService? _recording;
    private ScreenshotSession? _screenshotSession;
    private SettingsWindow? _settingsWindow;
    private AppSettings _settings = new();
    private bool _captureStarting;
    private bool _isDialogOpen;
    private bool _isExiting;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        ShutdownMode = ShutdownMode.OnExplicitShutdown;
        DispatcherUnhandledException += OnDispatcherUnhandledException;
        AppDomain.CurrentDomain.UnhandledException += OnDomainUnhandledException;

        try
        {
            _singleInstance = new SingleInstanceService();
            if (!_singleInstance.TryAcquire())
            {
                SingleInstanceService.NotifyExistingInstance();
                Shutdown(0);
                return;
            }

            InitializeServices();
        }
        catch (Exception ex)
        {
            Log.Error("HaloCap 启动失败", ex);
            MessageBox.Show(
                $"HaloCap 启动失败：\n{ex.Message}\n\n详细日志：{Log.LogDirectory}",
                DisplayName,
                MessageBoxButton.OK,
                MessageBoxImage.Error);
            Shutdown(1);
        }
    }

    private void InitializeServices()
    {
        _settings = SettingsStore.Load();

        _radial = new RadialMenuController(_settings.Hotkey, _settings.MenuSize);
        _radial.ActionSelected += OnActionSelected;

        _tray = new TrayIconService
        {
            AutoStartStateProvider = _autoStart.IsEnabled
        };
        _tray.OpenRequested += (_, _) => Defer(ShowRadialFromTray);
        _tray.SettingsRequested += (_, _) => Defer(OpenSettings);
        _tray.ExitRequested += (_, _) => Defer(ExitApplication);
        _tray.AutoStartToggleRequested += enabled => Defer(() => ToggleAutoStartFromTray(enabled));
        _tray.StopRecordingRequested += (_, _) => Defer(StopRecordingFromTray);

        _recording = new RecordingService();
        _recording.StateChanged += OnRecordingStateChanged;
        _recording.Saved += OnRecordingSaved;
        _recording.Failed += OnRecordingFailed;

        _hotkeyService = new HotkeyService();
        _hotkeyService.Pressed += (_, _) => Defer(ToggleRadialFromHotkey);

        var registered = _hotkeyService.TryChange(_settings.Hotkey, out var hotkeyError);
        var activeHotkey = _hotkeyService.Current;
        _radial.UpdateHotkey(activeHotkey ?? _settings.Hotkey, activeHotkey is not null);
        UpdateHotkeyUi();

        _singleInstance!.ActivationRequested += (_, _) =>
            Dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(ShowRadialFromTray));
        _singleInstance.StartListening();

        if (!registered)
        {
            _tray.ShowNotification(
                "快捷键未注册",
                hotkeyError ?? "无法注册默认快捷键，请在托盘菜单中打开设置更换组合键。",
                System.Windows.Forms.ToolTipIcon.Warning);
        }

        Log.Info("HaloCap 已启动。");
    }

    private void Defer(Action action)
    {
        Dispatcher.BeginInvoke(DispatcherPriority.Input, action);
    }

    private bool IsBusy =>
        _captureStarting ||
        _isDialogOpen ||
        _screenshotSession is not null ||
        _recording is { IsRecording: true };

    private void ToggleRadialFromHotkey()
    {
        if (_isExiting || _settingsWindow is { IsVisible: true } || IsBusy)
        {
            return;
        }

        _radial?.ToggleFromHotkey();
    }

    private void ShowRadialFromTray()
    {
        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        if (_isExiting || IsBusy)
        {
            return;
        }

        _radial?.ShowFromTray();
    }

    private void OpenSettings()
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        _radial?.HideMenu();

        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        var window = new SettingsWindow(
            _hotkeyService?.Current ?? _settings.Hotkey,
            _autoStart.IsEnabled(),
            _settings.ScreenshotFormat,
            _settings.MenuSize,
            _settings.RecordingDirectory,
            ApplyHotkey,
            ApplyAutoStart,
            ApplyScreenshotFormat,
            ApplyMenuSize,
            ApplyRecordingDirectory);

        _settingsWindow = window;
        window.Closed += (_, _) =>
        {
            if (ReferenceEquals(_settingsWindow, window))
            {
                _settingsWindow = null;
            }
        };

        window.ShowDialog();
    }

    private string? ApplyHotkey(HotkeyDefinition hotkey)
    {
        if (_hotkeyService is null)
        {
            return "热键服务尚未就绪，请重新启动 HaloCap。";
        }

        if (!_hotkeyService.TryChange(hotkey, out var error))
        {
            var registeredHotkey = _hotkeyService.Current;
            _radial?.UpdateHotkey(registeredHotkey ?? _settings.Hotkey, registeredHotkey is not null);
            UpdateHotkeyUi();
            return error ?? "无法注册该快捷键。";
        }

        var previous = _settings.Hotkey;
        _settings.Hotkey = hotkey.Clone();

        try
        {
            SettingsStore.Save(_settings);
        }
        catch (Exception ex)
        {
            Log.Error("保存设置失败", ex);
            _settings.Hotkey = previous;

            if (!_hotkeyService.TryChange(previous, out var restoreError))
            {
                Log.Error($"保存失败后恢复原快捷键失败：{restoreError}");
            }

            var restoredHotkey = _hotkeyService.Current;
            _radial?.UpdateHotkey(restoredHotkey ?? previous, restoredHotkey is not null);
            UpdateHotkeyUi();
            return "无法保存设置，已尝试恢复原快捷键：" + ex.Message;
        }

        _radial?.UpdateHotkey(_settings.Hotkey, true);
        UpdateHotkeyUi();
        return null;
    }

    private string? ApplyAutoStart(bool enabled)
    {
        try
        {
            _autoStart.SetEnabled(enabled);
            _tray?.RefreshState();
            return null;
        }
        catch (Exception ex)
        {
            Log.Error("应用开机自启设置失败", ex);
            return ex.Message;
        }
    }

    private string? ApplyScreenshotFormat(ScreenshotImageFormat format)
    {
        if (_settings.ScreenshotFormat == format)
        {
            return null;
        }

        var previous = _settings.ScreenshotFormat;
        _settings.ScreenshotFormat = format;

        try
        {
            SettingsStore.Save(_settings);
            return null;
        }
        catch (Exception ex)
        {
            _settings.ScreenshotFormat = previous;
            Log.Error("保存截图格式失败", ex);
            return "无法保存截图格式：" + ex.Message;
        }
    }

    private string? ApplyMenuSize(RadialMenuSize size)
    {
        if (_settings.MenuSize == size)
        {
            _radial?.UpdateMenuSize(size);
            return null;
        }

        var previous = _settings.MenuSize;
        _settings.MenuSize = size;
        _radial?.UpdateMenuSize(size);

        try
        {
            SettingsStore.Save(_settings);
            return null;
        }
        catch (Exception ex)
        {
            _settings.MenuSize = previous;
            _radial?.UpdateMenuSize(previous);
            Log.Error("保存圆盘尺寸失败", ex);
            return "无法保存圆盘尺寸：" + ex.Message;
        }
    }

    private string? ApplyRecordingDirectory(string directory)
    {
        string normalized;

        try
        {
            var expanded = string.IsNullOrWhiteSpace(directory)
                ? string.Empty
                : Environment.ExpandEnvironmentVariables(directory.Trim().Trim('"'));

            if (expanded.Length > 0 && !Path.IsPathFullyQualified(expanded))
            {
                return "请输入绝对路径，例如 D:\\Recordings。";
            }

            normalized = expanded.Length == 0
                ? string.Empty
                : AppPaths.NormalizeDirectory(expanded);

            if (normalized.Length > 0)
            {
                Directory.CreateDirectory(normalized);
            }
        }
        catch (Exception ex)
        {
            Log.Error("录制目录不可用", ex);
            return "录制目录不可用：" + ex.Message;
        }

        var previous = _settings.RecordingDirectory;
        _settings.RecordingDirectory = normalized;

        try
        {
            SettingsStore.Save(_settings);
            return null;
        }
        catch (Exception ex)
        {
            _settings.RecordingDirectory = previous;
            Log.Error("保存录制目录失败", ex);
            return "无法保存录制目录：" + ex.Message;
        }
    }

    private void ToggleAutoStartFromTray(bool enabled)
    {
        var error = ApplyAutoStart(enabled);
        if (string.IsNullOrEmpty(error))
        {
            return;
        }

        MessageBox.Show(
            error,
            DisplayName,
            MessageBoxButton.OK,
            MessageBoxImage.Warning);
    }

    private void UpdateHotkeyUi()
    {
        var current = _hotkeyService?.Current;
        _tray?.SetHotkeyText(current?.DisplayText ?? "未注册");
    }

    private async void OnActionSelected(RadialAction action)
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        var mode = action switch
        {
            RadialAction.Ocr => CaptureSessionMode.Ocr,
            RadialAction.Record => CaptureSessionMode.Record,
            RadialAction.Pin => CaptureSessionMode.Pin,
            _ => CaptureSessionMode.Screenshot
        };

        _captureStarting = true;

        try
        {
            // 等待圆盘淡出，避免遮罩把圆盘本身截进去。
            await Task.Delay(170);

            if (_isExiting)
            {
                return;
            }

            _screenshotSession = ScreenshotSession.Start(mode, HandleCaptureCompleted);
        }
        catch (Exception ex)
        {
            Log.Error("打开截图/录制选区失败", ex);
            _tray?.ShowNotification(
                "无法打开选区",
                ex.Message,
                Forms.ToolTipIcon.Error);
        }
        finally
        {
            _captureStarting = false;
        }
    }

    private async void HandleCaptureCompleted(CaptureResult result)
    {
        _screenshotSession = null;

        if (_isExiting || result.Command == CaptureCommand.Cancel)
        {
            return;
        }

        if (result.Command == CaptureCommand.StartRecording)
        {
            await StartRecordingAsync(result);
            return;
        }

        var image = result.Image;

        if (image is null)
        {
            _tray?.ShowNotification(
                "截图失败",
                "未能获取屏幕画面。请确认没有启用受保护内容后重试。",
                Forms.ToolTipIcon.Error);
            return;
        }

        try
        {
            switch (result.Command)
            {
                case CaptureCommand.Copy:
                {
                    ImageClipboard.SetImage(image);
                    _tray?.ShowNotification(
                        "已复制截图",
                        $"{image.PixelWidth} × {image.PixelHeight} 像素已复制到剪贴板。");
                    break;
                }

                case CaptureCommand.Save:
                {
                    string? path;
                    _isDialogOpen = true;

                    try
                    {
                        path = ImageFileService.SaveWithDialog(image, _settings.ScreenshotFormat);
                    }
                    finally
                    {
                        _isDialogOpen = false;
                    }

                    if (path is not null)
                    {
                        _tray?.ShowNotification("截图已保存", path);
                    }

                    break;
                }

                case CaptureCommand.Pin:
                {
                    if (result.Monitor is not null)
                    {
                        var pin = new PinWindow(
                            image,
                            result.PhysicalRect,
                            result.Monitor.Scale,
                            _settings.ScreenshotFormat);
                        pin.ShowPin();
                    }

                    break;
                }

                case CaptureCommand.Ocr:
                {
                    var monitor = result.Monitor ?? MonitorService.GetMonitorAtCursor();
                    var window = new OcrResultWindow(image, result.PhysicalRect, monitor);
                    window.Show();
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            Log.Error($"处理截图操作 {result.Command} 失败", ex);
            _tray?.ShowNotification("操作未完成", ex.Message, Forms.ToolTipIcon.Warning);
        }
    }

    private async Task StartRecordingAsync(CaptureResult result)
    {
        if (_recording is null || result.Monitor is null)
        {
            return;
        }

        _captureStarting = true;

        try
        {
            await Task.Delay(140);

            if (_isExiting)
            {
                return;
            }

            await _recording.StartAsync(
                result.Monitor,
                result.PhysicalRect,
                _settings.RecordingDirectory);
        }
        catch (Exception ex)
        {
            if (ex is not OperationCanceledException)
            {
                Log.Error("启动录屏失败", ex);
                _tray?.ShowNotification("录屏未开始", ex.Message, Forms.ToolTipIcon.Error);
            }
        }
        finally
        {
            _captureStarting = false;
        }
    }

    private async void StopRecordingFromTray()
    {
        if (_recording is null || !_recording.IsRecording)
        {
            return;
        }

        try
        {
            await _recording.StopAsync();
        }
        catch (Exception ex)
        {
            Log.Error("停止录屏失败", ex);
            _tray?.ShowNotification("停止录屏失败", ex.Message, Forms.ToolTipIcon.Error);
        }
    }

    private void OnRecordingStateChanged()
    {
        _tray?.SetRecordingState(_recording?.IsRecording == true);
    }

    private void OnRecordingSaved(string path)
    {
        try
        {
            _tray?.ShowNotificationWithAction(
                "录屏已保存",
                Path.GetFileName(path) + " · 点击打开所在文件夹",
                path,
                Forms.ToolTipIcon.Info);
            Log.Info("录屏已保存：" + path);
        }
        catch (Exception ex)
        {
            Log.Error("显示录屏完成通知失败", ex);
        }
    }

    private void OnRecordingFailed(string message)
    {
        _tray?.ShowNotification("录屏未完成", message, Forms.ToolTipIcon.Warning);
        Log.Error("录屏失败：" + message);
    }

    private void ExitApplication()
    {
        if (_isExiting)
        {
            return;
        }

        _isExiting = true;
        Shutdown();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        _isExiting = true;

        try
        {
            if (_settingsWindow is { IsVisible: true })
            {
                _settingsWindow.Close();
            }
        }
        catch (Exception ex)
        {
            Log.Error("关闭设置窗口失败", ex);
        }

        try
        {
            _screenshotSession?.Dispose();
        }
        catch (Exception ex)
        {
            Log.Error("关闭选区失败", ex);
        }

        _screenshotSession = null;

        try
        {
            PinWindow.CloseAllPins();
        }
        catch (Exception ex)
        {
            Log.Error("关闭全部贴图失败", ex);
        }

        try
        {
            OcrResultWindow.CloseAllWindows();
        }
        catch (Exception ex)
        {
            Log.Error("关闭全部 OCR 窗口失败", ex);
        }

        try
        {
            _recording?.Dispose();
        }
        catch (Exception ex)
        {
            Log.Error("退出时结束录制失败", ex);
        }

        _recording = null;

        _radial?.Dispose();
        _radial = null;
        _tray?.Dispose();
        _tray = null;
        _hotkeyService?.Dispose();
        _hotkeyService = null;
        _singleInstance?.Dispose();
        _singleInstance = null;

        Log.Info("HaloCap 已退出。");
        base.OnExit(e);
    }

    private void OnDispatcherUnhandledException(
        object sender,
        DispatcherUnhandledExceptionEventArgs e)
    {
        Log.Error("UI 线程未处理异常", e.Exception);
        e.Handled = true;

        if (_isExiting)
        {
            return;
        }

        MessageBox.Show(
            $"HaloCap 遇到错误，但仍在运行。\n\n{e.Exception.Message}\n\n详细日志：{Log.LogDirectory}",
            DisplayName,
            MessageBoxButton.OK,
            MessageBoxImage.Error);
    }

    private static void OnDomainUnhandledException(object? sender, UnhandledExceptionEventArgs e)
    {
        if (e.ExceptionObject is Exception exception)
        {
            Log.Error("应用域未处理异常", exception);
        }
        else
        {
            Log.Error("应用域发生未知的未处理异常");
        }
    }
}
