using System;
using System.Runtime.InteropServices;

namespace HaloCap.Spikes;

/// <summary>
/// S0 spike 用的手写 MF / D3D11 interop。槽位与常量全部按头文件核对：
/// IMFTransform 来自 mftransform.h，IMFAttributes/IMFActivate/IMFDXGIDeviceManager/IMFSample 来自 mfobjects.h，
/// ID3D11Device::CreateTexture2D 来自 d3d11.h（与产品里已在跑的 D3D11CropCopier 一致）。
/// </summary>
internal static class Native
{
    // ---- mftransform.h ----
    public const int MftMessageSetD3dManager = 0x2;
    public const int MftMessageNotifyStartOfStream = 0x10000003;
    public const uint MftSetTypeTestOnly = 0x1;
    public const uint MftProcessOutputDiscardWhenNoBuffer = 0x1;

    // ---- mfapi.h ----
    public const int MfVersion = 0x00020070;
    public const uint MftEnumFlagSyncMft = 0x1;
    public const uint MftEnumFlagHardware = 0x4;
    public const uint MftEnumFlagSortAndFilter = 0x40;

    public const uint MftOutputStreamProvidesSamples = 0x100;

    // ---- d3d11.h / dxgiformat.h ----
    public const uint D3dDriverTypeHardware = 1;
    public const uint D3d11CreateDeviceBgraSupport = 0x20;
    public const uint D3d11SdkVersion = 7;
    public const uint D3d11UsageDefault = 0;
    public const uint D3d11BindShaderResource = 0x8;
    public const uint DxgiFormatNv12 = 0x67;
    public const uint DxgiFormatB8G8R8A8Unorm = 87;

    // ---- vtable slots ----
    public const int SlotQueryInterface = 0;
    public const int SlotRelease = 2;

    public const int SlotAttributesGetUINT32 = 7;
    public const int SlotAttributesGetUINT64 = 8;
    public const int SlotAttributesGetGUID = 10;
    public const int SlotAttributesSetUINT32 = 21;
    public const int SlotAttributesSetUINT64 = 22;
    public const int SlotAttributesSetGUID = 24;
    public const int SlotAttributesSetUnknown = 27;
    public const int SlotAttributesGetCount = 30;
    public const int SlotAttributesGetItemByIndex = 31;
    public const int SlotAttributesCopyAllItems = 32;

    public const int SlotActivateActivateObject = 33;

    public const int SlotDxgiManagerResetDevice = 7;

    public const int SlotSampleSetSampleTime = 36;
    public const int SlotSampleSetSampleDuration = 38;
    public const int SlotSampleConvertToContiguousBuffer = 41;
    public const int SlotSampleAddBuffer = 42;

    public const int SlotMediaBufferGetCurrentLength = 5;
    public const int SlotMediaBufferSetCurrentLength = 6;

    public const int SlotDxgiBufferGetResource = 3;

    public const int SlotTransformGetStreamCount = 4;
    public const int SlotTransformGetInputStreamInfo = 6;
    public const int SlotTransformGetAttributes = 8;
    public const int SlotTransformGetOutputStreamInfo = 7;
    public const int SlotTransformGetInputAvailableType = 13;
    public const int SlotTransformGetOutputAvailableType = 14;
    public const int SlotTransformSetInputType = 15;
    public const int SlotTransformSetOutputType = 16;
    public const int SlotTransformProcessMessage = 23;
    public const int SlotTransformProcessInput = 24;
    public const int SlotTransformProcessOutput = 25;

    public const int SlotDeviceCreateTexture2D = 5;

    // IMFSinkWriter 槽位取自产品里已在跑的 MediaFoundationWriter.cs（真机验证过）。
    public const int SlotSinkWriterAddStream = 3;
    public const int SlotSinkWriterSetInputMediaType = 4;
    public const int SlotSinkWriterBeginWriting = 5;
    public const int SlotSinkWriterWriteSample = 6;
    public const int SlotSinkWriterFinishWriting = 11;
    public const int SlotSinkWriterGetServiceForStream = 12;

