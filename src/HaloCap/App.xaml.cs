using System;
using System.Collections.Generic;
using System.IO;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;
using HaloCap.UI;
using Forms = System.Windows.Forms;

namespace HaloCap;

public partial class App : Application
{
    private const string DisplayName = AppInfo.DisplayName;
    private const int RadialActionDelayMs = 60;

    private static readonly RadialAction[] ActionHotkeyOrder =
    {
        RadialAction.Screenshot,
        RadialAction.Ocr,
        RadialAction.Record,
        RadialAction.Pin
    };

    private readonly AutoStartService _autoStart = new();
    private readonly ClipboardPinService _clipboardPin = new();
    private readonly Dictionary<RadialAction, HotkeyService> _actionHotkeyServices = new();
    private SingleInstanceService? _singleInstance;
    private HotkeyService? _hotkeyService;
    private TrayIconService? _tray;
    private TrayMenu? _trayMenu;
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

        _radial = new RadialMenuController(_settings.Hotkey, _settings.MenuSize)
        {
            PinAvailabilityProvider = () => _clipboardPin.CanPin
        };
        _radial.ActionSelected += OnActionSelected;

        _tray = new TrayIconService();
        _tray.MenuRequested += (_, _) => Defer(ShowTrayMenu);
        _tray.OpenRequested += (_, _) => Defer(ShowRadialFromTray);
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

        var actionHotkeyErrors = new List<string>();
        foreach (var action in ActionHotkeyOrder)
        {
            var capturedAction = action;
            var service = new HotkeyService();
            service.Pressed += (_, _) => Defer(() => TriggerAction(capturedAction));
            _actionHotkeyServices[capturedAction] = service;

            if (!service.TryChange(_settings.GetActionHotkey(capturedAction), out var actionError))
            {
                actionHotkeyErrors.Add(
                    $"{RadialActionInfo.Get(capturedAction).Title}：{actionError ?? "无法注册"}");
            }
            else
            {
                Log.Info($"已注册{RadialActionInfo.Get(capturedAction).Title}快捷键：{_settings.GetActionHotkey(capturedAction).DisplayText}");
            }
        }

