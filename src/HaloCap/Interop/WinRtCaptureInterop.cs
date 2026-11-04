using System;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using HaloCap.Diagnostics;
using Windows.Graphics.Capture;
using Windows.Graphics.DirectX.Direct3D11;

namespace HaloCap.Interop;

internal static class WinRtCaptureInterop
{
    private const string CaptureItemClassName = "Windows.Graphics.Capture.GraphicsCaptureItem";
    private const uint D3D11CreateDeviceBgraSupport = 0x20;
    private const int D3DDriverTypeHardware = 1;
    private const int D3DDriverTypeWarp = 5;
    private const uint D3D11SdkVersion = 7;

    private static readonly Guid GraphicsCaptureItemInteropIid =
        new("3628E81B-3CAC-4C60-B7F4-23CE0E0C3356");

    private static readonly Guid GraphicsCaptureItemIid =
        new("79C3F95B-31F7-4EC2-A464-632EF5D30760");

    private static readonly Guid DxgiDeviceIid =
        new("54EC77FA-1377-44E6-8C32-88FD5F44C84C");

    // Windows.Graphics.Capture.IGraphicsCaptureSession3：只有 IsBorderRequired 一个属性。
    private static readonly Guid GraphicsCaptureSession3Iid =
        new("F2CDD966-22AE-5EA1-9596-3A289344C3BE");

    // windows.graphics.directx.direct3d11.interop.h：IDirect3DDxgiInterfaceAccess 只有 GetInterface 一个方法。
    private static readonly Guid Direct3DDxgiInterfaceAccessIid =
        new("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1");

    private static readonly Guid D3D11Texture2DIid =
        new("6F15AAF2-D208-4E89-9AB4-489535D34F9C");

