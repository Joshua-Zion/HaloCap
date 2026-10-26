using System;
using System.Diagnostics;
using HaloCap.Diagnostics;
using Microsoft.Win32;

namespace HaloCap.Services;

internal sealed class AutoStartService
{
    private const string RunKeyPath = @"Software\Microsoft\Windows\CurrentVersion\Run";
    private const string ValueName = "HaloCap";

    private string ExecutablePath =>
        Environment.ProcessPath ??
        Process.GetCurrentProcess().MainModule?.FileName ??
        string.Empty;

    public bool IsEnabled()
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(RunKeyPath, false);
            var stored = key?.GetValue(ValueName) as string;
            if (string.IsNullOrWhiteSpace(stored))
            {
                return false;
            }

            return string.Equals(
                Unquote(stored),
                ExecutablePath,
                StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception ex)
        {
            Log.Error("读取开机自启状态失败", ex);
            return false;
        }
    }

    public void SetEnabled(bool enabled)
    {
        try
        {
            if (string.IsNullOrWhiteSpace(ExecutablePath))
            {
                throw new InvalidOperationException("无法确定 HaloCap 的可执行文件路径。");
            }

            using var key = Registry.CurrentUser.CreateSubKey(RunKeyPath, true)
                ?? throw new InvalidOperationException("无法打开开机启动注册表项。");

            if (enabled)
            {
                key.SetValue(ValueName, $"\"{ExecutablePath}\"", RegistryValueKind.String);
            }
            else
            {
                key.DeleteValue(ValueName, false);
            }
        }
        catch (Exception ex)
        {
            Log.Error($"更新开机自启状态失败（enabled={enabled}）", ex);
            throw new InvalidOperationException("无法更新开机自启设置，请检查注册表权限。", ex);
        }
    }

    private static string Unquote(string value)
    {
        var trimmed = value.Trim();
        if (trimmed.Length >= 2 && trimmed[0] == '"' && trimmed[^1] == '"')
        {
            return trimmed[1..^1];
        }

        return trimmed;
    }
}
