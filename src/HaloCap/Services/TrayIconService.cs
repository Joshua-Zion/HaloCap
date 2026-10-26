using System;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Windows.Forms;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using DrawingColor = System.Drawing.Color;

namespace HaloCap.Services;

internal sealed class TrayIconService : IDisposable
{
    private readonly NotifyIcon _notifyIcon;
    private readonly ContextMenuStrip _menu;
    private readonly StopRecordingToolStripMenuItem _stopRecordingItem;
    private readonly ToolStripSeparator _stopRecordingSeparator;
    private readonly ToolStripMenuItem _openItem;
    private readonly ToolStripMenuItem _settingsItem;
    private readonly ToolStripMenuItem _autoStartItem;
    private readonly ToolStripMenuItem _hotkeyItem;
    private readonly ToolStripMenuItem _exitItem;
    private readonly Font _menuFont;
    private readonly Font _openItemFont;
    private readonly Icon _idleIcon;
    private readonly Icon _recordingIcon;
    private string _hotkeyDisplay = string.Empty;
    private string? _balloonActionPath;
    private bool _isRecording;
    private bool _disposed;

    public TrayIconService()
    {
        _menuFont = new Font("Microsoft YaHei UI", 9.5f, FontStyle.Regular, GraphicsUnit.Point);
        _openItemFont = new Font(_menuFont, FontStyle.Bold);
        _idleIcon = CreateTrayIcon(false);
        _recordingIcon = CreateTrayIcon(true);

        _stopRecordingItem = new StopRecordingToolStripMenuItem("●  停止录制")
        {
            Font = _openItemFont,
            Padding = new Padding(14, 7, 18, 7),
            Visible = false
        };
        _stopRecordingItem.Click += (_, _) => Raise(StopRecordingRequested);

        _stopRecordingSeparator = new ToolStripSeparator { Visible = false };

        _openItem = new ToolStripMenuItem("打开圆盘")
        {
            Font = _openItemFont,
            Padding = new Padding(14, 7, 18, 7)
        };
        _openItem.Click += (_, _) => Raise(OpenRequested);

        _settingsItem = new ToolStripMenuItem("设置…")
        {
            Padding = new Padding(14, 7, 18, 7)
        };
        _settingsItem.Click += (_, _) => Raise(SettingsRequested);

        _autoStartItem = new ToolStripMenuItem
        {
            Padding = new Padding(14, 7, 18, 7)
        };
        _autoStartItem.Click += (_, _) =>
        {
            var desired = !(AutoStartStateProvider?.Invoke() ?? false);
            AutoStartToggleRequested?.Invoke(desired);
        };

        _hotkeyItem = new ToolStripMenuItem
        {
            Enabled = false,
            Padding = new Padding(14, 5, 18, 7)
        };

        _exitItem = new ToolStripMenuItem("退出 HaloCap")
        {
            Padding = new Padding(14, 7, 18, 7)
        };
        _exitItem.Click += (_, _) => Raise(ExitRequested);

        _menu = new ContextMenuStrip
        {
            ShowImageMargin = false,
            Font = _menuFont,
            Padding = new Padding(0, 5, 0, 5),
            Renderer = new DarkMenuRenderer(),
            DropShadowEnabled = true,
            BackColor = DrawingColor.FromArgb(24, 28, 37),
            ForeColor = DrawingColor.FromArgb(232, 237, 245)
        };

        _menu.Items.Add(_stopRecordingItem);
        _menu.Items.Add(_stopRecordingSeparator);
        _menu.Items.Add(_openItem);
        _menu.Items.Add(_settingsItem);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_autoStartItem);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_hotkeyItem);
        _menu.Items.Add(new ToolStripSeparator());
        _menu.Items.Add(_exitItem);
        _menu.Opening += (_, _) => RefreshState();

        _notifyIcon = new NotifyIcon
        {
            Icon = _idleIcon,
            Text = "HaloCap 光圈",
            ContextMenuStrip = _menu,
            Visible = true
        };
        _notifyIcon.MouseUp += (_, e) =>
        {
            if (e.Button != MouseButtons.Left)
            {
                return;
            }

            if (_isRecording)
            {
                Raise(StopRecordingRequested);
            }
            else
            {
                Raise(OpenRequested);
            }
        };
        _notifyIcon.BalloonTipClicked += (_, _) => HandleBalloonAction();

        RefreshState();
    }

    public event EventHandler? OpenRequested;

    public event EventHandler? SettingsRequested;

    public event EventHandler? ExitRequested;

    public event EventHandler? StopRecordingRequested;

    public event Action<bool>? AutoStartToggleRequested;

    public Func<bool>? AutoStartStateProvider { get; set; }

    public void SetHotkeyText(string displayText)
    {
        _hotkeyDisplay = displayText;
        _hotkeyItem.Text = $"当前快捷键  {displayText}";
        UpdateTooltip();
    }

    public void SetRecordingState(bool recording)
    {
        if (_disposed)
        {
            return;
        }

        _isRecording = recording;
        _notifyIcon.Icon = recording ? _recordingIcon : _idleIcon;
        UpdateTooltip();
        ApplyRecordingMenuState();
    }

    public void RefreshState()
    {
        if (_disposed)
        {
            return;
        }

        var enabled = AutoStartStateProvider?.Invoke() ?? false;
        _autoStartItem.Text = enabled ? "开机自动启动   ✓" : "开机自动启动";
        ApplyRecordingMenuState();
    }

    public void ShowNotification(string title, string message, ToolTipIcon icon = ToolTipIcon.Info)
    {
        if (_disposed)
        {
            return;
        }

        _balloonActionPath = null;
        _notifyIcon.ShowBalloonTip(3500, title, message, icon);
    }

    public void ShowNotificationWithAction(
        string title,
        string message,
        string path,
        ToolTipIcon icon = ToolTipIcon.Info)
    {
        if (_disposed)
        {
            return;
        }

        _balloonActionPath = path;
        _notifyIcon.ShowBalloonTip(3500, title, message, icon);
    }

    private void UpdateTooltip()
    {
        var tooltip = _isRecording
            ? "HaloCap 光圈 · 正在录制"
            : _hotkeyDisplay.Length > 0
                ? $"HaloCap 光圈 · {_hotkeyDisplay}"
                : "HaloCap 光圈";

        _notifyIcon.Text = tooltip.Length <= 63 ? tooltip : tooltip[..63];
    }

    private void ApplyRecordingMenuState()
    {
        _stopRecordingItem.Visible = _isRecording;
        _stopRecordingSeparator.Visible = _isRecording;
        _openItem.Enabled = !_isRecording;
        _settingsItem.Enabled = !_isRecording;
        _autoStartItem.Enabled = !_isRecording;
        _exitItem.Enabled = true;
    }

    private void HandleBalloonAction()
    {
        var path = _balloonActionPath;
        _balloonActionPath = null;

        if (string.IsNullOrEmpty(path))
        {
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = "/select,\"" + path + "\"",
                UseShellExecute = true
            });
        }
        catch (Exception ex)
        {
            Log.Error("打开录制文件位置失败", ex);
        }
    }

    private static void Raise(EventHandler? handler) => handler?.Invoke(null, EventArgs.Empty);

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _notifyIcon.Visible = false;
        _notifyIcon.Dispose();
        _menu.Dispose();
        _openItemFont.Dispose();
        _menuFont.Dispose();
        _idleIcon.Dispose();
        _recordingIcon.Dispose();
    }

    private static Icon CreateTrayIcon(bool recording)
    {
        const int size = 32;
        using var bitmap = new Bitmap(size, size, PixelFormat.Format32bppArgb);
        using (var graphics = Graphics.FromImage(bitmap))
        {
            graphics.SmoothingMode = SmoothingMode.AntiAlias;
            graphics.Clear(DrawingColor.Transparent);

            var diskRect = new RectangleF(1.5f, 1.5f, size - 3f, size - 3f);
            using var diskBrush = new SolidBrush(DrawingColor.FromArgb(255, 20, 25, 34));
            graphics.FillEllipse(diskBrush, diskRect);

            using var borderPen = new Pen(DrawingColor.FromArgb(64, 255, 255, 255), 1f);
            graphics.DrawEllipse(borderPen, diskRect);

            var colors = new[]
            {
                DrawingColor.FromArgb(255, 108, 168, 255),
                DrawingColor.FromArgb(255, 176, 140, 255),
                DrawingColor.FromArgb(255, 255, 115, 115),
                DrawingColor.FromArgb(255, 245, 185, 99)
            };

            const float ringInset = 6.2f;
            var ringRect = new RectangleF(
                ringInset,
                ringInset,
                size - (ringInset * 2),
                size - (ringInset * 2));

            using var arcPen = new Pen(DrawingColor.White, 3.2f)
            {
                StartCap = LineCap.Round,
                EndCap = LineCap.Round
            };

            for (var i = 0; i < colors.Length; i++)
            {
                arcPen.Color = recording
                    ? DrawingColor.FromArgb(145, colors[i])
                    : colors[i];
                var centerAngle = -90 + (i * 90);
                graphics.DrawArc(arcPen, ringRect, centerAngle - 28, 56);
            }

            if (recording)
            {
                using var glowBrush = new SolidBrush(DrawingColor.FromArgb(70, 255, 90, 90));
                graphics.FillEllipse(glowBrush, (size / 2f) - 8f, (size / 2f) - 8f, 16f, 16f);

                using var recordBrush = new SolidBrush(DrawingColor.FromArgb(255, 255, 92, 92));
                graphics.FillEllipse(recordBrush, (size / 2f) - 4.5f, (size / 2f) - 4.5f, 9f, 9f);
            }
            else
            {
                using var centerBrush = new SolidBrush(DrawingColor.FromArgb(245, 255, 255, 255));
                graphics.FillEllipse(centerBrush, (size / 2f) - 1.8f, (size / 2f) - 1.8f, 3.6f, 3.6f);
            }
        }

        var iconHandle = bitmap.GetHicon();
        try
        {
            using var temporaryIcon = Icon.FromHandle(iconHandle);
            return (Icon)temporaryIcon.Clone();
        }
        finally
        {
            NativeMethods.DestroyIcon(iconHandle);
        }
    }

    private sealed class StopRecordingToolStripMenuItem : ToolStripMenuItem
    {
        public StopRecordingToolStripMenuItem(string text)
            : base(text)
        {
        }
    }

    private sealed class DarkMenuRenderer : ToolStripProfessionalRenderer
    {
        private static readonly DrawingColor TextNormal = DrawingColor.FromArgb(232, 237, 245);
        private static readonly DrawingColor TextDisabled = DrawingColor.FromArgb(110, 122, 138);
        private static readonly DrawingColor RecordingRed = DrawingColor.FromArgb(255, 124, 124);

        public DarkMenuRenderer()
            : base(new DarkColorTable())
        {
            RoundedEdges = false;
        }

        protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
        {
            if (!e.Item.Enabled)
            {
                e.TextColor = TextDisabled;
            }
            else if (e.Item is StopRecordingToolStripMenuItem)
            {
                e.TextColor = RecordingRed;
            }
            else
            {
                e.TextColor = TextNormal;
            }

            base.OnRenderItemText(e);
        }
    }

    private sealed class DarkColorTable : ProfessionalColorTable
    {
        private static readonly DrawingColor MenuBackground = DrawingColor.FromArgb(24, 28, 37);
        private static readonly DrawingColor Selection = DrawingColor.FromArgb(38, 46, 61);
        private static readonly DrawingColor Border = DrawingColor.FromArgb(58, 67, 82);
        private static readonly DrawingColor Separator = DrawingColor.FromArgb(45, 53, 67);

        public override DrawingColor ToolStripDropDownBackground => MenuBackground;
        public override DrawingColor ImageMarginGradientBegin => MenuBackground;
        public override DrawingColor ImageMarginGradientMiddle => MenuBackground;
        public override DrawingColor ImageMarginGradientEnd => MenuBackground;
        public override DrawingColor MenuBorder => Border;
        public override DrawingColor MenuItemBorder => MenuBackground;
        public override DrawingColor MenuItemSelected => Selection;
        public override DrawingColor MenuItemSelectedGradientBegin => Selection;
        public override DrawingColor MenuItemSelectedGradientEnd => Selection;
        public override DrawingColor SeparatorDark => Separator;
        public override DrawingColor SeparatorLight => Separator;
    }
}
