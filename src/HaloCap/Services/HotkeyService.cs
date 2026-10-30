using System;
using System.Runtime.InteropServices;
using System.Windows.Input;
using System.Windows.Interop;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.Services;

internal sealed class HotkeyService : IDisposable
{
    private const int HotkeyId = 0x4843;

    private readonly HwndSource _messageSource;
    private bool _isRegistered;
    private bool _disposed;

    public HotkeyService()
    {
        var parameters = new HwndSourceParameters("HaloCap.HotkeySink")
        {
            Width = 0,
            Height = 0,
            WindowStyle = 0,
            ExtendedWindowStyle = NativeMethods.WS_EX_TOOLWINDOW
        };

        _messageSource = new HwndSource(parameters);
        _messageSource.AddHook(WndProc);
    }

    public event EventHandler? Pressed;

    public HotkeyDefinition? Current { get; private set; }

    public bool TryChange(HotkeyDefinition hotkey, out string? error)
    {
        error = null;

        if (!hotkey.IsValid)
        {
            error = hotkey.IsSingleKey
                ? "单键快捷键只支持 F1–F24、字母、数字、小键盘数字与翻页键，请换一个键或加上 Ctrl / Alt / Shift。"
                : "这个组合用不了，请换一个普通按键作为主键。";
            return false;
        }

        if (_isRegistered && Current is not null &&
            Current.Modifiers == hotkey.Modifiers && Current.Key == hotkey.Key)
        {
            return true;
        }

        var previous = Current;
        UnregisterCore();

        if (RegisterCore(hotkey, out error))
        {
            Current = hotkey;
            return true;
        }

        if (previous is not null)
        {
            if (RegisterCore(previous, out var restoreError))
            {
                Current = previous;
            }
            else
            {
                Current = null;
                Log.Error($"恢复原快捷键失败：{restoreError}");
                error = $"{error} 原快捷键也未能恢复，请在设置中重新指定。";
            }
        }
        else
        {
            Current = null;
        }

        return false;
    }

    public bool ReRegister(out string? error)
    {
        error = null;

        if (Current is null)
        {
            return false;
        }

        var hotkey = Current;
        UnregisterCore();
        return RegisterCore(hotkey, out error);
    }

    private bool RegisterCore(HotkeyDefinition hotkey, out string? error)
    {
        error = null;
        var modifiers = ToNativeModifiers(hotkey.Modifiers) | NativeMethods.MOD_NOREPEAT;
        var virtualKey = (uint)hotkey.VirtualKey;

        if (!NativeMethods.RegisterHotKey(_messageSource.Handle, HotkeyId, modifiers, virtualKey))
        {
            var errorCode = Marshal.GetLastWin32Error();
            error = errorCode == NativeMethods.ErrorHotkeyAlreadyRegistered
                ? $"「{hotkey.DisplayText}」已被其他程序占用，换一个组合键试试。"
                : $"无法注册快捷键「{hotkey.DisplayText}」（错误码 {errorCode}）。";
            Log.Error(error);
            return false;
        }

        _isRegistered = true;
        return true;
    }

    private void UnregisterCore()
    {
        if (!_isRegistered)
        {
            return;
        }

        NativeMethods.UnregisterHotKey(_messageSource.Handle, HotkeyId);
        _isRegistered = false;
    }

    private IntPtr WndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == NativeMethods.WM_HOTKEY && wParam.ToInt32() == HotkeyId)
        {
            handled = true;
            try
            {
                Pressed?.Invoke(this, EventArgs.Empty);
            }
            catch (Exception ex)
            {
                Log.Error("处理全局热键时发生异常", ex);
            }
        }

        return IntPtr.Zero;
    }

    private static uint ToNativeModifiers(ModifierKeys modifiers)
    {
        uint value = 0;
        if ((modifiers & ModifierKeys.Alt) != 0) value |= NativeMethods.MOD_ALT;
        if ((modifiers & ModifierKeys.Control) != 0) value |= NativeMethods.MOD_CONTROL;
        if ((modifiers & ModifierKeys.Shift) != 0) value |= NativeMethods.MOD_SHIFT;
        if ((modifiers & ModifierKeys.Windows) != 0) value |= NativeMethods.MOD_WIN;
        return value;
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        UnregisterCore();
        _messageSource.RemoveHook(WndProc);
        _messageSource.Dispose();
    }
}
