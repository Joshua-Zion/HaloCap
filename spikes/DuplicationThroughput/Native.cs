using System;
using System.Runtime.InteropServices;

namespace HaloCap.Spikes2;

/// <summary>
/// 桌面复制所需的手写 interop。槽位与结构体按 dxgi1_2.h / dxgi.h 数出来：
/// IDXGIOutput1::DuplicateOutput = 22；IDXGIOutputDuplication 的 AcquireNextFrame = 8、
/// GetFrameDirtyRects = 9、GetFrameMoveRects = 10、ReleaseFrame = 14。
/// </summary>
internal static class Dxgi
{
    public const int SlotFactoryEnumAdapters1 = 12;
    public const int SlotAdapterEnumOutputs = 7;
    public const int SlotOutputGetDesc = 7;
    public const int SlotOutput1DuplicateOutput = 22;

    public const int SlotDuplicationAcquireNextFrame = 8;
    public const int SlotDuplicationGetFrameDirtyRects = 9;
    public const int SlotDuplicationGetFrameMoveRects = 10;
    public const int SlotDuplicationReleaseFrame = 14;

    public const int DxgiErrorWaitTimeout = unchecked((int)0x887A0027);
    public const int DxgiErrorAccessLost = unchecked((int)0x887A0026);

    public static readonly Guid IIDIDXGIFactory1 = new("770aae78-f26f-4dba-a829-253c83d1b387");
    public static readonly Guid IIDIDXGIOutput1 = new("00cddea8-939b-4b83-a340-a685226666cc");

    [StructLayout(LayoutKind.Sequential)]
    public struct Rect
    {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;

        public long Area
        {
            get
            {
                var width = Right - Left;
                var height = Bottom - Top;

                return width > 0 && height > 0 ? (long)width * height : 0;
            }
        }

        public int Width() => Right - Left;

        public int Height() => Bottom - Top;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct Point
    {
        public int X;
        public int Y;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct MoveRect
    {
        public Point Source;
        public Rect Destination;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct PointerPosition
    {
        public int X;
        public int Y;
        public uint IncludeCursor;
    }

    /// <summary>48 字节：偏移在 dxgi1_2.h 上逐个核过，别改字段顺序。</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct FrameInfo
    {
        public long LastPresentTime;
        public long LastMouseUpdateTime;
        public uint AccumulatedFrames;
        public uint RectsCoalesced;
        public uint ProtectedContentMaskedOut;
        public PointerPosition PointerPosition;
        public uint TotalMetadataBufferSize;
        public uint PointerShapeBufferSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct OutputDesc
    {
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string DeviceName;

        public Rect DesktopCoordinates;
        public int AttachedToDesktop;
        public uint Rotation;
        public IntPtr Monitor;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int ReleaseDelegate(IntPtr self);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int QueryInterfaceDelegate(IntPtr self, ref Guid riid, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int EnumAdapters1Delegate(IntPtr self, uint index, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int EnumOutputsDelegate(IntPtr self, uint index, out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetOutputDescDelegate(IntPtr self, IntPtr description);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int DuplicateOutputDelegate(IntPtr self, IntPtr device, out IntPtr duplication);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int AcquireNextFrameDelegate(
        IntPtr self,
        uint timeoutMilliseconds,
        out FrameInfo frameInfo,
        out IntPtr resource);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int GetFrameRectsDelegate(
        IntPtr self,
        uint capacity,
        IntPtr buffer,
        out uint required);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    public delegate int NoArgDelegate(IntPtr self);

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

    [DllImport("dxgi.dll", ExactSpelling = true)]
    public static extern int CreateDXGIFactory1(ref Guid riid, out IntPtr factory);

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

    [DllImport("winmm.dll", EntryPoint = "timeBeginPeriod", ExactSpelling = true)]
    public static extern int TimeBeginPeriod(uint milliseconds);

    [DllImport("winmm.dll", EntryPoint = "timeEndPeriod", ExactSpelling = true)]
    public static extern int TimeEndPeriod(uint milliseconds);

    [DllImport("ole32.dll", ExactSpelling = true)]
    public static extern void CoTaskMemFree(IntPtr pointer);
}

/// <summary>受控变化源用的最小 Win32 窗口：不抢焦点、不进任务栏，只用来制造可知的画面变化。</summary>
internal static class Win32
{
    public const uint WsPopup = 0x80000000;
    public const uint WsVisible = 0x10000000;
    public const uint WsExToolWindow = 0x00000080;
    public const uint WsExNoActivate = 0x08000000;
    public const uint SwpNoZOrder = 0x0004;
    public const uint SwpNoActivate = 0x0010;
    public const uint SsBlackRect = 0x00000004;

    [StructLayout(LayoutKind.Sequential)]
    public struct Msg
    {
        public IntPtr hWnd;
        public uint message;
        public IntPtr wParam;
        public IntPtr lParam;
        public uint time;
        public int ptX;
        public int ptY;
    }

    [DllImport("user32.dll", ExactSpelling = true, CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr CreateWindowExW(
        uint dwExStyle,
        string lpClassName,
        string? lpWindowName,
        uint dwStyle,
        int x,
        int y,
        int nWidth,
        int nHeight,
        IntPtr hWndParent,
        IntPtr hMenu,
        IntPtr hInstance,
        IntPtr lpParam);

    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool DestroyWindow(IntPtr hWnd);

    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool SetWindowPos(
        IntPtr hWnd,
        IntPtr hWndInsertAfter,
        int x,
        int y,
        int cx,
        int cy,
        uint uFlags);

    [DllImport("user32.dll", ExactSpelling = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool PeekMessageW(out Msg lpMsg, IntPtr hWnd, uint min, uint max, uint remove);

    [DllImport("user32.dll", ExactSpelling = true)]
    public static extern int TranslateMessage(ref Msg lpMsg);

    [DllImport("user32.dll", ExactSpelling = true)]
    public static extern IntPtr DispatchMessageW(ref Msg lpMsg);

    [DllImport("user32.dll", ExactSpelling = true)]
    public static extern int GetSystemMetrics(int nIndex);

    [DllImport("kernel32.dll", EntryPoint = "GetModuleHandleW", ExactSpelling = true, CharSet = CharSet.Unicode)]
    public static extern IntPtr GetModuleHandle(string? lpModuleName);

    public const int SmXVirtualScreen = 76;
    public const int SmYVirtualScreen = 77;
    public const int SmCxVirtualScreen = 78;
    public const int SmCyVirtualScreen = 79;
    public const uint PmRemove = 0x0001;
}