    // DXGI 槽位按 DirectN 从 SDK 头生成的声明顺序数出来：IUnknown(0-2) + IDXGIObject(3-6)
    // + IDXGIFactory(7-11) → EnumAdapters1 = 12；IDXGIAdapter(7-9) → GetDesc1 = 10。
    public const int SlotFactoryEnumAdapters1 = 12;
    public const int SlotAdapterGetDesc1 = 10;

    public static readonly Guid IIDIMFTransform = new("bf94c121-5b05-4e6f-8000-ba598961414d");
    public static readonly Guid IIDIMFMediaBuffer = new("045fa593-8799-42b8-bc8d-8968c6453507");
    public static readonly Guid IIDIMFDXGIBuffer = new("e7174cfa-1c9e-48b1-8866-626226bfc258");
    public static readonly Guid IIDID3D11Texture2D = new("6f15aaf2-d208-4e89-9ab4-489535d34f9c");
    public static readonly Guid IIDIDXGIFactory1 = new("770aae78-f26f-4dba-a829-253c83d1b387");
    public static readonly Guid IIDIDXGIAdapter1 = new("29038f61-3839-4626-91fd-086879011a05");

    // 这几个常量产品里已在用，值已被真机验证。
    public static readonly Guid MFMediaTypeVideo = new("73646976-0000-0010-8000-00AA00389B71");
    public static readonly Guid MFVideoFormatH264 = new("34363248-0000-0010-8000-00AA00389B71");
    public static readonly Guid MFVideoFormatRgb32 = new("00000016-0000-0010-8000-00AA00389B71");
    public static readonly Guid MFMTMajorType = new("48eba18e-f8c9-4687-bf11-0a74c9f96a8f");
    public static readonly Guid MFMTSubtype = new("f7e34c9a-42e8-4714-b74b-cb29d72c35e5");
    public static readonly Guid MFMTFrameSize = new("1652c33d-d6b2-4012-b834-72030849a37d");
    public static readonly Guid MFMTFrameRate = new("c459a2e8-3d2c-4e44-b132-fee5156c7bb0");
    public static readonly Guid MFMTAvgBitrate = new("20332624-fb0d-4d9e-bd0d-cbf6786c102e");
    public static readonly Guid MFMTInterlaceMode = new("e2724bb8-e676-4806-b4b2-a8d6efb44ccd");
    public static readonly Guid MFMTMpeg2Profile = new("ad76a80b-2d5c-4e0b-b375-64e520137036");
    public static readonly Guid MFMTDefaultStride = new("644b4e48-1e02-4516-b0eb-c01ca9d49ac6");
    public static readonly Guid MFMTSampleSize = new("dad3ab78-1990-408b-bce2-eba673dacc10");
    public static readonly Guid MFTCategoryVideoEncoder = new("f79eac7d-e545-4387-bdee-d647d7bde42a");

    // mfapi.h: DEFINE_MEDIATYPE_GUID(MFVideoFormat_NV12, FCC('NV12'))
    public static readonly Guid MFVideoFormatNv12 = new("3231564e-0000-0010-8000-00aa00389b71");

    // 后两个 GUID 产品里已在用（真机验证），MF_SINK_WRITER_D3D_MANAGER 由 SDK IDL 核对。
    public static readonly Guid MFReadWriteEnableHardwareTransforms = new("a634a91c-822b-41b9-a494-4de4643612b0");
    public static readonly Guid MFSinkWriterD3DManager = new("ec822da2-e1e9-4b29-a0d8-563c719f5269");
    public static readonly Guid MFSinkWriterDisableThrottling = new("08b845d8-2b74-4afe-9d53-be16d2d5ae4f");

