using System;
using System.Runtime.InteropServices;

namespace HaloCap.Spikes3;

/// <summary>
/// 离屏渲染用的手写 interop。槽位来源：D3D11 与产品里已在跑的 D3D11CropCopier 一致
/// （CreateTexture2D=5、Map=14、Unmap=15），CopyResource=47、Flush=111 按 d3d11.h 数出；
/// D2D 的绝对槽位由 SDK 头 sdk_d2d1.h / sdk_d2d1_1.h 的继承链组合得出：
/// ID2D1Resource(3) → ID2D1RenderTarget(4..56) → ID2D1DeviceContext(57..91)。
/// </summary>
internal static class Com
{
    // ID3D11Device
    public const int SlotDeviceCreateTexture2D = 5;

    // ID3D11DeviceContext
    public const int SlotContextMap = 14;
    public const int SlotContextUnmap = 15;
    public const int SlotContextCopyResource = 47;
    public const int SlotContextFlush = 111;

    // ID2D1Factory（继承 IUnknown，自有方法 3..16）
    public const int SlotFactoryCreateDxgiSurfaceRenderTarget = 15;

    // ID2D1RenderTarget 自有（含 CreateBitmap）
    public const int SlotRtCreateBitmap = 4;

    // 继承自 ID2D1RenderTarget
    public const int SlotRtCreateSolidColorBrush = 8;
    public const int SlotRtDrawLine = 15;
    public const int SlotRtDrawRectangle = 16;
    public const int SlotRtFillRectangle = 17;
    public const int SlotRtDrawBitmap = 26;
    public const int SlotRtDrawText = 27;
    public const int SlotRtSetAntialiasMode = 32;
    public const int SlotRtFlush = 42;
    public const int SlotRtClear = 47;
    public const int SlotRtBeginDraw = 48;
    public const int SlotRtEndDraw = 49;

    // IDWriteFactory
    public const int SlotDWriteCreateTextFormat = 15;

    // DXGI_FORMAT / D2D1_ALPHA_MODE / 选项常量
    public const uint DxgiFormatB8G8R8A8Unorm = 87;
    public const uint D2dAlphaModePremultiplied = 1;
    public const uint D2dBitmapInterpolationLinear = 1;
    public const uint D2dAntialiasPerPrimitive = 0;
    public const uint D2dDrawTextOptionsNone = 0;
    public const uint DwriteMeasuringNatural = 0;
    public const uint DwriteWeightNormal = 400;
    public const uint DwriteStyleNormal = 0;
    public const uint DwriteStretchNormal = 5;
    public const uint DwriteFactoryShared = 0;
    public const uint D2dFactorySingleThreaded = 0;
    public const uint D3d11UsageStaging = 3;
    public const uint D3d11UsageDefault = 0;
    public const uint D3d11BindRenderTarget = 0x20;
    public const uint D3d11BindShaderResource = 0x8;
    public const uint D3d11MapRead = 1;
    public const uint D3dDriverTypeHardware = 1;
    public const uint D3d11CreateDeviceBgraSupport = 0x20;
    public const uint D3d11SdkVersion = 7;

    public static readonly Guid IIDIDXGIDevice = new("54ec77fa-1377-44e6-8c32-88fd5f44c84c");
    public static readonly Guid IIDIDXGISurface = new("cafcb56c-6ac3-4889-bf47-9e23bbd260ec");
    public static readonly Guid IIDID2D1Factory1 = new("bb12d362-daee-4b9a-aa1d-14ba401cfa1f");
    public static readonly Guid IIDIDWriteFactory = new("b859ee5a-d838-4b5b-a2e8-1adc7d93db48");

