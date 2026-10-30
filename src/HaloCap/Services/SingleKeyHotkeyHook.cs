using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;
using HaloCap.Interop;

namespace HaloCap.Services;

/// <summary>
/// 常驻的键盘钩子，只接管字母/数字类单键快捷键。
/// 低级钩子先于输入法拿到按键，可避免中文输入法把单键吃成候选或字符。
/// </summary>
internal sealed class SingleKeyHotkeyHook : IDisposable
{
    private const int VkLShift = 0xA0;
    private const int VkRShift = 0xA1;
    private const int VkLControl = 0xA2;
    private const int VkRControl = 0xA3;
    private const int VkLMenu = 0xA4;
    private const int VkRMenu = 0xA5;
    private const int VkLWin = 0x5B;
    private const int VkRWin = 0x5C;
    private const uint LlkhfInjected = 0x10;

    private readonly NativeMethods.LowLevelProc _proc;
    private readonly HashSet<int> _suppressed = new();
    private Dictionary<int, Action> _actions = new();
    private IntPtr _hook;
    private bool _disposed;

    public SingleKeyHotkeyHook()
    {
        _proc = HookProc;
    }

    public Func<bool>? CanHandle { get; set; }

    public void Update(IReadOnlyDictionary<int, Action> actions)
    {
        _actions = new Dictionary<int, Action>(actions);

        if (_disposed)
        {
            return;
        }

        if (_actions.Count == 0)
        {
            Uninstall();
            return;
        }

        // 更新快捷键时强制重建，顺便修复可能已被系统移除的旧钩子。
        Uninstall();
        Install();
    }

    private void Install()
    {
        if (_hook != IntPtr.Zero)
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
            Log.Error($"安装单键快捷键钩子失败，错误码 {Marshal.GetLastWin32Error()}");
        }
    }

    private void Uninstall()
    {
        if (_hook != IntPtr.Zero)
        {
            NativeMethods.UnhookWindowsHookEx(_hook);
            _hook = IntPtr.Zero;
        }

        _suppressed.Clear();
    }

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
                    // 按住时的重复按下只吞掉，不重复触发。
                    if (_suppressed.Contains(virtualKey))
                    {
                        return new IntPtr(1);
                    }

                    if (!HasBlockingModifier() &&
                        _actions.TryGetValue(virtualKey, out var action) &&
                        (CanHandle?.Invoke() ?? true))
                    {
                        _suppressed.Add(virtualKey);
                        action();
                        return new IntPtr(1);
                    }
                }
                else if (message is NativeMethods.WM_KEYUP or NativeMethods.WM_SYSKEYUP)
                {
                    if (_suppressed.Remove(virtualKey))
                    {
                        return new IntPtr(1);
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("单键快捷键钩子回调异常", ex);
            }
        }

        return NativeMethods.CallNextHookEx(_hook, nCode, wParam, lParam);
    }

    private static bool HasBlockingModifier() =>
        IsDown(VkLShift) ||
        IsDown(VkRShift) ||
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
        if (_disposed || _actions.Count == 0)
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
