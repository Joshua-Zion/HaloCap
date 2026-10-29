using System;
using System.Runtime.InteropServices;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;
using POINT = HaloCap.Interop.NativeMethods.POINT;

namespace HaloCap.UI;

internal sealed class RadialMenuController : IDisposable
{
    private const int VkEscape = 0x1B;
    private const int VkControl = 0x11;
    private const int VkMenu = 0x12;
    private const int VkShift = 0x10;
    private const int VkLWin = 0x5B;
    private const int VkRWin = 0x5C;
    private const int VkDigit1 = 0x31;
    private const int VkDigit4 = 0x34;
    private const int VkNumpad1 = 0x61;
    private const int VkNumpad4 = 0x64;
    private const double EdgeMarginDip = 10;

    private readonly RadialMenuWindow _window;
    private readonly InputHookService _hooks = new();
    private readonly Dispatcher _dispatcher;
    private readonly HwndSource? _messageSource;
    private bool _clipboardListenerRegistered;
    private bool _pinEnabled = true;
    private HotkeyDefinition _hotkey;
    private bool _hotkeyRegistered;
    private bool _isOpen;
    private bool _disposed;
    private int _showGeneration;
    private POINT _centerPx;
    private double _pixelScale = 1.0;
    private double _windowSizeDip = RadialMenuWindow.MenuSize;
    private int _windowSizePx;
    private RadialAction? _hovered;
    private bool _draggingCenter;
    private int _dragOffsetX;
    private int _dragOffsetY;
    private int _dragWindowSizePx;

    public RadialMenuController(HotkeyDefinition hotkey, RadialMenuSize menuSize)
    {
        _hotkey = hotkey;
        _dispatcher = Dispatcher.CurrentDispatcher;
        _window = new RadialMenuWindow();
        _messageSource = HwndSource.FromHwnd(_window.Handle);
        if (_messageSource is null)
        {
            Log.Error("无法获取光圈窗口消息源，剪贴板变化监听未注册");
        }
        else
        {
            _messageSource.AddHook(WndProc);
            try
            {
                _clipboardListenerRegistered = NativeMethods.AddClipboardFormatListener(_window.Handle);
                if (!_clipboardListenerRegistered)
                {
                    Log.Error($"注册光圈剪贴板变化监听失败，Win32 错误 {Marshal.GetLastWin32Error()}");
                }
            }
            catch (Exception ex)
            {
                Log.Error("注册光圈剪贴板变化监听时发生异常", ex);
            }
        }

        UpdateMenuSize(menuSize);

        _hooks.KeyDown = HandleKeyDown;
        _hooks.MouseMove = HandleMouseMove;
        _hooks.MouseDown = HandleMouseDown;
        _hooks.MouseUp = HandleMouseUp;
    }

    public event Action<RadialAction>? ActionSelected;

    public bool IsOpen => _isOpen;

    public Func<bool>? PinAvailabilityProvider { get; set; }

    public void RefreshActionAvailability()
    {
        if (_disposed)
        {
            return;
        }

        if (!_dispatcher.CheckAccess())
        {
            _dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(RefreshActionAvailability));
            return;
        }

        try
        {
            _pinEnabled = PinAvailabilityProvider?.Invoke() ?? true;
        }
        catch (Exception ex)
        {
            _pinEnabled = false;
            Log.Error("刷新光圈贴图可用状态时发生异常", ex);
        }

