using System;
using System.Runtime.InteropServices;
using Windows.Graphics.DirectX.Direct3D11;

namespace HaloCap.Interop;

/// <summary>
/// WGC 需要的 WinRT 设备，同时保留创建它的 ID3D11Device 与立即上下文，
/// 录制时要用同一个设备做选区拷贝，否则跨设备纹理不能直接 CopySubresourceRegion。
/// </summary>
internal sealed class Direct3DDeviceHolder : IDisposable
{
    private bool _disposed;

    internal Direct3DDeviceHolder(IDirect3DDevice device, IntPtr nativeDevice, IntPtr nativeContext)
    {
        Device = device;
        NativeDevice = nativeDevice;
        NativeContext = nativeContext;
    }

    internal IDirect3DDevice Device { get; }

    internal IntPtr NativeDevice { get; }

    internal IntPtr NativeContext { get; }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;

        try
        {
            Device.Dispose();
        }
        catch
        {
            // WinRT 设备释放失败时仍需归还原生引用。
        }

        if (NativeContext != IntPtr.Zero)
        {
            Marshal.Release(NativeContext);
        }

        if (NativeDevice != IntPtr.Zero)
        {
            Marshal.Release(NativeDevice);
        }
    }
}
