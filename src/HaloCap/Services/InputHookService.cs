using System;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;
using HaloCap.Interop;

namespace HaloCap.Services;

/// <summary>
/// 仅在圆盘显示期间安装的全局输入钩子，用于在不抢焦点的情况下接收 Esc、数字键与点击。
/// </summary>
internal sealed class InputHookService : IDisposable
{
    private readonly NativeMethods.LowLevelProc _keyboardProc;
    private readonly NativeMethods.LowLevelProc _mouseProc;
    private IntPtr _keyboardHook;
    private IntPtr _mouseHook;
    private bool _disposed;

    public InputHookService()
    {
        _keyboardProc = KeyboardHookProc;
        _mouseProc = MouseHookProc;
    }

    public Func<int, bool>? KeyDown { get; set; }

    public Action<int, int>? MouseMove { get; set; }

    public Func<int, int, bool, bool>? MouseDown { get; set; }

    public Func<int, int, bool>? MouseUp { get; set; }

    public void Install()
    {
        if (_disposed || _keyboardHook != IntPtr.Zero || _mouseHook != IntPtr.Zero)
        {
            return;
        }

        var moduleHandle = NativeMethods.GetModuleHandle(null);

        _keyboardHook = NativeMethods.SetWindowsHookEx(
            NativeMethods.WH_KEYBOARD_LL,
            _keyboardProc,
            moduleHandle,
            0);

        if (_keyboardHook == IntPtr.Zero)
        {
            Log.Error($"安装键盘钩子失败，错误码 {Marshal.GetLastWin32Error()}");
        }

        _mouseHook = NativeMethods.SetWindowsHookEx(
            NativeMethods.WH_MOUSE_LL,
            _mouseProc,
            moduleHandle,
            0);

        if (_mouseHook == IntPtr.Zero)
        {
            Log.Error($"安装鼠标钩子失败，错误码 {Marshal.GetLastWin32Error()}");
        }

        if (_keyboardHook == IntPtr.Zero || _mouseHook == IntPtr.Zero)
        {
            Uninstall();
        }
    }

    public void Uninstall()
    {
        if (_keyboardHook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_keyboardHook);
            _keyboardHook = IntPtr.Zero;
        }

        if (_mouseHook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_mouseHook);
            _mouseHook = IntPtr.Zero;
        }
    }

    private IntPtr KeyboardHookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            try
            {
                var message = wParam.ToInt32();
                var data = Marshal.PtrToStructure<NativeMethods.KBDLLHOOKSTRUCT>(lParam);
                var virtualKey = (int)data.vkCode;

                if (message is NativeMethods.WM_KEYDOWN or NativeMethods.WM_SYSKEYDOWN)
                {
                    if (KeyDown?.Invoke(virtualKey) == true)
                    {
                        return new IntPtr(1);
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("键盘钩子回调异常", ex);
            }
        }

        return NativeMethods.CallNextHookEx(_keyboardHook, nCode, wParam, lParam);
    }

    private IntPtr MouseHookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            try
            {
                var message = wParam.ToInt32();
                var data = Marshal.PtrToStructure<NativeMethods.MSLLHOOKSTRUCT>(lParam);

                switch (message)
                {
                    case NativeMethods.WM_MOUSEMOVE:
                        MouseMove?.Invoke(data.pt.X, data.pt.Y);
                        break;

                    case NativeMethods.WM_LBUTTONDOWN:
                        if (MouseDown?.Invoke(data.pt.X, data.pt.Y, true) == true)
                        {
                            return new IntPtr(1);
                        }
                        break;

                    case NativeMethods.WM_RBUTTONDOWN:
                        if (MouseDown?.Invoke(data.pt.X, data.pt.Y, false) == true)
                        {
                            return new IntPtr(1);
                        }
                        break;

                    case NativeMethods.WM_LBUTTONUP:
                    case NativeMethods.WM_RBUTTONUP:
                        if (MouseUp?.Invoke(data.pt.X, data.pt.Y) == true)
                        {
                            return new IntPtr(1);
                        }
                        break;
                }
            }
            catch (Exception ex)
            {
                Log.Error("鼠标钩子回调异常", ex);
            }
        }

        return NativeMethods.CallNextHookEx(_mouseHook, nCode, wParam, lParam);
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        Uninstall();
    }
}
