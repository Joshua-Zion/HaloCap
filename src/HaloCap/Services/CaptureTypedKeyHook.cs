using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;
using HaloCap.Interop;

namespace HaloCap.Services;

/// <summary>
/// 仅在截图选区会话期间安装的键盘钩子：把字母、数字、空格等可输入键先于输入法
/// 交给选区处理并吞掉，避免中文输入法弹出候选条。文字工具编辑时不拦截。
/// </summary>
internal sealed class CaptureTypedKeyHook : IDisposable
{
    private const uint LlkhfInjected = 0x10;
    private const int VkLControl = 0xA2;
    private const int VkRControl = 0xA3;
    private const int VkLMenu = 0xA4;
    private const int VkRMenu = 0xA5;
    private const int VkLWin = 0x5B;
    private const int VkRWin = 0x5C;

    private readonly NativeMethods.LowLevelProc _proc;
    private readonly HashSet<int> _down = new();
    private IntPtr _hook;
    private bool _disposed;

    public CaptureTypedKeyHook()
    {
        _proc = HookProc;
    }

    public Func<int, bool>? KeyDown { get; set; }

    public Func<bool>? CanHandle { get; set; }

    public void Install()
    {
        if (_disposed || _hook != IntPtr.Zero)
        {
            return;
        }

        var moduleHandle = NativeMethods.GetModuleHandle(null);
        _hook = NativeMethods.SetWindowsHookEx(
            NativeMethods.WH_KEYBOARD_LL,
            _proc,
            moduleHandle,
            0);

        if (_hook == IntPtr.Zero)
        {
            Log.Error($"安装选区键盘钩子失败，错误码 {Marshal.GetLastWin32Error()}");
        }
    }

    public void Uninstall()
    {
        if (_hook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }

        _down.Clear();
    }

    internal static bool IsTypingKey(int virtualKey) =>
        virtualKey is
            >= 0x30 and <= 0x39 or // 0-9
            >= 0x41 and <= 0x5A or // A-Z
            0x20 or                // Space
            >= 0x60 and <= 0x69 or // numpad 0-9
            >= 0xBA and <= 0xC0 or // ; = , - . /
            >= 0xDB and <= 0xDF;  // [ \ ] '

    private IntPtr HookProc(int nCode, IntPtr wParam, IntPtr lParam)
    {
        if (nCode >= 0)
        {
            try
            {
                var message = wParam.ToInt32();
                var data = Marshal.PtrToStructure<NativeMethods.KBDLLHOOKSTRUCT>(lParam);
                var virtualKey = (int)data.vkCode;

                if ((data.flags & LlkhfInjected) != 0)
                {
                    return NativeMethods.CallNextHookEx(_hook, nCode, wParam, lParam);
                }

                if (message is NativeMethods.WM_KEYDOWN or NativeMethods.WM_SYSKEYDOWN)
                {
                    if (!IsTypingKey(virtualKey) || HasBlockingModifier())
                    {
                        return NativeMethods.CallNextHookEx(_hook, nCode, wParam, lParam);
                    }

                    if (_down.Contains(virtualKey))
                    {
                        return new IntPtr(1);
                    }

                    if (CanHandle?.Invoke() == false)
                    {
                        return NativeMethods.CallNextHookEx(_hook, nCode, wParam, lParam);
                    }

                    if (KeyDown?.Invoke(virtualKey) == true)
                    {
                        _down.Add(virtualKey);
                        return new IntPtr(1);
                    }
                }
                else if (message is NativeMethods.WM_KEYUP or NativeMethods.WM_SYSKEYUP)
                {
                    if (_down.Remove(virtualKey))
                    {
                        return new IntPtr(1);
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("选区键盘钩子回调异常", ex);
            }
        }

        return NativeMethods.CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    private static bool HasBlockingModifier() =>
        IsDown(VkLControl) ||
        IsDown(VkRControl) ||
        IsDown(VkLMenu) ||
        IsDown(VkRMenu) ||
        IsDown(VkLWin) ||
        IsDown(VkRWin);

    private static bool IsDown(int virtualKey) =>
        (NativeMethods.GetAsyncKeyState(virtualKey) & 0x8000) != 0;

    public void Reinstall()
    {
        if (_disposed)
        {
            return;
        }

        Uninstall();
        Install();
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