    [StructLayout(LayoutKind.Sequential)]
    public struct ColorF
    {
        public float R;
        public float G;
        public float B;
        public float A;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RectF
    {
        public float Left;
        public float Top;
        public float Right;
        public float Bottom;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Point2F
    {
        public float X;
        public float Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct SizeU
    {
        public uint Width;
        public uint Height;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PixelFormat
    {
        public uint Format;
        public uint AlphaMode;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct BitmapProperties
    {
        public PixelFormat PixelFormat;
        public float DpiX;
        public float DpiY;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RenderTargetProperties
    {
        public uint Type;
        public PixelFormat PixelFormat;
        public float DpiX;
        public float DpiY;
        public uint Usage;
        public uint MinLevel;
    }

    /// <summary>D3D11_MAPPED_SUBRESOURCE：指针 + RowPitch + DepthPitch。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct MappedSubresource
    {
        public IntPtr Data;
        public uint RowPitch;
        public uint DepthPitch;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Texture2DDescription
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
        public uint CpuAccessFlags;
        public uint MiscFlags;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ReleaseDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int QueryInterfaceDelegate(IntPtr self, ref Guid riid, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CreateTexture2DDel(
        IntPtr self,
        ref Texture2DDescription description,
        IntPtr initialData,
        out IntPtr texture);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CopyResourceDelegate(IntPtr self, IntPtr dst, IntPtr src);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int MapDelegate(IntPtr self, IntPtr resource, uint subresource, uint mapType, uint flags, out MappedSubresource mapped);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int UnmapDelegate(IntPtr self, IntPtr resource, uint subresource);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int NoArgDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CreateDxgiSurfaceRenderTargetDelegate(
        IntPtr self,
        IntPtr surface,
        ref RenderTargetProperties properties,
        out IntPtr renderTarget);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int RtCreateBitmapDelegate(
        IntPtr self,
        SizeU size,
        IntPtr srcData,
        uint pitch,
        ref BitmapProperties properties,
        out IntPtr bitmap);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CreateBrushDelegate(
        IntPtr self,
        ref ColorF color,
        IntPtr brushProperties,
        out IntPtr brush);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int DrawLineDelegate(
        IntPtr self,
        Point2F point0,
        Point2F point1,
        IntPtr brush,
        float strokeWidth,
        IntPtr strokeStyle);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int DrawRectangleDelegate(
        IntPtr self,
        ref RectF rect,
        IntPtr brush,
        float strokeWidth,
        IntPtr strokeStyle);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int FillRectangleDelegate(IntPtr self, ref RectF rect, IntPtr brush);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int DrawBitmapDelegate(
        IntPtr self,
        IntPtr bitmap,
        IntPtr destinationRectangle,
        float opacity,
        uint interpolationMode,
        IntPtr sourceRectangle);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate void DrawTextDelegate(
        IntPtr self,
        string text,
        uint length,
        IntPtr textFormat,
        ref RectF layoutRect,
        IntPtr brush,
        uint options,
        uint measuringMode);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetAntialiasModeDelegate(IntPtr self, uint mode);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int FlushDelegate(IntPtr self, out ulong tag1, IntPtr tag2);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ClearDelegate(IntPtr self, ref ColorF color);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int EndDrawDelegate(IntPtr self, out ulong tag1, out ulong tag2);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CreateTextFormatDelegate(
        IntPtr self,
        [MarshalAs(UnmanagedType.LPWStr)] string fontFamily,
        IntPtr fontCollection,
        uint weight,
        uint style,
        uint stretch,
        float fontSize,
        [MarshalAs(UnmanagedType.LPWStr)] string locale,
        out IntPtr textFormat);

    public static T Invoke<T>(IntPtr comObject, int slot) where T : class
    {
        var vtable = Marshal.ReadIntPtr(comObject);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
    }

    public static int Release(IntPtr pointer)
    {
        if (pointer == IntPtr.Zero)
        {
            return 0;
        }

        return Invoke<ReleaseDelegate>(pointer, 2)(pointer);
    }

    public static int QueryInterface(IntPtr pointer, Guid iid, out IntPtr result)
    {
        var target = iid;
        return Invoke<QueryInterfaceDelegate>(pointer, 0)(pointer, ref target, out result);
    }

    public static string Hex(int hr) => "0x" + hr.ToString("X8");

    [DllImport("d3d11.dll", ExactSpelling = true)]
    public static extern int D3D11CreateDevice(
        IntPtr adapter,
        uint driverType,
        IntPtr software,
        uint flags,
        IntPtr featureLevels,
        uint featureLevelCount,
        uint sdkVersion,
        out IntPtr device,
        IntPtr obtainedFeatureLevel,
        out IntPtr context);

    [DllImport("d2d1.dll", ExactSpelling = true)]
    public static extern int D2D1CreateFactory(
        uint factoryType,
        ref Guid riid,
        IntPtr factoryOptions,
        out IntPtr factory);

    [DllImport("DWrite.dll", ExactSpelling = true)]
    public static extern int DWriteCreateFactory(
        uint factoryType,
        ref Guid riid,
        out IntPtr factory);
}