    // 样本级 D3D11 标志，取自 DirectN（C# 版 MF API）的常量表。
    public static readonly Guid MFSaD3D11Aware = new("206b4fc8-fcf9-4c51-afe3-9764369e33a0");
    public static readonly Guid MFSAD3D11BindFlags = new("eacf97ad-065c-4408-bee3-fdcbfd128be2");

    [StructLayout(LayoutKind.Sequential)]
    public struct MftRegisterTypeInfo
    {
        public Guid MajorType;
        public Guid Subtype;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MftOutputStreamInfo
    {
        public uint Flags;
        public uint Size;
        public uint Alignment;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MftInputStreamInfo
    {
        public long MaxLatency;
        public uint Flags;
        public uint Size;
        public uint MaxLookahead;
        public uint Alignment;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MftOutputDataBuffer
    {
        public uint StreamId;
        public IntPtr Sample;
        public uint Status;
        public IntPtr Events;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct D3d11Texture2DDescription
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

    /// <summary>64 位 PROPVARIANT：vt 在 0，数据从偏移 8 开始，总长 24。</summary>
    [StructLayout(LayoutKind.Explicit, Size = 24)]
    public struct PropVariant
    {
        [FieldOffset(0)] public ushort VarType;
        [FieldOffset(8)] public ulong First;
        [FieldOffset(16)] public ulong Second;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct DxgiAdapterDesc1
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
        public string Description;

        public uint VendorId;
        public uint DeviceId;
        public uint SubSysId;
        public uint Revision;
        public nint DedicatedVideoMemory;
        public nint DedicatedSystemMemory;
        public nint SharedSystemMemory;
        public long AdapterLuid;
        public uint Flags;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ReleaseDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int QueryInterfaceDelegate(IntPtr self, ref Guid riid, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ActivateObjectDelegate(IntPtr self, ref Guid riid, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetCountDelegate(IntPtr self, out uint count);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetItemByIndexDelegate(IntPtr self, uint index, out Guid key, out PropVariant value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetGuidDelegate(IntPtr self, ref Guid key, out Guid value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetUInt32Delegate(IntPtr self, ref Guid key, uint value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetUInt64Delegate(IntPtr self, ref Guid key, ulong value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CopyAllItemsDelegate(IntPtr self, IntPtr source);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetUnknownDelegate(IntPtr self, ref Guid key, IntPtr value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetGuidDelegate(IntPtr self, ref Guid key, ref Guid value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int NoArgDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetOnePointerDelegate(IntPtr self, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int AddStreamDelegate(IntPtr self, IntPtr outputType, out uint streamIndex);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetInputMediaTypeDelegate(IntPtr self, uint streamIndex, IntPtr inputType, IntPtr properties);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int WriteSampleDelegate(IntPtr self, uint streamIndex, IntPtr sample, uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetServiceForStreamDelegate(
        IntPtr self,
        uint streamIndex,
        ref Guid service,
        ref Guid riid,
        out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int EnumAdapters1Delegate(IntPtr self, uint index, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetAdapterDesc1Delegate(IntPtr self, IntPtr description);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ResetDeviceDelegate(IntPtr self, IntPtr device, uint resetToken);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetStreamCountDelegate(IntPtr self, out uint inputs, out uint outputs);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetStreamInfoDelegate(IntPtr self, uint streamId, ref MftOutputStreamInfo info);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetInputStreamInfoDelegate(IntPtr self, uint streamId, ref MftInputStreamInfo info);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetAvailableTypeDelegate(IntPtr self, uint streamId, uint index, out IntPtr type);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetTypeDelegate(IntPtr self, uint streamId, IntPtr type, uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ProcessMessageDelegate(IntPtr self, uint message, IntPtr parameter);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ProcessInputDelegate(IntPtr self, uint streamId, IntPtr sample, uint flags);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ProcessOutputDelegate(
        IntPtr self,
        uint flags,
        uint count,
        ref MftOutputDataBuffer buffer,
        out uint status);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int AddBufferDelegate(IntPtr self, IntPtr buffer);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetTimeDelegate(IntPtr self, long value);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ConvertToContiguousBufferDelegate(IntPtr self, out IntPtr buffer);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetCurrentLengthDelegate(IntPtr self, out uint length);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int SetCurrentLengthDelegate(IntPtr self, uint length);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int CreateTexture2DDelegate(
        IntPtr self,
        ref D3d11Texture2DDescription description,
        IntPtr initialData,
        out IntPtr texture);

    public static T Invoke<T>(IntPtr comObject, int slot) where T : class
    {
        var vtable = Marshal.ReadIntPtr(comObject);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
    }

    public static int Release(IntPtr comObject)
    {
        if (comObject == IntPtr.Zero)
        {
            return 0;
        }

        return Invoke<ReleaseDelegate>(comObject, SlotRelease)(comObject);
    }

    public static int SetUInt32(IntPtr comObject, Guid key, uint value)
    {
        var target = key;
        return Invoke<SetUInt32Delegate>(comObject, SlotAttributesSetUINT32)(comObject, ref target, value);
    }

    public static int SetUInt64(IntPtr comObject, Guid key, ulong value)
    {
        var target = key;
        return Invoke<SetUInt64Delegate>(comObject, SlotAttributesSetUINT64)(comObject, ref target, value);
    }

    public static int SetGuid(IntPtr comObject, Guid key, Guid value)
    {
        var targetKey = key;
        var targetValue = value;
        return Invoke<SetGuidDelegate>(comObject, SlotAttributesSetGUID)(comObject, ref targetKey, ref targetValue);
    }

    public static int SetUnknown(IntPtr comObject, Guid key, IntPtr value)
    {
        var target = key;
        return Invoke<SetUnknownDelegate>(comObject, SlotAttributesSetUnknown)(comObject, ref target, value);
    }

    public static ulong Pack(uint high, uint low) => (ulong)high << 32 | low;

    public static int QueryInterface(IntPtr comObject, Guid iid, out IntPtr result)
    {
        var target = iid;
        return Invoke<QueryInterfaceDelegate>(comObject, SlotQueryInterface)(comObject, ref target, out result);
    }

    public static string Hex(int hr) => "0x" + hr.ToString("X8");

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFStartup(int version, int flags);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFShutdown();

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFTEnumEx(
        Guid category,
        uint flags,
        ref MftRegisterTypeInfo inputType,
        ref MftRegisterTypeInfo outputType,
        out IntPtr activates,
        out uint count);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateMediaType(out IntPtr mediaType);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateSample(out IntPtr sample);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateMemoryBuffer(uint maxLength, out IntPtr buffer);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateAttributes(out IntPtr attributes, uint initialCapacity);

    [DllImport("mfreadwrite.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    public static extern int MFCreateSinkWriterFromURL(
        string outputUrl,
        IntPtr byteStream,
        IntPtr attributes,
        out IntPtr sinkWriter);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateDXGIDeviceManager(out uint resetToken, out IntPtr deviceManager);

    [DllImport("mfplat.dll", ExactSpelling = true)]
    public static extern int MFCreateDXGISurfaceBuffer(
        ref Guid riid,
        IntPtr surface,
        uint subresourceIndex,
        int bottomUpWhenLinear,
        out IntPtr buffer);

    [DllImport("d3d11.dll", ExactSpelling = true)]
    public static extern int D3D11CreateDevice(        IntPtr adapter,
        uint driverType,
        IntPtr software,
        uint flags,
        IntPtr featureLevels,
        uint featureLevelCount,
        uint sdkVersion,
        out IntPtr device,
        IntPtr obtainedFeatureLevel,
        out IntPtr context);

    [DllImport("ole32.dll", ExactSpelling = true)]
    public static extern void CoTaskMemFree(IntPtr pointer);

    [DllImport("dxgi.dll", ExactSpelling = true)]
    public static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);

    [DllImport("ole32.dll", ExactSpelling = true)]
    public static extern int PropVariantClear(ref PropVariant value);
}
