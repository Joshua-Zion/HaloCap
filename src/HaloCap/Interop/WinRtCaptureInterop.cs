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
    private delegate int CreateForMonitorDelegate(
        IntPtr self,
        IntPtr monitor,
        ref Guid iid,
        out IntPtr result);

    public static GraphicsCaptureItem CreateItemForMonitor(IntPtr monitor)
    {
        if (monitor == IntPtr.Zero)
        {
            throw new ArgumentException("显示器句柄为空。", nameof(monitor));
        }

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
                var function = Marshal.ReadIntPtr(vtable, 4 * IntPtr.Size);
                var createForMonitor = (CreateForMonitorDelegate)Marshal.GetDelegateForFunctionPointer(
                    function,
                    typeof(CreateForMonitorDelegate));

                var itemIid = GraphicsCaptureItemIid;
                CheckHr(
                    createForMonitor(factoryPtr, monitor, ref itemIid, out var itemPtr),
                    "为显示器创建捕获项目");

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

    public static IDirect3DDevice CreateDirect3DDevice()
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

                return (IDirect3DDevice)ConvertFromAbi(typeof(IDirect3DDevice), inspectable);
            }
            finally
            {
                Marshal.Release(dxgiDevice);
            }
        }
        finally
        {
            if (context != IntPtr.Zero)
            {
                Marshal.Release(context);
            }

            if (device != IntPtr.Zero)
            {
                Marshal.Release(device);
            }
        }
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