        _window.SetActionEnabled(RadialAction.Pin, _pinEnabled);
        if (!_pinEnabled && _hovered == RadialAction.Pin)
        {
            SetHover(null);
        }
    }

    private IntPtr WndProc(IntPtr hwnd, int message, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (message == NativeMethods.WM_CLIPBOARDUPDATE && _isOpen && !_disposed)
        {
            RefreshActionAvailability();
        }

        return IntPtr.Zero;
    }

    private bool IsActionEnabled(RadialAction action) => action != RadialAction.Pin || _pinEnabled;

    public void UpdateHotkey(HotkeyDefinition hotkey, bool isRegistered)
    {
        _hotkey = hotkey;
        _hotkeyRegistered = isRegistered;
    }

    public void UpdateMenuSize(RadialMenuSize size)
    {
        _windowSizeDip = RadialMenuWindow.GetWindowSizeDip(size);
        _window.ApplySize(size);
    }

    public void ToggleFromHotkey()
    {
        var cursor = GetCursorPoint();

        if (!_isOpen)
        {
            ShowMenu();
            return;
        }

        if (IsPointInsideDisk(cursor))
        {
            HideMenu();
            return;
        }

        // 光标已经离开光圈：把光圈移到光标的新位置重新展开。
        ShowMenu();
    }

    public void ShowFromTray()
    {
        if (!_isOpen)
        {
            ShowMenu();
        }
    }

    public void HideMenu()
    {
        if (!_isOpen)
        {
            return;
        }

        _isOpen = false;
        _draggingCenter = false;
        _showGeneration++;
        _hovered = null;
        _hooks.Uninstall();
        _window.PlayHideAnimation();
    }

    private void ShowMenu()
    {
        if (_disposed)
        {
            return;
        }

        RefreshActionAvailability();
        var cursor = GetCursorPoint();
        _isOpen = true;
        _draggingCenter = false;
        var generation = ++_showGeneration;

        _window.EnsureShownInvisible();
        PlaceWindow(cursor);

        // 等 WPF 处理完跨屏 DPI 变化后再最终定位，此时窗口仍完全透明。
        _dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (_disposed || !_isOpen || generation != _showGeneration)
                {
                    return;
                }

                PlaceWindow(cursor);
                _window.SetHover(null);
                _hooks.Install();
                _window.PlayShowAnimation();
            }));
    }

    private void PlaceWindow(POINT cursor)
    {
        var handle = _window.Handle;
        var monitor = NativeMethods.MonitorFromPoint(cursor, NativeMethods.MONITOR_DEFAULTTONEAREST);
        var monitorInfo = new NativeMethods.MONITORINFO
        {
            cbSize = Marshal.SizeOf<NativeMethods.MONITORINFO>()
        };

        if (!NativeMethods.GetMonitorInfo(monitor, ref monitorInfo))
        {
            monitorInfo.rcWork = new NativeMethods.RECT
            {
                Left = -100_000,
                Top = -100_000,
                Right = 100_000,
                Bottom = 100_000
            };
        }

        var scale = GetMonitorScale(monitor, handle);
        var placement = ComputePlacement(cursor, monitorInfo.rcWork, scale, _windowSizeDip);
        _windowSizePx = placement.Size;

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            placement.Left,
            placement.Top,
            placement.Size,
            placement.Size,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);

        if (!NativeMethods.GetWindowRect(handle, out var rect))
        {
            _centerPx = new POINT(cursor.X, cursor.Y);
            _pixelScale = ComputePixelScale(_windowSizeDip * scale, scale);
            return;
        }

        var actualDpiScale = rect.Width / _windowSizeDip;
        if (actualDpiScale > 0.1 && Math.Abs(actualDpiScale - scale) > 0.02)
        {
            placement = ComputePlacement(cursor, monitorInfo.rcWork, actualDpiScale, _windowSizeDip);
            NativeMethods.SetWindowPos(
                handle,
                NativeMethods.HWND_TOPMOST,
                placement.Left,
                placement.Top,
                placement.Size,
                placement.Size,
                NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
            NativeMethods.GetWindowRect(handle, out rect);
        }

        if (rect.Width > 0)
        {
            _windowSizePx = rect.Width;
        }

        _centerPx = new POINT((rect.Left + rect.Right) / 2, (rect.Top + rect.Bottom) / 2);
        _pixelScale = rect.Width > 0
            ? ComputePixelScale(rect.Width, rect.Width / _windowSizeDip)
            : ComputePixelScale(_windowSizeDip * scale, scale);
    }

    /// <summary>
    /// 光圈设计画布只占窗口内部的矩形，四周还有 MenuPadding 的透明边距；
    /// 命中半径必须按内容区域换算，不能直接用窗口宽度。
    /// </summary>
    private double ComputePixelScale(double windowWidthPx, double dpiScale)
    {
        var contentWidth = windowWidthPx - (2 * RadialMenuWindow.MenuPadding * dpiScale);
        return contentWidth > 0
            ? contentWidth / RadialMenuWindow.MenuSize
            : ((_windowSizeDip - (2 * RadialMenuWindow.MenuPadding)) * dpiScale) / RadialMenuWindow.MenuSize;
    }

    private static Placement ComputePlacement(
        POINT cursor,
        NativeMethods.RECT workArea,
        double dpiScale,
        double windowSizeDip)
    {
        var size = Math.Max(1, (int)Math.Round(windowSizeDip * dpiScale));
        var margin = Math.Max(1, (int)Math.Round(EdgeMarginDip * dpiScale));

        var desiredLeft = cursor.X - (size / 2);
        var desiredTop = cursor.Y - (size / 2);

        var minLeft = workArea.Left + margin;
        var maxLeft = workArea.Right - size - margin;
        var minTop = workArea.Top + margin;
        var maxTop = workArea.Bottom - size - margin;

        var left = maxLeft >= minLeft
            ? Math.Clamp(desiredLeft, minLeft, maxLeft)
            : workArea.Left;
        var top = maxTop >= minTop
            ? Math.Clamp(desiredTop, minTop, maxTop)
            : workArea.Top;

        return new Placement(left, top, size);
    }

    private static double GetMonitorScale(IntPtr monitor, IntPtr windowHandle)
    {
        uint dpiX = 0;
        uint dpiY = 0;

        var result = NativeMethods.GetDpiForMonitor(
            monitor,
            NativeMethods.MonitorDpiType.EffectiveDpi,
            out dpiX,
            out dpiY);

        if (result != 0 || dpiX == 0)
        {
            dpiX = NativeMethods.GetDpiForWindow(windowHandle);
        }

        if (dpiX == 0)
        {
            dpiX = 96;
        }

        return dpiX / 96.0;
    }

    private bool HandleKeyDown(int virtualKey)
    {
        if (!_isOpen)
        {
            return false;
        }

        if (virtualKey == VkEscape)
        {
            _dispatcher.BeginInvoke(DispatcherPriority.Input, new Action(HideMenu));
            return true;
        }

        if (TryGetShortcutAction(virtualKey, out var action))
        {
            // 快捷键本身用了 1-4 且修饰键完全匹配时，把这次按键交给 RegisterHotKey 处理“再按关闭”。
            if (_hotkeyRegistered &&
                virtualKey == _hotkey.VirtualKey &&
                AreRequiredModifiersDown(_hotkey.Modifiers))
            {
                return false;
            }

            _dispatcher.BeginInvoke(
                DispatcherPriority.Input,
                new Action(() => SelectAction(action)));
            return true;
        }

        return false;
    }

    private void HandleMouseMove(int x, int y)
    {
        if (!_isOpen)
        {
            return;
        }

        if (_draggingCenter)
        {
            DragCenter(new POINT(x, y));
            return;
        }

        UpdateHover(new POINT(x, y));
    }

    private bool HandleMouseDown(int x, int y, bool isLeftButton)
    {
        if (!_isOpen)
        {
            return false;
        }

        var point = new POINT(x, y);

        if (isLeftButton && IsPointInsideDeadZone(point))
        {
            BeginCenterDrag(point);
            return true;
        }

        _dispatcher.BeginInvoke(
            DispatcherPriority.Input,
            new Action(() => HandleClick(point)));
        return true;
    }

    private bool HandleMouseUp(int x, int y)
    {
        if (!_isOpen)
        {
            return false;
        }

        if (_draggingCenter)
        {
            _draggingCenter = false;
            UpdateHover(new POINT(x, y));
        }

        return true;
    }

    private void BeginCenterDrag(POINT point)
    {
        _draggingCenter = true;
        _dragOffsetX = _centerPx.X - point.X;
        _dragOffsetY = _centerPx.Y - point.Y;
        _dragWindowSizePx = Math.Max(1, _windowSizePx);
        SetHover(null);
    }

    private void DragCenter(POINT point)
    {
        var size = _dragWindowSizePx;
        var half = size / 2;

        // 呼出时才限制在可视范围内；拖动后允许任意移动，不再钳制到工作区。
        var left = point.X + _dragOffsetX - half;
        var top = point.Y + _dragOffsetY - half;

        NativeMethods.SetWindowPos(
            _window.Handle,
            NativeMethods.HWND_TOPMOST,
            left,
            top,
            0,
            0,
            NativeMethods.SWP_NOSIZE |
            NativeMethods.SWP_NOACTIVATE |
            NativeMethods.SWP_SHOWWINDOW);

        _centerPx = new POINT(left + half, top + half);
    }

    private bool IsPointInsideDisk(POINT point)
    {
        var dx = point.X - _centerPx.X;
        var dy = point.Y - _centerPx.Y;
        var radius = RadialMenuWindow.DiskRadius * _pixelScale;
        return ((dx * dx) + (dy * dy)) <= radius * radius;
    }

    private bool IsPointInsideDeadZone(POINT point)
    {
        var dx = point.X - _centerPx.X;
        var dy = point.Y - _centerPx.Y;
        var radius = RadialMenuWindow.DeadZoneRadius * _pixelScale;
        return ((dx * dx) + (dy * dy)) <= radius * radius;
    }

    private void HandleClick(POINT point)
    {
        if (!_isOpen)
        {
            return;
        }

        var dx = point.X - _centerPx.X;
        var dy = point.Y - _centerPx.Y;
        var distance = Math.Sqrt((dx * dx) + (dy * dy));

        if (distance < RadialMenuWindow.DeadZoneRadius * _pixelScale)
        {
            return;
        }

        if (distance > RadialMenuWindow.DiskRadius * _pixelScale)
        {
            HideMenu();
            return;
        }

        SelectAction(RadialActionInfo.All[GetSectorIndex(dx, dy)].Action);
    }

    private void UpdateHover(POINT point)
    {
        var dx = point.X - _centerPx.X;
        var dy = point.Y - _centerPx.Y;
        var distance = Math.Sqrt((dx * dx) + (dy * dy));

        if (distance < RadialMenuWindow.DeadZoneRadius * _pixelScale)
        {
            SetHover(null);
            return;
        }

        if (distance > RadialMenuWindow.DiskRadius * _pixelScale)
        {
            // 光标移出光圈后取消选中，避免松手误触上一块扇区。
            SetHover(null);
            return;
        }

        var action = RadialActionInfo.All[GetSectorIndex(dx, dy)].Action;
        SetHover(IsActionEnabled(action) ? action : null);
    }

    private void SetHover(RadialAction? action)
    {
        if (action.HasValue && !IsActionEnabled(action.Value))
        {
            action = null;
        }

        if (_hovered == action)
        {
            return;
        }

        _hovered = action;
        _window.SetHover(action);
    }

    private void SelectAction(RadialAction action)
    {
        if (!_isOpen)
        {
            return;
        }

        RefreshActionAvailability();
        if (!IsActionEnabled(action))
        {
            return;
        }

        HideMenu();

        try
        {
            ActionSelected?.Invoke(action);
        }
        catch (Exception ex)
        {
            Log.Error($"执行光圈动作 {action} 时发生异常", ex);
        }
    }

    private static int GetSectorIndex(double dx, double dy)
    {
        var angle = Math.Atan2(dx, -dy) * 180.0 / Math.PI;
        if (angle < 0)
        {
            angle += 360;
        }

        return (int)Math.Floor((angle + 45) / 90) % 4;
    }

    private static bool TryGetShortcutAction(int virtualKey, out RadialAction action)
    {
        switch (virtualKey)
        {
            case >= VkDigit1 and <= VkDigit4:
                action = (RadialAction)(virtualKey - VkDigit1);
                return true;

            case >= VkNumpad1 and <= VkNumpad4:
                action = (RadialAction)(virtualKey - VkNumpad1);
                return true;

            default:
                action = default;
                return false;
        }
    }

    private static bool AreRequiredModifiersDown(ModifierKeys modifiers)
    {
        if ((modifiers & ModifierKeys.Control) != 0 && !IsKeyDown(VkControl)) return false;
        if ((modifiers & ModifierKeys.Alt) != 0 && !IsKeyDown(VkMenu)) return false;
        if ((modifiers & ModifierKeys.Shift) != 0 && !IsKeyDown(VkShift)) return false;
        if ((modifiers & ModifierKeys.Windows) != 0 &&
            !IsKeyDown(VkLWin) &&
            !IsKeyDown(VkRWin))
        {
            return false;
        }

        return true;
    }

    private static POINT GetCursorPoint()
    {
        return NativeMethods.GetCursorPos(out var point)
            ? point
            : new POINT(0, 0);
    }

    private static bool IsKeyDown(int virtualKey)
    {
        return (NativeMethods.GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _isOpen = false;
        _showGeneration++;
        _hooks.Dispose();
        if (_clipboardListenerRegistered)
        {
            try
            {
                if (!NativeMethods.RemoveClipboardFormatListener(_window.Handle))
                {
                    Log.Error($"移除光圈剪贴板变化监听失败，Win32 错误 {Marshal.GetLastWin32Error()}");
                }
            }
            catch (Exception ex)
            {
                Log.Error("移除光圈剪贴板变化监听时发生异常", ex);
            }

            _clipboardListenerRegistered = false;
        }

        _messageSource?.RemoveHook(WndProc);
        _window.Close();
    }

    private readonly record struct Placement(int Left, int Top, int Size);
}
