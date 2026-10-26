using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.Services;

internal static class MonitorService
{
    public static List<ScreenMonitor> GetMonitors()
    {
        var monitors = new List<ScreenMonitor>();

        NativeMethods.MonitorEnumProc callback = (IntPtr handle, IntPtr hdc, ref NativeMethods.RECT rect, IntPtr data) =>
        {
            try
            {
                monitors.Add(CreateMonitor(handle, rect));
            }
            catch (Exception ex)
            {
                Log.Error("枚举显示器时发生异常", ex);
            }

            return true;
        };

        if (!NativeMethods.EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, callback, IntPtr.Zero))
        {
            Log.Error("EnumDisplayMonitors 失败");
            monitors.Clear();
        }

        GC.KeepAlive(callback);

        if (monitors.Count == 0)
        {
            monitors.Add(CreateFallbackMonitor());
        }

        return monitors;
    }

    public static ScreenMonitor GetMonitorAt(int x, int y)
    {
        var monitors = GetMonitors();
        var handle = NativeMethods.MonitorFromPoint(
            new NativeMethods.POINT(x, y),
            NativeMethods.MONITOR_DEFAULTTONEAREST);

        return monitors.FirstOrDefault(monitor => monitor.Handle == handle)
               ?? monitors.FirstOrDefault(monitor => monitor.IsPrimary)
               ?? monitors[0];
    }

    public static ScreenMonitor GetMonitorAtCursor()
    {
        if (!NativeMethods.GetCursorPos(out var point))
        {
            point = new NativeMethods.POINT(0, 0);
        }

        return GetMonitorAt(point.X, point.Y);
    }

    private static ScreenMonitor CreateMonitor(IntPtr handle, NativeMethods.RECT fallbackBounds)
    {
        var info = new NativeMethods.MONITORINFOEX
        {
            cbSize = Marshal.SizeOf<NativeMethods.MONITORINFOEX>(),
            szDevice = string.Empty
        };

        var hasInfo = NativeMethods.GetMonitorInfoEx(handle, ref info);
        var bounds = hasInfo
            ? PhysicalRect.FromNative(info.rcMonitor)
            : PhysicalRect.FromNative(fallbackBounds);
        var workArea = hasInfo
            ? PhysicalRect.FromNative(info.rcWork)
            : bounds;
        var isPrimary = hasInfo && (info.dwFlags & NativeMethods.MONITORINFOF_PRIMARY) != 0;

        var dpi = GetMonitorDpi(handle);
        return new ScreenMonitor
        {
            Handle = handle,
            DeviceName = info.szDevice ?? string.Empty,
            Bounds = bounds,
            WorkArea = workArea,
            Scale = dpi / 96.0,
            IsPrimary = isPrimary
        };
    }

    private static ScreenMonitor CreateFallbackMonitor()
    {
        var bounds = new PhysicalRect(0, 0, 1920, 1080);
        return new ScreenMonitor
        {
            Handle = IntPtr.Zero,
            DeviceName = "Fallback",
            Bounds = bounds,
            WorkArea = bounds,
            Scale = 1.0,
            IsPrimary = true
        };
    }

    private static uint GetMonitorDpi(IntPtr monitor)
    {
        uint dpiX = 0;
        uint dpiY = 0;

        var result = NativeMethods.GetDpiForMonitor(
            monitor,
            NativeMethods.MonitorDpiType.EffectiveDpi,
            out dpiX,
            out dpiY);

        return result == 0 && dpiX > 0 ? dpiX : 96;
    }

    public static void ActivateWindow(IntPtr handle)
    {
        if (handle == IntPtr.Zero)
        {
            return;
        }

        try
        {
            var foreground = NativeMethods.GetForegroundWindow();
            var currentThread = NativeMethods.GetCurrentThreadId();
            var foregroundThread = foreground == IntPtr.Zero
                ? 0
                : NativeMethods.GetWindowThreadProcessId(foreground, IntPtr.Zero);
            var attached = false;

            if (foregroundThread != 0 && foregroundThread != currentThread)
            {
                attached = NativeMethods.AttachThreadInput(currentThread, foregroundThread, true);
            }

            try
            {
                NativeMethods.SetForegroundWindow(handle);
            }
            finally
            {
                if (attached)
                {
                    NativeMethods.AttachThreadInput(currentThread, foregroundThread, false);
                }
            }
        }
        catch (Exception ex)
        {
            Log.Error("激活前台窗口失败", ex);
        }
    }

    public static void RestoreForegroundWindow(IntPtr handle) => ActivateWindow(handle);
}
