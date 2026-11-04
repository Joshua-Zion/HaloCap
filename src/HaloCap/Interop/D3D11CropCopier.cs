using System;
using System.Runtime.InteropServices;
using HaloCap.Models;

namespace HaloCap.Interop;

/// <summary>
/// 直接照 d3d11.h 的 vtable 顺序手写的 D3D11 调用：把 WGC 帧纹理里的选区拷到
/// CPU 可读的暂存纹理，避免整帧（4K 约 33MB）逐次搬运到托管数组。
/// </summary>
internal sealed class D3D11CropCopier : IDisposable
{
    private const uint CpuAccessRead = 0x00020000;
    private const uint UsageStaging = 3;
    private const uint FormatB8G8R8A8Unorm = 87;
    private const uint MapTypeRead = 1;
    private const int SlotCreateTexture2D = 5;
    private const int SlotMap = 14;
    private const int SlotUnmap = 15;
    private const int SlotCopySubresourceRegion = 46;

    private readonly IntPtr _device;
    private readonly IntPtr _context;
    private IntPtr _staging;
    private int _stagingWidth;
    private int _stagingHeight;
    private bool _disposed;

    public D3D11CropCopier(IntPtr device, IntPtr context)
    {
        if (device == IntPtr.Zero || context == IntPtr.Zero)
        {
            throw new ArgumentException("D3D11 设备或上下文为空。");
        }

        _device = device;
        _context = context;
    }

    public void Copy(
        IntPtr sourceTexture,
        PhysicalRect region,
        byte[] destination,
        int destinationStride)
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(D3D11CropCopier));
        }

        var width = region.Width;
        var height = region.Height;

        if (width <= 0 || height <= 0)
        {
            throw new ArgumentException("拷贝区域尺寸无效。");
        }

        if (destinationStride < (width * 4))
        {
            throw new ArgumentException("目标步幅小于区域宽度。", nameof(destinationStride));
        }

        if (destination.Length < ((height - 1) * destinationStride) + (width * 4))
        {
            throw new ArgumentException("目标缓冲区不足以容纳该区域。", nameof(destination));
        }

        EnsureStaging(width, height);

        var box = new D3D11Box
        {
            Left = (uint)region.Left,
            Top = (uint)region.Top,
            Front = 0,
            Right = (uint)region.Right,
            Bottom = (uint)region.Bottom,
            Back = 1
        };

        Invoke<CopySubresourceRegionDelegate>(_context, SlotCopySubresourceRegion)(
            _context,
            _staging,
            0,
            0,
            0,
            0,
            sourceTexture,
            0,
            ref box);

        var hr = Invoke<MapDelegate>(_context, SlotMap)(
            _context,
            _staging,
            0,
            MapTypeRead,
            0,
            out var mapped);

        if (hr < 0)
        {
            throw new COMException($"锁定暂存纹理失败（HRESULT 0x{hr:X8}）。", hr);
        }

        try
        {
            var rowBytes = width * 4;
            var sourceStride = (int)mapped.RowPitch;
            var lastRowOffset = ((height - 1) * destinationStride) + rowBytes;

            if (sourceStride == destinationStride && lastRowOffset <= destination.Length)
            {
                Marshal.Copy(mapped.pData, destination, 0, lastRowOffset);
            }
            else
            {
                for (var row = 0; row < height; row++)
                {
                    Marshal.Copy(
                        IntPtr.Add(mapped.pData, row * sourceStride),
                        destination,
                        row * destinationStride,
                        rowBytes);
                }
            }
        }
        finally
        {
            Invoke<UnmapDelegate>(_context, SlotUnmap)(_context, _staging, 0);
        }
    }

    private void EnsureStaging(int width, int height)
    {
        if (_staging != IntPtr.Zero && _stagingWidth == width && _stagingHeight == height)
        {
            return;
        }

        ReleaseStaging();

        var description = new D3D11Texture2DDescription
        {
            Width = (uint)width,
            Height = (uint)height,
            MipLevels = 1,
            ArraySize = 1,
            Format = FormatB8G8R8A8Unorm,
            SampleCount = 1,
            SampleQuality = 0,
            Usage = UsageStaging,
            BindFlags = 0,
            CPUAccessFlags = CpuAccessRead,
            MiscFlags = 0
        };

        var hr = Invoke<CreateTexture2DDelegate>(_device, SlotCreateTexture2D)(
            _device,
            ref description,
            IntPtr.Zero,
            out var texture);

        if (hr < 0)
        {
            throw new COMException(
                $"创建 {width}x{height} 暂存纹理失败（HRESULT 0x{hr:X8}）。",
                hr);
        }

        _staging = texture;
        _stagingWidth = width;
        _stagingHeight = height;
    }

    private void ReleaseStaging()
    {
        if (_staging == IntPtr.Zero)
        {
            return;
        }

        try
        {
            Invoke<ReleaseDelegate>(_staging, 2)(_staging);
        }
        catch
        {
            // 释放阶段的异常不能影响录制收尾。
        }

        _staging = IntPtr.Zero;
        _stagingWidth = 0;
        _stagingHeight = 0;
    }

    private static T Invoke<T>(IntPtr comObject, int slot) where T : class
    {
        if (comObject == IntPtr.Zero)
        {
            throw new InvalidOperationException("COM 对象尚未初始化。");
        }

        var vtable = Marshal.ReadIntPtr(comObject);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        ReleaseStaging();
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct D3D11Texture2DDescription
    {
        public uint Width;
        public uint Height;
        public uint MipLevels;
        public uint ArraySize;
        public uint Format;
        public uint SampleCount;
        public uint SampleQuality;
        public uint Usage;
        public uint BindFlags;
        public uint CPUAccessFlags;
        public uint MiscFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct D3D11Box
    {
        public uint Left;
        public uint Top;
        public uint Front;
        public uint Right;
        public uint Bottom;
        public uint Back;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MappedSubresource
    {
        public IntPtr pData;
        public uint RowPitch;
        public uint DepthPitch;
    }

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate uint ReleaseDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int CreateTexture2DDelegate(
        IntPtr self,
        ref D3D11Texture2DDescription description,
        IntPtr initialData,
        out IntPtr texture);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void CopySubresourceRegionDelegate(
        IntPtr self,
        IntPtr destination,
        uint destinationSubresource,
        uint destinationX,
        uint destinationY,
        uint destinationZ,
        IntPtr source,
        uint sourceSubresource,
        ref D3D11Box sourceBox);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int MapDelegate(
        IntPtr self,
        IntPtr resource,
        uint subresource,
        uint mapType,
        uint flags,
        out MappedSubresource mapped);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void UnmapDelegate(IntPtr self, IntPtr resource, uint subresource);
}