    [DllImport("combase.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    private static extern int WindowsCreateString(string sourceString, uint length, out IntPtr hstring);

    [DllImport("combase.dll", ExactSpelling = true)]
    private static extern int WindowsDeleteString(IntPtr hstring);

    [DllImport("combase.dll", ExactSpelling = true)]
    private static extern int RoGetActivationFactory(
        IntPtr activatableClassId,
        ref Guid iid,
        out IntPtr factory);

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int D3D11CreateDevice(
        IntPtr adapter,
        int driverType,
        IntPtr software,
        uint flags,
        IntPtr featureLevels,
        uint featureLevelCount,
        uint sdkVersion,
        out IntPtr device,
        out int featureLevel,
        out IntPtr immediateContext);

    [DllImport("d3d11.dll", ExactSpelling = true)]
    private static extern int CreateDirect3D11DeviceFromDXGIDevice(
        IntPtr dxgiDevice,
        out IntPtr graphicsDevice);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int CreateItemDelegate(
        IntPtr self,
        IntPtr handle,
        ref Guid iid,
        out IntPtr result);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int SetBoolDelegate(IntPtr self, byte value);

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate int GetInterfaceDelegate(IntPtr self, ref Guid iid, out IntPtr result);

    public static GraphicsCaptureItem CreateItemForMonitor(IntPtr monitor)
    {
        if (monitor == IntPtr.Zero)
        {
            throw new ArgumentException("显示器句柄为空。", nameof(monitor));
        }

        return CreateItem(4, monitor, "为显示器创建捕获项目");
    }

    public static GraphicsCaptureItem CreateItemForWindow(IntPtr window)
    {
        if (window == IntPtr.Zero)
        {
            throw new ArgumentException("窗口句柄为空。", nameof(window));
        }

        return CreateItem(3, window, "为窗口创建捕获项目");
    }

    private static GraphicsCaptureItem CreateItem(int slot, IntPtr handle, string operation)
    {
        var classId = CaptureItemClassName;
        CheckHr(
            WindowsCreateString(classId, (uint)classId.Length, out var hstring),
            "创建 WinRT 类名");

        try
        {
            var interopIid = GraphicsCaptureItemInteropIid;
            CheckHr(
                RoGetActivationFactory(hstring, ref interopIid, out var factoryPtr),
                "获取 Windows.Graphics.Capture 工厂");

            try
            {
                // IGraphicsCaptureItemInterop: IUnknown 之后依次是 CreateForWindow(3)、CreateForMonitor(4)。
                var vtable = Marshal.ReadIntPtr(factoryPtr);
                var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
                var create = (CreateItemDelegate)Marshal.GetDelegateForFunctionPointer(
                    function,
                    typeof(CreateItemDelegate));

                var itemIid = GraphicsCaptureItemIid;
                CheckHr(
                    create(factoryPtr, handle, ref itemIid, out var itemPtr),
                    operation);

                return (GraphicsCaptureItem)ConvertFromAbi(typeof(GraphicsCaptureItem), itemPtr);
            }
            finally
            {
                if (factoryPtr != IntPtr.Zero)
                {
                    Marshal.Release(factoryPtr);
                }
            }
        }
        finally
        {
            WindowsDeleteString(hstring);
        }
    }

    /// <summary>
    /// 对 WinRT 投影对象做真正的原生 QueryInterface。
    /// 直接用 Marshal.GetIUnknownForObject 拿到的是托管 CCW，它不认识只在原生侧存在的
    /// 接口（比如 IDirect3DDxgiInterfaceAccess），所以必须走投影自己的对象引用。
    /// </summary>
    public static IntPtr QueryInterface(object projected, Guid iid, string operation)
    {
        if (!AbiReference.TryQuery(projected, iid, out var pointer, out var hr))
        {
            throw new COMException(
                $"{operation}失败（HRESULT 0x{hr:X8}）。",
                hr < 0 ? hr : unchecked((int)0x80004002));
        }

        return pointer;
    }

    /// <summary>
    /// 关闭 WGC 在窗口或屏幕上绘制的彩色边框。系统需要 20348+ 且授予无边框权限，
    /// 不支持时保持默认行为并记录一次日志。
    /// </summary>
    public static bool TryDisableBorder(Windows.Graphics.Capture.GraphicsCaptureSession session)
    {
        IntPtr session3 = IntPtr.Zero;

        try
        {
            // IInspectable 占 0..5，get_IsBorderRequired 是 6，put_IsBorderRequired 是 7。
            session3 = QueryInterface(session, GraphicsCaptureSession3Iid, "查询 IGraphicsCaptureSession3");
            var hr = Invoke<SetBoolDelegate>(session3, 7)(session3, 0);
            return hr >= 0;
        }
        catch (Exception ex)
        {
            Log.Error("关闭录屏彩色边框失败", ex);
            return false;
        }
        finally
        {
            if (session3 != IntPtr.Zero)
            {
                Marshal.Release(session3);
            }
        }
    }

    /// <summary>
    /// 取回 WGC 帧表面背后的 ID3D11Texture2D 指针，调用方负责 Release。
    /// </summary>
    public static IntPtr GetNativeTexture(Windows.Graphics.DirectX.Direct3D11.IDirect3DSurface surface)
    {
        var access = QueryInterface(surface, Direct3DDxgiInterfaceAccessIid, "获取 IDirect3DDxgiInterfaceAccess");

        try
        {
            var textureIid = D3D11Texture2DIid;
            var hr = Invoke<GetInterfaceDelegate>(access, 3)(access, ref textureIid, out var texture);

            if (hr < 0 || texture == IntPtr.Zero)
            {
                throw new COMException(
                    $"取回 D3D11 帧纹理失败（HRESULT 0x{hr:X8}）。",
                    hr < 0 ? hr : unchecked((int)0x80004002));
            }

            return texture;
        }
        finally
        {
            Marshal.Release(access);
        }
    }

    public static Direct3DDeviceHolder CreateDirect3DDeviceHolder()
    {
        var featureLevels = new[] { 0xB100, 0xB000, 0xA000, 0x9300 };
        var featureLevelsHandle = GCHandle.Alloc(featureLevels, GCHandleType.Pinned);
        IntPtr device = IntPtr.Zero;
        IntPtr context = IntPtr.Zero;

        try
        {
            var hr = D3D11CreateDevice(
                IntPtr.Zero,
                D3DDriverTypeHardware,
                IntPtr.Zero,
                D3D11CreateDeviceBgraSupport,
                featureLevelsHandle.AddrOfPinnedObject(),
                (uint)featureLevels.Length,
                D3D11SdkVersion,
                out device,
                out _,
                out context);

            if (hr < 0)
            {
                hr = D3D11CreateDevice(
                    IntPtr.Zero,
                    D3DDriverTypeWarp,
                    IntPtr.Zero,
                    D3D11CreateDeviceBgraSupport,
                    featureLevelsHandle.AddrOfPinnedObject(),
                    (uint)featureLevels.Length,
                    D3D11SdkVersion,
                    out device,
                    out _,
                    out context);
            }

            CheckHr(hr, "创建 Direct3D 设备");
        }
        finally
        {
            featureLevelsHandle.Free();
        }

        try
        {
            var dxgiIid = DxgiDeviceIid;
            CheckHr(
                Marshal.QueryInterface(device, ref dxgiIid, out var dxgiDevice),
                "获取 DXGI 设备");

            try
            {
                CheckHr(
                    CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice, out var inspectable),
                    "创建 WinRT Direct3D 设备");

                var projected = (IDirect3DDevice)ConvertFromAbi(typeof(IDirect3DDevice), inspectable);

                return new Direct3DDeviceHolder(projected, device, context);
            }
            finally
            {
                Marshal.Release(dxgiDevice);
            }
        }
        catch
        {
            ReleaseIfExists(context);
            ReleaseIfExists(device);
            throw;
        }
    }

    private static void ReleaseIfExists(IntPtr pointer)
    {
        if (pointer != IntPtr.Zero)
        {
            Marshal.Release(pointer);
        }
    }

    private static T Invoke<T>(IntPtr comObject, int slot) where T : class
    {
        var vtable = Marshal.ReadIntPtr(comObject);
        var function = Marshal.ReadIntPtr(vtable, slot * IntPtr.Size);
        return (T)(object)Marshal.GetDelegateForFunctionPointer(function, typeof(T));
    }

    private static object ConvertFromAbi(Type projectedType, IntPtr abi)
    {
        if (abi == IntPtr.Zero)
        {
            throw new InvalidOperationException("WinRT 对象指针为空。");
        }

        foreach (var typeName in new[] { "WinRT.MarshalInterface`1", "WinRT.MarshalInspectable`1" })
        {
            var openType = FindType(typeName);
            if (openType is null)
            {
                continue;
            }

            try
            {
                var closedType = openType.MakeGenericType(projectedType);
                var method = closedType.GetMethod(
                    "FromAbi",
                    BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static,
                    null,
                    new[] { typeof(IntPtr) },
                    null);

                if (method is null)
                {
                    continue;
                }

                var result = method.Invoke(null, new object[] { abi });
                if (result is not null)
                {
                    return result;
                }
            }
            catch (Exception ex)
            {
                Log.Error($"WinRT 对象转换失败（{typeName}）", ex);
            }
        }

        try
        {
            var ownFactory = projectedType.GetMethod(
                "FromAbi",
                BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.Static,
                null,
                new[] { typeof(IntPtr) },
                null);

            if (ownFactory is not null)
            {
                var result = ownFactory.Invoke(null, new object[] { abi });
                if (result is not null)
                {
                    return result;
                }
            }
        }
        catch (Exception ex)
        {
            Log.Error("WinRT 类型自身的 FromAbi 转换失败", ex);
        }

        var castExtensions = FindType("WinRT.CastExtensions");
        if (castExtensions is not null)
        {
            try
            {
                var method = castExtensions
                    .GetMethods(BindingFlags.Public | BindingFlags.Static)
                    .FirstOrDefault(methodInfo =>
                        methodInfo.Name == "FromAbi" &&
                        methodInfo.IsGenericMethodDefinition &&
                        methodInfo.GetParameters().Length == 1 &&
                        methodInfo.GetParameters()[0].ParameterType == typeof(IntPtr));

                if (method is not null)
                {
                    var result = method.MakeGenericMethod(projectedType).Invoke(null, new object[] { abi });
                    if (result is not null)
                    {
                        return result;
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Error("WinRT CastExtensions 转换失败", ex);
            }
        }

        var fallback = Marshal.GetObjectForIUnknown(abi);
        if (projectedType.IsInstanceOfType(fallback))
        {
            return fallback;
        }

        throw new InvalidOperationException("无法创建 WinRT 对象：" + projectedType.FullName);
    }

    private static Type? FindType(string fullName)
    {
        try
        {
            var type = Type.GetType(fullName + ", WinRT.Runtime", false);
            if (type is not null)
            {
                return type;
            }
        }
        catch
        {
            // 继续扫描已加载程序集。
        }

        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            try
            {
                var type = assembly.GetType(fullName, false);
                if (type is not null)
                {
                    return type;
                }
            }
            catch
            {
                // 忽略无法反射的程序集。
            }
        }

        return null;
    }

    private static void CheckHr(int hr, string operation)
    {
        if (hr < 0)
        {
            throw new COMException(
                operation + "失败（HRESULT 0x" + hr.ToString("X8") + "）。",
                hr);
        }
    }
}
