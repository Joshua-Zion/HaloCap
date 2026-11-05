using System;
using System.Reflection;
using System.Runtime.InteropServices;

namespace HaloCap.Interop;

/// <summary>
/// 通过 CsWinRT 投影自己的对象引用做原生 QueryInterface。
/// Marshal.GetIUnknownForObject 只会得到托管侧的 CCW，它不认识仅存在于原生侧的接口
/// （例如 IDirect3DDxgiInterfaceAccess），必须用投影保存的原生 IUnknown 指针。
/// </summary>
internal static class AbiReference
{
    private const int ENoInterface = unchecked((int)0x80004002);

    private static PropertyInfo? _nativeObjectProperty;
    private static PropertyInfo? _thisPtrProperty;
    private static bool _initialized;

    internal static bool TryQuery(object projected, Guid iid, out IntPtr pointer, out int hr)
    {
        pointer = IntPtr.Zero;
        hr = ENoInterface;

        if (projected is null || !EnsureReflection())
        {
            return false;
        }

        object? nativeObject;
        IntPtr unknown;

        try
        {
            nativeObject = _nativeObjectProperty!.GetValue(projected);

            if (nativeObject is null)
            {
                return false;
            }

            unknown = (IntPtr)_thisPtrProperty!.GetValue(nativeObject)!;
        }
        catch (Exception ex)
        {
            hr = Marshal.GetHRForException(ex);
            return false;
        }

        if (unknown == IntPtr.Zero)
        {
            return false;
        }

        var target = iid;
        hr = Marshal.QueryInterface(unknown, in target, out pointer);

        if (hr < 0 || pointer == IntPtr.Zero)
        {
            pointer = IntPtr.Zero;

            if (hr >= 0)
            {
                hr = ENoInterface;
            }

            return false;
        }

        return true;
    }

    private static bool EnsureReflection()
    {
        if (_initialized)
        {
            return _nativeObjectProperty is not null && _thisPtrProperty is not null;
        }

        _initialized = true;

        var winRuntimeObject = FindType("WinRT.IWinRTObject");
        var objectReference = FindType("WinRT.IObjectReference");

        _nativeObjectProperty = winRuntimeObject?.GetProperty("NativeObject");
        _thisPtrProperty = objectReference?.GetProperty("ThisPtr");

        return _nativeObjectProperty is not null && _thisPtrProperty is not null;
    }

    private static Type? FindType(string name)
    {
        try
        {
            var type = Type.GetType(name + ", WinRT.Runtime", false);

            if (type is not null)
            {
                return type;
            }
        }
        catch
        {
            // 继续扫描已加载的程序集。
        }

        foreach (var assembly in AppDomain.CurrentDomain.GetAssemblies())
        {
            try
            {
                var type = assembly.GetType(name, false);

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
}
