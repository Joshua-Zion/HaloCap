using System;
using System.Runtime.InteropServices;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.Services;

/// <summary>
/// 窗口录制需要的几何信息：WGC 的捕获表面对应 GetWindowRect（含不可见的缩放边框与阴影），
/// 而用户看到的窗口是 DWM 扩展边框，因此需要把两者的差当作裁剪偏移。
/// </summary>
internal readonly struct WindowCaptureTarget
{
    public WindowCaptureTarget(IntPtr handle, PhysicalRect surfaceRect, PhysicalRect visibleRect)
    {
        Handle = handle;
        SurfaceRect = surfaceRect;
        VisibleRect = visibleRect;
    }

    public IntPtr Handle { get; }

    /// <summary>屏幕坐标下的捕获表面尺寸，等于 WGC 的 item.Size。</summary>
    public PhysicalRect SurfaceRect { get; }

    /// <summary>屏幕坐标下用户可见的窗口区域。</summary>
    public PhysicalRect VisibleRect { get; }

    /// <summary>可见区域在捕获表面内的偏移与尺寸。</summary>
    public PhysicalRect CropInSurface => new(
        VisibleRect.Left - SurfaceRect.Left,
        VisibleRect.Top - SurfaceRect.Top,
        VisibleRect.Width,
        VisibleRect.Height);

    public bool IsUsable =>
        Handle != IntPtr.Zero &&
        SurfaceRect.Width >= 16 &&
        SurfaceRect.Height >= 16 &&
        !CropInSurface.IsEmpty;
}

internal static class WindowGeometry
{
    public static bool TryGet(IntPtr hwnd, out WindowCaptureTarget target)
    {
        target = default;

        if (hwnd == IntPtr.Zero ||
            !NativeMethods.IsWindow(hwnd) ||
            IsMinimized(hwnd) ||
            !NativeMethods.IsWindowVisible(hwnd))
        {
            return false;
        }

        if (!NativeMethods.GetWindowRect(hwnd, out var surfaceRect))
        {
            return false;
        }

        var surface = PhysicalRect.FromNative(surfaceRect);

        if (!TryGetVisibleRect(hwnd, out var visible))
        {
            return false;
        }

        target = new WindowCaptureTarget(hwnd, surface, visible);
        return target.IsUsable;
    }

    public static bool TryGetVisibleRect(IntPtr hwnd, out PhysicalRect visible)
    {
        visible = default;

        if (NativeMethods.DwmGetWindowAttribute(
                hwnd,
                NativeMethods.DWMWA_EXTENDED_FRAME_BOUNDS,
                out NativeMethods.RECT frame,
                Marshal.SizeOf<NativeMethods.RECT>()) == 0 &&
            frame.Width > 0 &&
            frame.Height > 0)
        {
            visible = PhysicalRect.FromNative(frame);
            return true;
        }

        if (NativeMethods.GetWindowRect(hwnd, out frame))
        {
            visible = PhysicalRect.FromNative(frame);
            return true;
        }

        return false;
    }

    public static bool IsMinimized(IntPtr hwnd) => NativeMethods.IsIconic(hwnd);

    /// <summary>把窗口中心点所在显示器作为录制浮层的定位依据。</summary>
    public static ScreenMonitor GetMonitor(WindowCaptureTarget target) =>
        MonitorService.GetMonitorAt(
            target.VisibleRect.Left + (target.VisibleRect.Width / 2),
            target.VisibleRect.Top + (target.VisibleRect.Height / 2));
}
