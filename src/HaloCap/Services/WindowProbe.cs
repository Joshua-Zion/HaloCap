using System;
using System.Runtime.InteropServices;
using System.Text;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.Services;

/// <summary>
/// 在截图遮罩下方寻找光标处的顶层窗口，用于启动时和单击选区时直接选中窗口区域。
/// 遮罩本身位于最上层，因此不能用 WindowFromPoint，改为按 Z 序枚举。
/// </summary>
internal static class WindowProbe
{
    private const int MaxClassName = 64;

    // 高亮候选至少要有这个尺寸；更小的顶层窗口通常是提示、输入法或阴影辅助窗。
    private const int MinimumRegionSize = 32;

    private const string PinWindowTitle = "HaloCap 贴图";

    public static PhysicalRect? FindRegionUnderPoint(
        int screenX,
        int screenY,
        PhysicalRect monitorBounds,
        IntPtr excludedWindow)
    {
        if (!TryFindCandidate(screenX, screenY, excludedWindow, out _, out var visible))
        {
            return null;
        }

        var local = ScreenGeometry.Intersect(
            ScreenGeometry.ToLocal(visible, monitorBounds),
            new PhysicalRect(0, 0, monitorBounds.Width, monitorBounds.Height));

        return IsCandidateSize(local) ? local : null;
    }

    /// <summary>
    /// 找到光标处最靠前的可录制窗口，窗口录制用它拿到句柄与可见区域。
    /// </summary>
    public static bool TryFindCandidate(
        int screenX,
        int screenY,
        IntPtr excludedWindow,
        out IntPtr handle,
        out PhysicalRect visibleRect)
    {
        handle = IntPtr.Zero;
        visibleRect = default;
        var found = false;
        IntPtr foundHandle = IntPtr.Zero;
        var foundRect = default(PhysicalRect);

        NativeMethods.EnumWindowsProc callback = (hwnd, _) =>
        {
            if (hwnd == excludedWindow ||
                IsOwnProcessWindow(hwnd) ||
                !IsCandidate(hwnd, screenX, screenY, out var frame))
            {
                return true;
            }

            foundHandle = hwnd;
            foundRect = frame;
            found = true;
            return false;
        };

        NativeMethods.EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);

        if (found)
        {
            handle = foundHandle;
            visibleRect = foundRect;
        }

        return found;
    }

    internal static bool IsCandidateSize(PhysicalRect region) =>
        region.Width >= MinimumRegionSize && region.Height >= MinimumRegionSize;

    private static bool IsCandidate(IntPtr hwnd, int screenX, int screenY, out PhysicalRect visible)
    {
        visible = default;

        if (!NativeMethods.IsWindowVisible(hwnd))
        {
            return false;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(hwnd, NativeMethods.GWL_EXSTYLE).ToInt64();

        if ((exStyle & NativeMethods.WS_EX_TRANSPARENT) != 0)
        {
            return false;
        }

        var className = ReadClassName(hwnd);
        if (className is "Progman" or "WorkerW" or "tooltips_class32" or "SysShadow"
            or "IME" or "MSCTFIME UI")
        {
            return false;
        }

        if (!WindowGeometry.TryGetVisibleRect(hwnd, out visible) ||
            visible.Width < MinimumRegionSize ||
            visible.Height < MinimumRegionSize)
        {
            return false;
        }

        if (!visible.Contains(screenX, screenY))
        {
            return false;
        }

        return !IsCloaked(hwnd);
    }

    private static bool IsOwnProcessWindow(IntPtr hwnd)
    {
        // 光圈、遮罩、录制框和托盘菜单不能成为待截取的目标；贴图是用户内容，仍可选中。
        NativeMethods.GetWindowThreadProcessId(hwnd, out var processId);
        if (processId != (uint)Environment.ProcessId)
        {
            return false;
        }

        return !string.Equals(ReadWindowText(hwnd), PinWindowTitle, StringComparison.Ordinal);
    }

    private static bool IsCloaked(IntPtr hwnd)
    {
        return NativeMethods.DwmGetWindowAttribute(
                   hwnd,
                   NativeMethods.DWMWA_CLOAKED,
                   out int cloaked,
                   sizeof(int)) == 0 &&
               cloaked != 0;
    }

    private static string ReadClassName(IntPtr hwnd)
    {
        var buffer = new StringBuilder(MaxClassName);
        return NativeMethods.GetClassName(hwnd, buffer, buffer.Capacity) > 0
            ? buffer.ToString()
            : string.Empty;
    }

    private static string ReadWindowText(IntPtr hwnd)
    {
        var buffer = new StringBuilder(MaxClassName);
        return NativeMethods.GetWindowText(hwnd, buffer, buffer.Capacity) > 0
            ? buffer.ToString()
            : string.Empty;
    }
}
