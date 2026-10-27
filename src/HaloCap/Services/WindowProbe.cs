using System;
using System.Runtime.InteropServices;
using System.Text;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.Services;

/// <summary>
/// 在截图遮罩下方寻找光标处的顶层窗口，用于单击选区时直接选中窗口区域。
/// 遮罩本身位于最上层，因此不能用 WindowFromPoint，改为按 Z 序枚举。
/// </summary>
internal static class WindowProbe
{
    private const int MaxClassName = 64;
    private const int MinimumRegionSize = 8;

    public static PhysicalRect? FindRegionUnderPoint(
        int screenX,
        int screenY,
        PhysicalRect monitorBounds,
        IntPtr excludedWindow)
    {
        var match = IntPtr.Zero;

        NativeMethods.EnumWindowsProc callback = (hwnd, _) =>
        {
            if (hwnd == excludedWindow || !IsCandidate(hwnd, screenX, screenY))
            {
                return true;
            }

            match = hwnd;
            return false;
        };

        NativeMethods.EnumWindows(callback, IntPtr.Zero);
        GC.KeepAlive(callback);

        if (match == IntPtr.Zero || !TryGetFrame(match, out var frame))
        {
            return null;
        }

        var local = ScreenGeometry.Intersect(
            ScreenGeometry.ToLocal(PhysicalRect.FromNative(frame), monitorBounds),
            new PhysicalRect(0, 0, monitorBounds.Width, monitorBounds.Height));

        return local.Width >= MinimumRegionSize && local.Height >= MinimumRegionSize
            ? local
            : null;
    }

    private static bool IsCandidate(IntPtr hwnd, int screenX, int screenY)
    {
        if (!NativeMethods.IsWindowVisible(hwnd))
        {
            return false;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(hwnd, NativeMethods.GWL_EXSTYLE).ToInt64();

        if ((exStyle & NativeMethods.WS_EX_TRANSPARENT) != 0)
        {
            return false;
        }

        if (!NativeMethods.GetWindowRect(hwnd, out var frame))
        {
            return false;
        }

        if (screenX < frame.Left ||
            screenX >= frame.Right ||
            screenY < frame.Top ||
            screenY >= frame.Bottom)
        {
            return false;
        }

        return frame.Width >= MinimumRegionSize &&
               frame.Height >= MinimumRegionSize &&
               !IsCloaked(hwnd) &&
               !IsDesktopShell(hwnd);
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

    private static bool TryGetFrame(IntPtr hwnd, out NativeMethods.RECT rect)
    {
        if (NativeMethods.DwmGetWindowAttribute(
                hwnd,
                NativeMethods.DWMWA_EXTENDED_FRAME_BOUNDS,
                out rect,
                Marshal.SizeOf<NativeMethods.RECT>()) == 0 &&
            rect.Width > 0 &&
            rect.Height > 0)
        {
            return true;
        }

        return NativeMethods.GetWindowRect(hwnd, out rect);
    }

    private static bool IsDesktopShell(IntPtr hwnd)
    {
        var buffer = new StringBuilder(MaxClassName);
        var length = NativeMethods.GetClassName(hwnd, buffer, buffer.Capacity);

        if (length <= 0)
        {
            return false;
        }

        var name = buffer.ToString();
        return name is "Progman" or "WorkerW";
    }
}