        _singleInstance!.ActivationRequested += (_, _) =>
            Dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(ShowRadialFromTray));
        _singleInstance.StartListening();

        if (!registered)
        {
            _tray.ShowNotification(
                "快捷键未生效",
                hotkeyError ?? "无法注册默认快捷键，请在设置中更换。",
                System.Windows.Forms.ToolTipIcon.Warning);
        }

        if (actionHotkeyErrors.Count > 0)
        {
            _tray.ShowNotification(
                "部分快捷键未生效",
                string.Join(Environment.NewLine, actionHotkeyErrors) + Environment.NewLine + "可在设置中更换。",
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

    private bool CanToggleRadial() =>
        !_isExiting && !IsBusy && _settingsWindow is not { IsCapturingHotkey: true };

    private bool CanShowTrayMenu() => !_isExiting && !_isDialogOpen;

    private void ToggleRadialFromHotkey()
    {
        if (!CanToggleRadial())
        {
            return;
        }

        _radial?.ToggleFromHotkey();
    }

    private void ShowRadialFromTray()
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        _radial?.ShowFromTray();
    }

    private void ShowTrayMenu()
    {
        if (!CanShowTrayMenu())
        {
            return;
        }

        _trayMenu?.Dismiss();

        var hotkey = _hotkeyService?.Current?.DisplayText ?? _settings.Hotkey.DisplayText;
        var menu = new TrayMenu(
            AppInfo.VersionLabel,
            hotkey,
            GetActionHotkeys(),
            _autoStart.IsEnabled(),
            _recording?.IsRecording == true,
            _clipboardPin.CanPin);

        menu.OpenRequested += (_, _) => Defer(ShowRadialFromTray);
        menu.SettingsRequested += (_, _) => Defer(OpenSettings);
        menu.ActionRequested += action => Defer(() => TriggerAction(action, RadialActionDelayMs));
        menu.AutoStartToggleRequested += enabled => Defer(() => ToggleAutoStartFromTray(enabled));
        menu.ExitRequested += (_, _) => Defer(ExitApplication);
        menu.Closed += (_, _) =>
        {
            if (ReferenceEquals(_trayMenu, menu))
            {
                _trayMenu = null;
            }
        };

        _trayMenu = menu;
        menu.ShowAtCursor();
    }

    private void OpenSettings()
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        _trayMenu?.Dismiss();
        _radial?.HideMenu();

        if (_settingsWindow is { IsVisible: true })
        {
            _settingsWindow.Activate();
            return;
        }

        var window = new SettingsWindow(
            _hotkeyService?.Current ?? _settings.Hotkey,
            GetActionHotkeys(),
            _autoStart.IsEnabled(),
            _settings.ScreenshotFormat,
            _settings.MenuSize,
            _settings.RecordingDirectory,
            _settings.ConvertFilePathsToImages,
            ApplyHotkeys,
            ApplyAutoStart,
            ApplyScreenshotFormat,
            ApplyMenuSize,
            ApplyRecordingDirectory,
            ApplyConvertFilePathsToImages);

        _settingsWindow = window;
        window.Closed += (_, _) =>
        {
            if (ReferenceEquals(_settingsWindow, window))
            {
                _settingsWindow = null;
            }
        };

        window.Show();
        window.Activate();
    }

    private Dictionary<RadialAction, HotkeyDefinition> GetActionHotkeys()
    {
        var actionHotkeys = new Dictionary<RadialAction, HotkeyDefinition>();

        foreach (var action in ActionHotkeyOrder)
        {
            var registeredHotkey = _actionHotkeyServices.TryGetValue(action, out var service)
                ? service.Current
                : null;
            actionHotkeys[action] = (registeredHotkey ?? _settings.GetActionHotkey(action)).Clone();
        }

        return actionHotkeys;
    }

    private string? ApplyHotkeys(
        HotkeyDefinition radial,
        IReadOnlyDictionary<RadialAction, HotkeyDefinition> actions)
    {
        if (_hotkeyService is null)
        {
            return "热键服务尚未就绪，请重启 HaloCap。";
        }

        var labels = new List<(string Label, HotkeyDefinition Hotkey)>
        {
            ("呼出光圈", radial)
        };

        foreach (var action in ActionHotkeyOrder)
        {
            labels.Add((RadialActionInfo.Get(action).Title, actions[action]));
        }

        for (var first = 0; first < labels.Count; first++)
        {
            for (var second = first + 1; second < labels.Count; second++)
            {
                if (SameHotkey(labels[first].Hotkey, labels[second].Hotkey))
                {
                    return $"「{labels[first].Label}」与「{labels[second].Label}」使用了相同的快捷键，请更换其中一个。";
                }
            }
        }

        var previousRadial = _settings.Hotkey.Clone();
        var previousActions = new Dictionary<RadialAction, HotkeyDefinition>();
        foreach (var action in ActionHotkeyOrder)
        {
            previousActions[action] = _settings.GetActionHotkey(action).Clone();
        }

        if (!_hotkeyService.TryChange(radial, out var error))
        {
            RefreshRadialHotkeyState();
            return error ?? "无法注册该快捷键，请换一个组合键。";
        }

        var changedActions = new List<RadialAction>();
        foreach (var action in ActionHotkeyOrder)
        {
            var service = _actionHotkeyServices[action];
            if (!service.TryChange(actions[action], out error))
            {
                RestoreHotkeys(previousRadial, previousActions, changedActions);
                return $"{RadialActionInfo.Get(action).Title}：{error ?? "无法注册该快捷键。"}";
            }

            changedActions.Add(action);
        }

        _settings.Hotkey = radial.Clone();
        foreach (var action in ActionHotkeyOrder)
        {
            _settings.SetActionHotkey(action, actions[action].Clone());
        }

        try
        {
            SettingsStore.Save(_settings);
        }
        catch (Exception ex)
        {
            Log.Error("保存快捷键设置失败", ex);
            _settings.Hotkey = previousRadial;
            foreach (var action in ActionHotkeyOrder)
            {
                _settings.SetActionHotkey(action, previousActions[action]);
            }

            RestoreHotkeys(previousRadial, previousActions, ActionHotkeyOrder);
            return "无法保存设置，已恢复原快捷键：" + ex.Message;
        }

        RefreshRadialHotkeyState();
        return null;
    }

    private void RestoreHotkeys(
        HotkeyDefinition radial,
        IReadOnlyDictionary<RadialAction, HotkeyDefinition> actions,
        IReadOnlyCollection<RadialAction> changedActions)
    {
        if (!_hotkeyService!.TryChange(radial, out var radialError))
        {
            Log.Error($"恢复呼出快捷键失败：{radialError}");
        }

        foreach (var action in changedActions)
        {
            if (!_actionHotkeyServices.TryGetValue(action, out var service))
            {
                continue;
            }

            if (!service.TryChange(actions[action], out var actionError))
            {
                Log.Error($"恢复{RadialActionInfo.Get(action).Title}快捷键失败：{actionError}");
            }
        }

        RefreshRadialHotkeyState();
    }

    private void RefreshRadialHotkeyState()
    {
        var current = _hotkeyService?.Current;
        _radial?.UpdateHotkey(current ?? _settings.Hotkey, current is not null);
        UpdateHotkeyUi();
    }

    private static bool SameHotkey(HotkeyDefinition first, HotkeyDefinition second) =>
        first.Modifiers == second.Modifiers && first.Key == second.Key;

    private string? ApplyAutoStart(bool enabled)
    {
        try
        {
            _autoStart.SetEnabled(enabled);
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
            Log.Error("保存光圈尺寸失败", ex);
            return "无法保存光圈尺寸：" + ex.Message;
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
                return "请输入完整路径，例如 D:\\Recordings。";
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

    private string? ApplyConvertFilePathsToImages(bool enabled)
    {
        if (_settings.ConvertFilePathsToImages == enabled)
        {
            return null;
        }

        var previous = _settings.ConvertFilePathsToImages;
        _settings.ConvertFilePathsToImages = enabled;

        try
        {
            SettingsStore.Save(_settings);
            _radial?.RefreshActionAvailability();
            return null;
        }
        catch (Exception ex)
        {
            _settings.ConvertFilePathsToImages = previous;
            Log.Error("保存剪贴板贴图设置失败", ex);
            return "无法保存剪贴板贴图设置：" + ex.Message;
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

    private void TriggerAction(RadialAction action, int delayMs = 0)
    {
        if (_isExiting || _settingsWindow is { IsCapturingHotkey: true })
        {
            return;
        }

        if (action == RadialAction.Record && _recording?.IsRecording == true)
        {
            _ = StopRecordingAsync();
            return;
        }

        // 截图选区完成后，贴图快捷键直接贴出当前选区，而不是重新打开一个选区。
        if (action == RadialAction.Pin && _screenshotSession?.TryPinSelection() == true)
        {
            return;
        }

        if (IsBusy)
        {
            return;
        }

        if (action == RadialAction.Pin)
        {
            _radial?.HideMenu();
            PinFromClipboard();
            return;
        }

        var wasRadialOpen = _radial?.IsOpen == true;
        // 光圈不再淡出，只需等它真正从屏幕消失，避免遮罩把光圈截进去。
        _radial?.HideMenu(immediate: wasRadialOpen);
        var wait = wasRadialOpen ? Math.Max(delayMs, RadialActionDelayMs) : delayMs;
        _ = StartCaptureAsync(ToCaptureMode(action), wait);
    }

    private static CaptureSessionMode ToCaptureMode(RadialAction action) => action switch
    {
        RadialAction.Ocr => CaptureSessionMode.Ocr,
        RadialAction.Record => CaptureSessionMode.Record,
        _ => CaptureSessionMode.Screenshot
    };

    private async void OnActionSelected(RadialAction action)
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        if (action == RadialAction.Pin)
        {
            PinFromClipboard();
            return;
        }

        // 光圈已即时收起，只留一帧让窗口真正消失，避免遮罩把光圈本身截进去。
        await StartCaptureAsync(ToCaptureMode(action), RadialActionDelayMs);
    }

    private async Task StartCaptureAsync(CaptureSessionMode mode, int delayMs)
    {
        if (_isExiting || IsBusy)
        {
            return;
        }

        _captureStarting = true;

        try
        {
            if (delayMs > 0)
            {
                await Task.Delay(delayMs);
            }

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

    private void CreatePin(BitmapSource image, PhysicalRect rect, double scale)
    {
        var pin = new PinWindow(image, rect, scale, _settings.ScreenshotFormat, _settings.PinGlowEnabled);
        pin.OutlineChanged += OnPinOutlineChanged;
        pin.ShowPin();
    }

    private void OnPinOutlineChanged(bool enabled)
    {
        if (_settings.PinGlowEnabled == enabled)
        {
            return;
        }

        _settings.PinGlowEnabled = enabled;

        try
        {
            SettingsStore.Save(_settings);
        }
        catch (Exception ex)
        {
            Log.Error("保存贴图外描边设置失败", ex);
        }
    }

    private void PinFromClipboard()
    {
        try
        {
            var monitor = MonitorService.GetMonitorAtCursor();
            var contents = _clipboardPin.CreatePins(_settings.ConvertFilePathsToImages, monitor.Scale);
            if (contents.Count == 0)
            {
                return;
            }

            NativeMethods.GetCursorPos(out var cursor);
            for (var index = 0; index < contents.Count; index++)
            {
                var content = contents[index];
                var work = monitor.WorkArea;
                var fit = Math.Min(1, Math.Min((work.Width - 32 * monitor.Scale) / content.Image.PixelWidth,
                    (work.Height - 32 * monitor.Scale) / content.Image.PixelHeight));
                var width = Math.Max(24, (int)Math.Round(content.Image.PixelWidth * fit));
                var height = Math.Max(24, (int)Math.Round(content.Image.PixelHeight * fit));
                var offset = (int)Math.Round(index * 18 * monitor.Scale);
                var left = cursor.X - (width / 2) + offset;
                var top = cursor.Y - (height / 2) + offset;
                CreatePin(content.Image, new PhysicalRect(left, top, width, height), monitor.Scale);
                _clipboardPin.MarkPasted(content);
            }
        }
        catch (Exception ex)
        {
            Log.Error("剪贴板贴图失败", ex);
            _tray?.ShowNotification("贴图失败", "无法读取剪贴板，请复制内容后重试。", Forms.ToolTipIcon.Warning);
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
                "无法获取屏幕画面，请关闭受保护内容后重试。",
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
                        CreatePin(image, result.PhysicalRect, result.Monitor.Scale);
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

    private void StopRecordingFromTray() => _ = StopRecordingAsync();

    private async Task StopRecordingAsync()
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
            _tray?.ShowNotification("无法停止录屏", ex.Message, Forms.ToolTipIcon.Error);
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
                Path.GetFileName(path) + " · 单击定位文件",
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

        _trayMenu?.Dismiss();
        _trayMenu = null;
        _radial?.Dispose();
        _radial = null;
        _tray?.Dispose();
        _tray = null;
        _hotkeyService?.Dispose();
        _hotkeyService = null;

        foreach (var service in _actionHotkeyServices.Values)
        {
            service.Dispose();
        }

        _actionHotkeyServices.Clear();
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
            $"HaloCap 遇到问题，但仍在运行。\n\n{e.Exception.Message}\n\n日志目录：{Log.LogDirectory}",
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
