using System;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Shapes;
using System.Windows.Threading;

namespace HaloCap.UI;

public partial class TrayMenu : ContextMenu
{
    private static readonly Brush IconBrush = UiColors.Brush("#6E6E73");
    private static readonly Brush RecordingBrush = UiColors.Brush("#E5484D");
    private static readonly Brush RecordingTextBrush = UiColors.Brush("#D70015");

    public TrayMenu(string versionLabel, string hotkeyDisplay, bool autoStart, bool recording)
    {
        InitializeComponent();
        Tag = versionLabel;
        BuildMenu(hotkeyDisplay, autoStart, recording);

        Opened += (_, _) => Dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() => Keyboard.Focus(this)));
        PreviewKeyDown += (_, e) =>
        {
            if (e.Key == Key.Escape)
            {
                e.Handled = true;
                IsOpen = false;
            }
        };
    }

    public event EventHandler? OpenRequested;

    public event EventHandler? SettingsRequested;

    public event EventHandler? StopRecordingRequested;

    public event EventHandler? ExitRequested;

    public event Action<bool>? AutoStartToggleRequested;

    public void ShowAtCursor()
    {
        Placement = PlacementMode.MousePoint;
        IsOpen = true;
    }

    public void Dismiss() => IsOpen = false;

    private void BuildMenu(string hotkeyDisplay, bool autoStart, bool recording)
    {
        if (recording)
        {
            var stop = CreateItem(
                "停止录制",
                string.Empty,
                "M12 5.5A6.5 6.5 0 1 0 12 18.5A6.5 6.5 0 0 0 12 5.5Z",
                filled: true,
                iconBrush: RecordingBrush);
            stop.Foreground = RecordingTextBrush;
            stop.Click += (_, _) => Activate(StopRecordingRequested);
            Items.Add(stop);
            Items.Add(CreateSeparator());
        }

        var open = CreateItem(
            "打开光圈",
            hotkeyDisplay,
            "M12 4.5A7.5 7.5 0 1 0 12 19.5A7.5 7.5 0 0 0 12 4.5Z M12 9.5A2.5 2.5 0 1 0 12 14.5A2.5 2.5 0 0 0 12 9.5Z",
            enabled: !recording);
        open.Click += (_, _) => Activate(OpenRequested);
        Items.Add(open);

        var settings = CreateItem(
            "设置…",
            string.Empty,
            "M3 8H10 M14 8H21 M12 5V11 M3 16H7 M11 16H21 M9 13V19",
            enabled: !recording);
        settings.Click += (_, _) => Activate(SettingsRequested);
        Items.Add(settings);

        Items.Add(CreateSeparator());

        var autoStartItem = CreateItem(
            "登录时启动",
            string.Empty,
            "M12 3.5V11 M7.2 6.6A7.5 7.5 0 1 0 16.8 6.6",
            enabled: !recording);
        autoStartItem.IsCheckable = true;
        autoStartItem.IsChecked = autoStart;
        System.Windows.Automation.AutomationProperties.SetAutomationId(autoStartItem, "tray.item.autoStart");
        autoStartItem.Click += (_, _) =>
        {
            var desired = autoStartItem.IsChecked;
            IsOpen = false;
            AutoStartToggleRequested?.Invoke(desired);
        };
        Items.Add(autoStartItem);

        Items.Add(CreateSeparator());

        var exit = CreateItem(
            "退出 HaloCap",
            string.Empty,
            "M14 4H19V20H14 M11 8L15 12L11 16 M15 12H3");
        exit.Click += (_, _) => Activate(ExitRequested);
        Items.Add(exit);
    }

    private void Activate(EventHandler? handler)
    {
        IsOpen = false;
        handler?.Invoke(this, EventArgs.Empty);
    }

    private MenuItem CreateItem(
        string header,
        string shortcut,
        string iconGeometry,
        bool enabled = true,
        bool filled = false,
        Brush? iconBrush = null)
    {
        var item = new MenuItem
        {
            Header = header,
            InputGestureText = shortcut,
            IsEnabled = enabled,
            Style = (Style)FindResource("TrayMenuItemStyle"),
            Icon = CreateIcon(iconGeometry, filled, iconBrush ?? IconBrush)
        };

        return item;
    }

    private Separator CreateSeparator() =>
        new() { Style = (Style)FindResource("TrayMenuSeparatorStyle") };

    private static Viewbox CreateIcon(string geometry, bool filled, Brush brush)
    {
        var path = new Path
        {
            Data = Geometry.Parse(geometry),
            Stroke = brush,
            Fill = filled ? brush : null,
            StrokeThickness = 1.5,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            StrokeLineJoin = PenLineJoin.Round
        };

        return new Viewbox
        {
            Width = 16,
            Height = 16,
            Child = path
        };
    }
}
