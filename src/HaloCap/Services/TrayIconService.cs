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
    private readonly Icon _idleIcon;
    private readonly Icon _recordingIcon;
    private string _hotkeyDisplay = string.Empty;
    private string? _balloonActionPath;
    private bool _isRecording;
    private bool _disposed;

    public TrayIconService()
    {
        _idleIcon = CreateTrayIcon(false);
        _recordingIcon = CreateTrayIcon(true);

        _notifyIcon = new NotifyIcon
        {
            Icon = _idleIcon,
            Text = "HaloCap 光圈",
            Visible = true
        };

        _notifyIcon.MouseUp += (_, e) =>
        {
            if (e.Button == MouseButtons.Right)
            {
                Raise(MenuRequested);
                return;
            }

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

        UpdateTooltip();
    }

    public event EventHandler? MenuRequested;

    public event EventHandler? OpenRequested;

    public event EventHandler? StopRecordingRequested;

    public void SetHotkeyText(string displayText)
    {
        _hotkeyDisplay = displayText;
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
            ? "HaloCap · 正在录制"
            : _hotkeyDisplay.Length > 0
                ? $"HaloCap · {_hotkeyDisplay}"
                : "HaloCap 光圈";

        _notifyIcon.Text = tooltip.Length <= 63 ? tooltip : tooltip[..63];
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
}
