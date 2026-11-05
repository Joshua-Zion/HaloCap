using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace HaloCap.Spikes;

/// <summary>
/// S0 spike 1：硬件 H.264 编码器是否接受 D3D11 纹理直入。
///   list                        列出硬件/软件编码器与激活属性
///   probe hw|sw N [w] [h] [f]   纹理直入路径
///   control hw|sw N [w] [h] [f] 系统内存对照路径
///   all [w] [h] [f]             每个编码器 fork 子进程跑两遍，崩溃只损失一项
/// </summary>
internal static class Program
{
    private sealed record Target(
        bool Hardware,
        int Index,
        int Width,
        int Height,
        int Frames,
        bool Texture,
        int Variant = 0,
        int Adapter = -1);

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;

        if (args.Length == 0)
        {
            return 1;
        }

        try
        {
            return args[0].ToLowerInvariant() switch
            {
                "list" => List(),
                "probe" => Run(ReadTarget(args, allowIndex: true), true),
                "control" => Run(ReadTarget(args, allowIndex: true), false),
                "writer" => RunWriter(ReadWriterTarget(args)),
                "adapters" => RunAdapters(ReadWriterTarget(args)),
                "all" => RunAll(ReadTarget(args, allowIndex: false)),
                _ => 1
            };
        }
        catch (Exception ex)
        {
            Say("FAIL", ex.GetType().Name + " " + ex.Message);
            return 3;
        }
    }

    private static void Say(string key, string value) => Console.WriteLine(key + "=" + value);

    private static Target ReadTarget(string[] args, bool allowIndex)
    {
        var hardware = !(args.Length >= 2 && args[1].Equals("sw", StringComparison.OrdinalIgnoreCase));
        var index = 0;
        var width = 1920;
        var height = 1080;
        var frames = 30;

        if (allowIndex)
        {
            if (args.Length >= 3 && int.TryParse(args[2], out var parsed))
            {
                index = parsed;
            }

            if (args.Length >= 6
                && int.TryParse(args[3], out width)
                && int.TryParse(args[4], out height)
                && int.TryParse(args[5], out frames))
            {
                // 用了调用方给的尺寸与帧数。
            }

            return new Target(hardware, index, width, height, frames, true);
        }

        if (args.Length >= 4
            && int.TryParse(args[1], out width)
            && int.TryParse(args[2], out height)
            && int.TryParse(args[3], out frames))
        {
            // all 模式同样支持自定义尺寸。
        }

        return new Target(true, 0, width, height, frames, true);
    }

    /// <summary>writer hw|sw texture|cpu [w] [h] [f] [variant]：走 SinkWriter，也就是 2.0 实际会用的那条路。</summary>
    private static Target ReadWriterTarget(string[] args)
    {
        var hardware = !(args.Length >= 2 && args[1].Equals("sw", StringComparison.OrdinalIgnoreCase));
        var texture = !(args.Length >= 3 && args[2].Equals("cpu", StringComparison.OrdinalIgnoreCase));
        var width = 1920;
        var height = 1080;
        var frames = 60;
        var variant = 0;

        if (args.Length >= 6
            && int.TryParse(args[3], out width)
            && int.TryParse(args[4], out height)
            && int.TryParse(args[5], out frames))
        {
            // 用了调用方给的尺寸与帧数。
        }

        if (args.Length >= 7 && int.TryParse(args[6], out variant))
        {
            // 变体：0=裸纹理样本，1=样本标 MF_SA_D3D11_AWARE，2=再加 BINDFLAGS。
        }

        var adapter = -1;

        if (args.Length >= 8 && int.TryParse(args[7], out adapter))
        {
            // 指定 DXGI 适配器序号；-1 表示用系统默认适配器。
        }

        return new Target(hardware, 0, width, height, frames, texture, variant, adapter);
    }

    // ---- 枚举 ----

    private static IntPtr Enumerate(bool hardware, out uint count)
    {
        var input = new Native.MftRegisterTypeInfo
        {
            MajorType = Native.MFMediaTypeVideo,
            Subtype = Native.MFVideoFormatNv12
        };

        var output = new Native.MftRegisterTypeInfo
        {
            MajorType = Native.MFMediaTypeVideo,
            Subtype = Native.MFVideoFormatH264
        };

        var flags = Native.MftEnumFlagSortAndFilter
            | (hardware ? Native.MftEnumFlagHardware : Native.MftEnumFlagSyncMft);

        var hr = Native.MFTEnumEx(Native.MFTCategoryVideoEncoder, flags, ref input, ref output, out var array, out count);

        if (hr < 0)
        {
            Say("ENUM_HR", Native.Hex(hr));
            return IntPtr.Zero;
        }

        return array;
    }

    private static int List()
    {
        Say("MFSTARTUP_HR", Native.Hex(Native.MFStartup(Native.MfVersion, 0)));

        try
        {
            foreach (var hardware in new[] { true, false })
            {
                var tag = hardware ? "HW" : "SW";
                var array = Enumerate(hardware, out var count);
                Say(tag + "_COUNT", count.ToString());

                for (uint i = 0; i < count && array != IntPtr.Zero; i++)
                {
                    var activate = Marshal.ReadIntPtr(array, (int)(i * IntPtr.Size));
                    Say(tag, i + " | " + DumpAttributes(activate));
                    Native.Release(activate);
                }

                Native.CoTaskMemFree(array);
            }
        }
        finally
        {
            Native.MFShutdown();
        }

        return 0;
    }

    /// <summary>打印属性表；顺带把运行中发现的 MF_MT_DXGI_FORMAT 键记下来，不预设任何未验证的 GUID。</summary>
    private static string DumpAttributes(IntPtr attributesObject)
    {

        var hr = Native.Invoke<Native.GetCountDelegate>(attributesObject, Native.SlotAttributesGetCount)(
            attributesObject,
            out var count);

        if (hr < 0)
        {
            return "(属性读取失败 " + Native.Hex(hr) + ")";
        }

        var builder = new StringBuilder();

        for (uint i = 0; i < count; i++)
        {
            var itemHr = Native.Invoke<Native.GetItemByIndexDelegate>(
                attributesObject,
                Native.SlotAttributesGetItemByIndex)(attributesObject, i, out var key, out var value);

            if (itemHr < 0)
            {
                continue;
            }

            var text = DescribePropVariant(value);

            if (value.VarType == 19
                && ((uint)value.First == Native.DxgiFormatNv12 || (uint)value.First == Native.DxgiFormatB8G8R8A8Unorm))
            {
                DxgiFormatKey = key;
            }

            builder.Append(key.ToString("B").ToUpperInvariant())
                .Append(":vt")
                .Append(value.VarType)
                .Append(':')
                .Append(text)
                .Append(' ');

            Native.PropVariantClear(ref value);
        }

        return builder.ToString().Trim();
    }

    /// <summary>友好名的属性键由本机实测得到（每条编码器记录都带它），不靠记忆写常量。</summary>
    private static string FriendlyName(IntPtr attributesObject)
    {
        const string FriendlyNameKey = "{314FFBAE-5B41-4C95-9C19-4E7D586FACE3}:vt31:";
        var dump = DumpAttributes(attributesObject);
        var at = dump.IndexOf(FriendlyNameKey, StringComparison.OrdinalIgnoreCase);

        return at < 0
            ? "(unnamed)"
            : dump[(at + FriendlyNameKey.Length)..].Trim();
    }

    public static Guid DxgiFormatKey { get; private set; }

    private static string DescribePropVariant(Native.PropVariant value)
    {
        switch (value.VarType)
        {
            case 19:
                return ((uint)value.First).ToString();
            case 21:
                return value.First.ToString();
            case 31:
                return value.First == 0
                    ? string.Empty
                    : Marshal.PtrToStringUni(new IntPtr((long)value.First)) ?? string.Empty;
            case 72:
                return value.First == 0
                    ? "null"
                    : Marshal.PtrToStructure<Guid>(new IntPtr((long)value.First)).ToString("B").ToUpperInvariant();
            case 0x1041:
                return "blob(" + (uint)value.First + ")";
            default:
                return "raw" + value.First;
        }
    }

    // ---- 探针 ----

    private static int Run(Target target, bool texturePath)
    {
        Say("MODE", texturePath ? "texture" : "cpu");
        Say("KIND", target.Hardware ? "hw" : "sw");
        Say("INDEX", target.Index.ToString());
        Say("SIZE", target.Width + "x" + target.Height + "@" + target.Frames + "f");

        Say("MFSTARTUP_HR", Native.Hex(Native.MFStartup(Native.MfVersion, 0)));

        var array = Enumerate(target.Hardware, out var count);
        Say("COUNT", count.ToString());

        if (array == IntPtr.Zero || (uint)target.Index >= count)
        {
            Say("OUTCOME", "NOT-ENUMERATED");
            return 2;
        }

        var activate = Marshal.ReadIntPtr(array, target.Index * IntPtr.Size);
        Native.CoTaskMemFree(array);

        var transform = ActivateTransform(activate);
        var name = FriendlyName(activate);
        Say("NAME", name);

        if (transform == IntPtr.Zero)
        {
            Say("OUTCOME", "ACTIVATE-FAILED");
            return 2;
        }

        var device = IntPtr.Zero;
        var context = IntPtr.Zero;
        var manager = IntPtr.Zero;
        var inputType = IntPtr.Zero;
        var patchedType = IntPtr.Zero;
        var outputType = IntPtr.Zero;
        var texture = IntPtr.Zero;
        var buffer = IntPtr.Zero;
        var activeInputType = IntPtr.Zero;

        try
        {
            var streamHr = Native.Invoke<Native.GetStreamCountDelegate>(
                transform,
                Native.SlotTransformGetStreamCount)(transform, out var inputs, out var outputs);

            Say("STREAMS", streamHr >= 0 ? inputs + "in/" + outputs + "out" : "hr " + Native.Hex(streamHr));

            // 硬件 MFT 的输入类型取决于设备，必须先装好 D3D 管理器再枚举。
            if (texturePath)
            {
                var deviceHr = Native.D3D11CreateDevice(
                    IntPtr.Zero,
                    Native.D3dDriverTypeHardware,
                    IntPtr.Zero,
                    Native.D3d11CreateDeviceBgraSupport,
                    IntPtr.Zero,
                    0,
                    Native.D3d11SdkVersion,
                    out device,
                    IntPtr.Zero,
                    out context);

                Say("D3D11CREATE_HR", Native.Hex(deviceHr));

                if (deviceHr < 0)
                {
                    Say("OUTCOME", "D3D11-DEVICE-FAILED");
                    return 2;
                }

                var managerHr = Native.MFCreateDXGIDeviceManager(out var resetToken, out manager);
                Say("DXGI_MANAGER_HR", Native.Hex(managerHr));

                if (managerHr >= 0)
                {
                    Say("RESET_DEVICE_HR", Native.Hex(
                        Native.Invoke<Native.ResetDeviceDelegate>(
                            manager,
                            Native.SlotDxgiManagerResetDevice)(manager, device, resetToken)));

                    // 这一行是 spike 1 的核心问题。
                    Say("SET_D3D_MANAGER_HR", Native.Hex(
                        Native.Invoke<Native.ProcessMessageDelegate>(
                            transform,
                            Native.SlotTransformProcessMessage)(
                            transform,
                            Native.MftMessageSetD3dManager,
                            manager)));
                }
            }

            inputType = FindInputType(transform, texturePath);

            if (inputType != IntPtr.Zero)
            {
                Say("INPUT_TYPE_ATTRS", DumpAttributes(inputType));
                patchedType = CloneWithTypeSize(inputType, target.Width, target.Height);
            }

            if (inputType == IntPtr.Zero)
            {
                Say("OUTCOME", "NO-INPUT-TYPE");
                return 2;
            }

            if (patchedType != IntPtr.Zero)
            {
                var patchedHr = SetInput(transform, patchedType, 0);
                Say("SET_INPUT_PATCHED_HR", Native.Hex(patchedHr));

                var testOnly = SetInput(transform, patchedType, Native.MftSetTypeTestOnly);
                Say("TEST_INPUT_PATCHED_HR", Native.Hex(testOnly));

                if (patchedHr >= 0)
                {
                    activeInputType = patchedType;
                }
            }

            if (activeInputType == IntPtr.Zero)
            {
                var asIsHr = SetInput(transform, inputType, 0);
                Say("SET_INPUT_ASIS_HR", Native.Hex(asIsHr));
                activeInputType = asIsHr >= 0 ? inputType : IntPtr.Zero;
            }

            if (activeInputType == IntPtr.Zero)
            {
                Say("OUTCOME", "SET-INPUT-TYPE-FAILED");
                return 2;
            }

            var inInfo = new Native.MftInputStreamInfo();
            var inInfoHr = Native.Invoke<Native.GetInputStreamInfoDelegate>(
                transform,
                Native.SlotTransformGetInputStreamInfo)(transform, 0, ref inInfo);

            Say("INPUT_INFO", inInfoHr >= 0 ? "flags " + inInfo.Flags + " size " + inInfo.Size : Native.Hex(inInfoHr));

            outputType = FindH264OutputType(transform, target);

            if (outputType == IntPtr.Zero)
            {
                Say("OUTCOME", "NO-OUTPUT-TYPE");
                return 2;
            }

            Say("SET_OUTPUT_TYPE_HR", Native.Hex(SetOutput(transform, outputType)));

            var outInfo = new Native.MftOutputStreamInfo();
            var outInfoHr = Native.Invoke<Native.GetStreamInfoDelegate>(
                transform,
                Native.SlotTransformGetOutputStreamInfo)(transform, 0, ref outInfo);

            Say("OUTPUT_INFO", outInfoHr >= 0 ? "flags " + outInfo.Flags + " size " + outInfo.Size : Native.Hex(outInfoHr));

            Say("START_OF_STREAM_HR", Native.Hex(
                Native.Invoke<Native.ProcessMessageDelegate>(
                    transform,
                    Native.SlotTransformProcessMessage)(
                    transform,
                    Native.MftMessageNotifyStartOfStream,
                    IntPtr.Zero)));

            if (texturePath)
            {
                texture = CreateNv12Texture(device, target.Width, target.Height);

                if (texture == IntPtr.Zero)
                {
                    Say("OUTCOME", "TEXTURE-FAILED");
                    return 2;
                }

                var riid = Native.IIDIMFMediaBuffer;
                var surfaceHr = Native.MFCreateDXGISurfaceBuffer(ref riid, texture, 0, 0, out buffer);
                Say("SURFACE_BUFFER_HR", Native.Hex(surfaceHr));

                if (surfaceHr < 0)
                {
                    Say("OUTCOME", "SURFACE-BUFFER-FAILED");
                    return 2;
                }

                Say("DXGI_BACKED", IsDXGIBacked(buffer) ? "1" : "0");
            }
            else
            {
                buffer = CreateNv12SystemBuffer(target.Width, target.Height);

                if (buffer == IntPtr.Zero)
                {
                    Say("OUTCOME", "SYSTEM-BUFFER-FAILED");
                    return 2;
                }
            }

            var providesSamples = outInfoHr >= 0
                && (outInfo.Flags & Native.MftOutputStreamProvidesSamples) != 0;

            return Pump(transform, buffer, target, providesSamples, texturePath ? "TEXTURE" : "CPU");
        }
        finally
        {
            Native.Release(buffer);
            Native.Release(texture);
            Native.Release(inputType);
            Native.Release(patchedType);
            Native.Release(outputType);
            Native.Release(manager);
            Native.Release(context);
            Native.Release(device);
            Native.Release(transform);
            Native.MFShutdown();
        }
    }

    private static bool IsDXGIBacked(IntPtr mediaBuffer)
    {
        if (Native.QueryInterface(mediaBuffer, Native.IIDIMFDXGIBuffer, out var dxgiBuffer) < 0)
        {
            return false;
        }

        try
        {
            var riid = Native.IIDIMFMediaBuffer;
            var hr = Native.Invoke<Native.QueryInterfaceDelegate>(
                dxgiBuffer,
                Native.SlotDxgiBufferGetResource)(dxgiBuffer, ref riid, out var resource);

            Native.Release(resource);
            return hr >= 0 && resource != IntPtr.Zero;
        }
        finally
        {
            Native.Release(dxgiBuffer);
        }
    }

    private static IntPtr ActivateTransform(IntPtr activate)
    {
        // 头文件给的是 bf94c121-…；留一个旧文档常见的候选值，谁先成功用谁，避免凭记忆下注。
        foreach (var iid in new[]
        {
            Native.IIDIMFTransform,
            new Guid("bf94c821-44d0-4e7a-8fcb-6985a9e3396b")
        })
        {
            var target = iid;
            var hr = Native.Invoke<Native.ActivateObjectDelegate>(
                activate,
                Native.SlotActivateActivateObject)(activate, ref target, out var transform);

            Say("ACTIVATE_HR", target.ToString("B").ToUpperInvariant() + " " + Native.Hex(hr));

            if (hr >= 0 && transform != IntPtr.Zero)
            {
                return transform;
            }
        }

        return IntPtr.Zero;
    }

    private static int SetInput(IntPtr transform, IntPtr type, uint flags) =>
        Native.Invoke<Native.SetTypeDelegate>(transform, Native.SlotTransformSetInputType)(transform, 0, type, flags);

    private static int SetOutput(IntPtr transform, IntPtr type) =>
        Native.Invoke<Native.SetTypeDelegate>(transform, Native.SlotTransformSetOutputType)(transform, 0, type, 0);

    /// <summary>纹理路径要带 DXGI_FORMAT 的输入类型，对照路径要不带的；同时把全貌打印出来。</summary>
    private static IntPtr FindInputType(IntPtr transform, bool wantTexture)
    {
        var collected = new System.Collections.Generic.List<(IntPtr Type, uint Dxgi)>();
        var firstHr = 0;

        for (var index = 0u; index < 64; index++)
        {
            var hr = Native.Invoke<Native.GetAvailableTypeDelegate>(
                transform,
                Native.SlotTransformGetInputAvailableType)(transform, 0, index, out var type);

            if (index == 0)
            {
                firstHr = hr;
            }

            if (hr < 0)
            {
                break;
            }

            collected.Add((type, ReadDxgiFormat(type)));
        }

        Say("INPUT_TYPE_TOTAL", collected.Count.ToString());
        Say("INPUT_TYPE0_HR", Native.Hex(firstHr));

        var formats = new System.Text.StringBuilder();

        foreach (var item in collected)
        {
            formats.Append(item.Dxgi == 0 ? "-" : item.Dxgi.ToString()).Append(',');
        }

        Say("INPUT_TYPE_DXGI_FORMATS", formats.ToString());

        var chosen = IntPtr.Zero;

        foreach (var item in collected)
        {
            var isTextureType = item.Dxgi != 0;

            if (chosen == IntPtr.Zero && isTextureType == wantTexture)
            {
                chosen = item.Type;
            }
            else if (item.Type != chosen)
            {
                Native.Release(item.Type);
            }
        }

        // 想要纹理类型但一个都没有时，退回普通类型，让 SET_INPUT_TYPE 的返回值说话。
        if (chosen == IntPtr.Zero && wantTexture && collected.Count > 0)
        {
            foreach (var item in collected)
            {
                if (item.Dxgi == 0)
                {
                    chosen = item.Type;
                    break;
                }
            }

            foreach (var item in collected)
            {
                if (item.Type != chosen)
                {
                    Native.Release(item.Type);
                }
            }
        }

        return chosen;
    }

    private static uint ReadDxgiFormat(IntPtr mediaType)
    {
        var dump = DumpAttributes(mediaType);

        foreach (var part in dump.Split(' '))
        {
            var marker = ":vt19:";
            var at = part.IndexOf(marker, StringComparison.Ordinal);

            if (at < 0)
            {
                continue;
            }

            if (uint.TryParse(part[(at + marker.Length)..], out var value)
                && (value == Native.DxgiFormatNv12 || value == Native.DxgiFormatB8G8R8A8Unorm))
            {
                return value;
            }
        }

        return 0;
    }

    /// <summary>克隆编码器给的输入类型，只改尺寸——2.0 需要按用户选的分辨率驱动它。</summary>
    private static IntPtr CloneWithTypeSize(IntPtr source, int width, int height)
    {
        if (Native.MFCreateMediaType(out var clone) < 0)
        {
            return IntPtr.Zero;
        }

        var copyHr = Native.Invoke<Native.CopyAllItemsDelegate>(clone, Native.SlotAttributesCopyAllItems)(
            clone,
            source);

        Say("CLONE_INPUT_HR", Native.Hex(copyHr));

        if (copyHr < 0)
        {
            Native.Release(clone);
            return IntPtr.Zero;
        }

        Native.SetUInt64(clone, Native.MFMTFrameSize, Native.Pack((uint)width, (uint)height));
        Native.SetUInt64(clone, Native.MFMTFrameRate, Native.Pack(30, 1));
        Native.SetUInt32(clone, Native.MFMTDefaultStride, (uint)width);
        Native.SetUInt32(clone, Native.MFMTSampleSize, (uint)(width * height * 3 / 2));
        return clone;
    }

    private static IntPtr FindH264OutputType(IntPtr transform, Target target)
    {
        for (var index = 0u; index < 64; index++)
        {
            var hr = Native.Invoke<Native.GetAvailableTypeDelegate>(
                transform,
                Native.SlotTransformGetOutputAvailableType)(transform, 0, index, out var type);

            if (hr < 0)
            {
                break;
            }

            var subtypeKey = Native.MFMTSubtype;
            var guidHr = Native.Invoke<Native.GetGuidDelegate>(
                type,
                Native.SlotAttributesGetGUID)(type, ref subtypeKey, out var subtype);

            var isH264 = guidHr >= 0 && subtype == Native.MFVideoFormatH264;

            if (!isH264)
            {
                Native.Release(type);
                continue;
            }

            if (Native.MFCreateMediaType(out var clone) < 0)
            {
                Native.Release(type);
                return IntPtr.Zero;
            }

            var copyHr = Native.Invoke<Native.CopyAllItemsDelegate>(clone, Native.SlotAttributesCopyAllItems)(
                clone,
                type);

            Say("CLONE_OUTPUT_HR", Native.Hex(copyHr));
            Native.Release(type);

            if (copyHr < 0)
            {
                Native.Release(clone);
                return IntPtr.Zero;
            }

            var bitrate = (uint)Math.Clamp(
                (long)(target.Width * target.Height * 0.11 * target.Frames),
                1_000_000,
                45_000_000);

            Native.SetUInt32(clone, Native.MFMTAvgBitrate, bitrate);
            Native.SetUInt32(clone, Native.MFMTMpeg2Profile, 77);
            Say("OUTPUT_TYPE_ATTRS", DumpAttributes(clone));
            return clone;
        }

        return IntPtr.Zero;
    }

    private static IntPtr CreateNv12Texture(
        IntPtr device,
        int width,
        int height,
        uint format = Native.DxgiFormatNv12)
    {
        var description = new Native.D3d11Texture2DDescription
        {
            Width = (uint)width,
            Height = (uint)height,
            MipLevels = 1,
            ArraySize = 1,
            Format = format,
            SampleCount = 1,
            SampleQuality = 0,
            Usage = Native.D3d11UsageDefault,
            BindFlags = Native.D3d11BindShaderResource,
            CpuAccessFlags = 0,
            MiscFlags = 0
        };

        var hr = Native.Invoke<Native.CreateTexture2DDelegate>(
            device,
            Native.SlotDeviceCreateTexture2D)(device, ref description, IntPtr.Zero, out var texture);

        Say("CREATE_TEXTURE_HR", Native.Hex(hr));
        return hr >= 0 ? texture : IntPtr.Zero;
    }

    private static IntPtr CreateNv12SystemBuffer(int width, int height)
    {
        var length = width * height * 3 / 2;
        var hr = Native.MFCreateMemoryBuffer((uint)length, out var buffer);
        Say("MEMORY_BUFFER_HR", Native.Hex(hr));

        if (hr < 0)
        {
            return IntPtr.Zero;
        }

        Native.Invoke<Native.SetCurrentLengthDelegate>(buffer, Native.SlotMediaBufferSetCurrentLength)(
            buffer,
            (uint)length);

        return buffer;
    }

    private static int Pump(
        IntPtr transform,
        IntPtr buffer,
        Target target,
        bool providesSamples,
        string label)
    {
        var stopwatch = Stopwatch.StartNew();
        var beforeWorkingSet = Environment.WorkingSet;
        var accepted = 0;
        var produced = 0;
        var encodedBytes = 0L;
        var firstInputHr = 0;
        var firstOutputHr = 0;

        for (var frame = 0; frame < target.Frames; frame++)
        {
            if (Native.MFCreateSample(out var sample) < 0)
            {
                Say("OUTCOME", "SAMPLE-FAILED");
                return 2;
            }

            Native.Invoke<Native.AddBufferDelegate>(sample, Native.SlotSampleAddBuffer)(sample, buffer);
            Native.Invoke<Native.SetTimeDelegate>(sample, Native.SlotSampleSetSampleTime)(
                sample,
                frame * 10_000_000L / Math.Max(1, target.Frames));
            Native.Invoke<Native.SetTimeDelegate>(sample, Native.SlotSampleSetSampleDuration)(
                sample,
                10_000_000L / Math.Max(1, target.Frames));

            var inputHr = Native.Invoke<Native.ProcessInputDelegate>(
                transform,
                Native.SlotTransformProcessInput)(transform, 0, sample, 0);

            if (frame == 0)
            {
                firstInputHr = inputHr;
            }

            Native.Release(sample);

            if (inputHr < 0)
            {
                Say("INPUT_HR_LATE", Native.Hex(inputHr) + "@frame" + frame);
                break;
            }

            accepted++;

            var outputBuffer = new Native.MftOutputDataBuffer
            {
                StreamId = 0,
                Sample = providesSamples ? IntPtr.Zero : AllocateOutputSample(target)
            };

            var outputHr = Native.Invoke<Native.ProcessOutputDelegate>(
                transform,
                Native.SlotTransformProcessOutput)(transform, 0, 1, ref outputBuffer, out _);

            if (frame == 0)
            {
                firstOutputHr = outputHr;
            }

            if (outputHr >= 0 && outputBuffer.Sample != IntPtr.Zero)
            {
                produced++;
                encodedBytes += MeasureEncodedBytes(outputBuffer.Sample);
            }

            Native.Release(outputBuffer.Sample);
        }

        stopwatch.Stop();

        Say("INPUT_HR_FIRST", Native.Hex(firstInputHr));
        Say("OUTPUT_HR_FIRST", Native.Hex(firstOutputHr));
        Say("ACCEPTED", accepted + "/" + target.Frames);
        Say("PRODUCED", produced.ToString());
        Say("ENCODED_BYTES", encodedBytes.ToString());
        Say("ELAPSED_MS", stopwatch.ElapsedMilliseconds.ToString());
        Say(
            "WORKING_SET_MB",
            beforeWorkingSet / 1048576 + "->" + Environment.WorkingSet / 1048576);

        var success = firstInputHr >= 0 && produced > 0 && encodedBytes > 0;
        Say("OUTCOME", success ? label + "-ENCODED" : label + "-FAILED");
        Say("DXGI_FORMAT_KEY", DxgiFormatKey == Guid.Empty ? "(not-seen)" : DxgiFormatKey.ToString("B").ToUpperInvariant());
        return success ? 0 : 2;
    }

    private static IntPtr AllocateOutputSample(Target target)
    {
        if (Native.MFCreateMemoryBuffer((uint)(target.Width * target.Height * 3 / 2), out var memory) < 0)
        {
            return IntPtr.Zero;
        }

        Native.Invoke<Native.SetCurrentLengthDelegate>(memory, Native.SlotMediaBufferSetCurrentLength)(
            memory,
            0);

        var sample = IntPtr.Zero;

        if (Native.MFCreateSample(out sample) >= 0)
        {
            Native.Invoke<Native.AddBufferDelegate>(sample, Native.SlotSampleAddBuffer)(sample, memory);
        }

        Native.Release(memory);
        return sample;
    }

    private static long MeasureEncodedBytes(IntPtr sample)
    {
        if (Native.Invoke<Native.ConvertToContiguousBufferDelegate>(
            sample,
            Native.SlotSampleConvertToContiguousBuffer)(sample, out var contiguous) < 0)
        {
            return 0;
        }

        Native.Invoke<Native.GetCurrentLengthDelegate>(
            contiguous,
            Native.SlotMediaBufferGetCurrentLength)(contiguous, out var length);

        Native.Release(contiguous);
        return length;
    }

    /// <summary>按序号取 DXGI 适配器，同时打印厂商与设备号，用来和编码器属性里的 VEN_xxxx 对齐。</summary>
    private static IntPtr OpenAdapter(int index, out IntPtr factory)
    {
        factory = IntPtr.Zero;
        var iid = Native.IIDIDXGIFactory1;

        if (Native.CreateDXGIFactory1(ref iid, out factory) < 0)
        {
            Say("FACTORY_HR", "negative");
            return IntPtr.Zero;
        }

        var hr = Native.Invoke<Native.EnumAdapters1Delegate>(
            factory,
            Native.SlotFactoryEnumAdapters1)(factory, (uint)index, out var adapter);

        Say("ENUM_ADAPTER" + index, Native.Hex(hr));

        if (hr < 0 || adapter == IntPtr.Zero)
        {
            return IntPtr.Zero;
        }

        // 手工按 DXGI_ADAPTER_DESC1 的字节布局读，避免非 blittable 结构体封送带来的不确定性。
        // WCHAR Description[128] | uint VendorId | uint DeviceId | uint SubSysId | uint Revision
        // | SIZE_T x3 | LUID | uint Flags
        var buffer = Marshal.AllocHGlobal(312);

        try
        {
            var descHr = Native.Invoke<Native.GetAdapterDesc1Delegate>(
                adapter,
                Native.SlotAdapterGetDesc1)(adapter, buffer);

            if (descHr >= 0)
            {
                var name = Marshal.PtrToStringUni(buffer, 128)?.Split('\0')[0] ?? string.Empty;

                Say(
                    "ADAPTER" + index,
                    "VEN_" + Marshal.ReadInt32(buffer, 256).ToString("X4")
                    + " DEV_" + Marshal.ReadInt32(buffer, 260).ToString("X4")
                    + " " + name);
            }
            else
            {
                Say("ADAPTER" + index + "_DESC_HR", Native.Hex(descHr));
            }
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }

        var candidate = Native.IIDIDXGIAdapter1;
        var asAdapter1 = IntPtr.Zero;
        var qi = Native.QueryInterface(adapter, candidate, out asAdapter1);

        if (qi >= 0)
        {
            Native.Release(adapter);
            return asAdapter1;
        }

        return adapter;
    }

    /// <summary>写入器实际选中的编码器：GetServiceForStream 拿回 MFT，再 dump 它的属性。</summary>
    private static void ReportChosenEncoder(IntPtr writer, uint streamIndex)
    {
        var service = Guid.Empty;
        var riid = Native.IIDIMFTransform;
        var hr = Native.Invoke<Native.GetServiceForStreamDelegate>(
            writer,
            Native.SlotSinkWriterGetServiceForStream)(writer, streamIndex, ref service, ref riid, out var mft);

        Say("SERVICE_HR", Native.Hex(hr));

        if (hr < 0 || mft == IntPtr.Zero)
        {
            return;
        }

        var attributes = IntPtr.Zero;
        var attrHr = Native.Invoke<Native.GetOnePointerDelegate>(
            mft,
            Native.SlotTransformGetAttributes)(mft, out attributes);

        // MFT 的属性表里若带友好名，就能看出写入器选的是哪一家编码器。
        Say("CHOSEN_ENCODER", attrHr >= 0 ? DumpAttributes(attributes) : "(no attributes " + Native.Hex(attrHr) + ")");

        Native.Release(attributes);
        Native.Release(mft);
    }

    // ---- SinkWriter 路径（2.0 实际会用的那条） ----

    private static int RunWriter(Target target)
    {
        Say("MODE", "writer-" + (target.Texture ? "texture" : "cpu"));
        Say("HARDWARE", target.Hardware ? "1" : "0");
        Say("SIZE", target.Width + "x" + target.Height + "@" + target.Frames + "f");

        var path = System.IO.Path.Combine(
            System.IO.Path.GetTempPath(),
            "halocap-spike-" + Guid.NewGuid().ToString("N") + ".mp4");

        var device = IntPtr.Zero;
        var context = IntPtr.Zero;
        var manager = IntPtr.Zero;
        var attributes = IntPtr.Zero;
        var outputType = IntPtr.Zero;
        var inputType = IntPtr.Zero;
        var writer = IntPtr.Zero;
        var pool = new IntPtr[3];

        var stopwatch = Stopwatch.StartNew();
        var beforeWorkingSet = Environment.WorkingSet;
        var written = 0;
        var failures = 0;
        var firstFailureHr = 0;
        var outcome = "UNKNOWN";
        var code = 2;

        Say("MFSTARTUP_HR", Native.Hex(Native.MFStartup(Native.MfVersion, 0)));

        try
        {
            if (target.Texture)
            {
                var adapter = IntPtr.Zero;
                var factory = IntPtr.Zero;

                if (target.Adapter >= 0)
                {
                    adapter = OpenAdapter(target.Adapter, out factory);
                    Say("ADAPTER_INDEX", target.Adapter + (adapter == IntPtr.Zero ? " unavailable" : ""));

                    if (adapter == IntPtr.Zero)
                    {
                        outcome = "ADAPTER-UNAVAILABLE";
                        return code;
                    }
                }

                // 指定适配器时驱动类型必须是 UNKNOWN，否则 DXGI 适配器会被忽略。
                var deviceHr = Native.D3D11CreateDevice(
                    adapter,
                    adapter == IntPtr.Zero ? Native.D3dDriverTypeHardware : 0u,
                    IntPtr.Zero,
                    Native.D3d11CreateDeviceBgraSupport,
                    IntPtr.Zero,
                    0,
                    Native.D3d11SdkVersion,
                    out device,
                    IntPtr.Zero,
                    out context);

                Say("D3D11CREATE_HR", Native.Hex(deviceHr));
                Native.Release(adapter);
                Native.Release(factory);

                if (deviceHr < 0)
                {
                    outcome = "D3D11-DEVICE-FAILED";
                    return code;
                }

                var managerHr = Native.MFCreateDXGIDeviceManager(out var resetToken, out manager);
                Say("DXGI_MANAGER_HR", Native.Hex(managerHr));

                if (managerHr < 0)
                {
                    outcome = "DXGI-MANAGER-FAILED";
                    return code;
                }

                Say("RESET_DEVICE_HR", Native.Hex(
                    Native.Invoke<Native.ResetDeviceDelegate>(
                        manager,
                        Native.SlotDxgiManagerResetDevice)(manager, device, resetToken)));

                for (var i = 0; i < pool.Length; i++)
                {
                    pool[i] = CreateNv12Texture(
                        device,
                        target.Width,
                        target.Height,
                        IsBgra(target) ? Native.DxgiFormatB8G8R8A8Unorm : Native.DxgiFormatNv12);

                    if (pool[i] == IntPtr.Zero)
                    {
                        outcome = "TEXTURE-FAILED";
                        return code;
                    }
                }
            }

            if (Native.MFCreateAttributes(out attributes, 4) < 0)
            {
                outcome = "ATTRIBUTES-FAILED";
                return code;
            }

            Say("ATTR_HW_HR", Native.Hex(
                Native.SetUInt32(
                    attributes,
                    Native.MFReadWriteEnableHardwareTransforms,
                    target.Hardware ? 1u : 0u)));

            Say("ATTR_THROTTLE_HR", Native.Hex(
                Native.SetUInt32(attributes, Native.MFSinkWriterDisableThrottling, 1u)));

            if (target.Texture)
            {
                Say("ATTR_D3D_MANAGER_HR", Native.Hex(
                    Native.SetUnknown(attributes, Native.MFSinkWriterD3DManager, manager)));
            }

            Say("WRITER_CREATE_HR", Native.Hex(
                Native.MFCreateSinkWriterFromURL(path, IntPtr.Zero, attributes, out writer)));

            if (writer == IntPtr.Zero)
            {
                outcome = "WRITER-CREATE-FAILED";
                return code;
            }

            outputType = BuildWriterType(target, forInput: false);
            var addHr = Native.Invoke<Native.AddStreamDelegate>(
                writer,
                Native.SlotSinkWriterAddStream)(writer, outputType, out var streamIndex);

            Say("ADDSTREAM_HR", Native.Hex(addHr) + " index " + streamIndex);

            if (addHr < 0)
            {
                outcome = "ADDSTREAM-FAILED";
                return code;
            }

            ReportChosenEncoder(writer, streamIndex);

            inputType = BuildWriterType(target, forInput: true);
            var setInputHr = Native.Invoke<Native.SetInputMediaTypeDelegate>(
                writer,
                Native.SlotSinkWriterSetInputMediaType)(writer, streamIndex, inputType, IntPtr.Zero);

            Say("SET_INPUT_MEDIA_TYPE_HR", Native.Hex(setInputHr));

            if (setInputHr < 0)
            {
                outcome = "SET-INPUT-TYPE-FAILED";
                return code;
            }

            var beginHr = Native.Invoke<Native.NoArgDelegate>(
                writer,
                Native.SlotSinkWriterBeginWriting)(writer);

            Say("BEGIN_WRITING_HR", Native.Hex(beginHr));

            if (beginHr < 0)
            {
                outcome = "BEGIN-WRITING-FAILED";
                return code;
            }

            var duration = 10_000_000L / 30;

            for (var frame = 0; frame < target.Frames; frame++)
            {
                var buffer = IntPtr.Zero;
                var sample = IntPtr.Zero;

                try
                {
                    if (target.Texture)
                    {
                        var riid = Native.IIDID3D11Texture2D;
                        var surfaceHr = Native.MFCreateDXGISurfaceBuffer(
                            ref riid,
                            pool[frame % pool.Length],
                            0,
                            0,
                            out buffer);

                        if (frame == 0)
                        {
                            Say("SURFACE_BUFFER_HR", Native.Hex(surfaceHr));

                            var lengthHr = Native.Invoke<Native.GetCurrentLengthDelegate>(
                                buffer,
                                Native.SlotMediaBufferGetCurrentLength)(buffer, out var surfaceLength);

                            Say(
                                "SURFACE_CURRENT_LENGTH",
                                (lengthHr >= 0 ? surfaceLength.ToString() : "hr " + Native.Hex(lengthHr))
                                + " expected " + FrameBytes(target));
                        }

                        // 参考实现要求显式写上样本长度，否则写入器按空缓冲拒收。
                        if (target.Variant == 7 || target.Variant == 8)
                        {
                            Native.Invoke<Native.SetCurrentLengthDelegate>(
                                buffer,
                                Native.SlotMediaBufferSetCurrentLength)(
                                buffer,
                                (uint)FrameBytes(target));
                        }

                        if (surfaceHr < 0)
                        {
                            Say("OUTCOME_EARLY", "SURFACE-BUFFER-FAILED " + Native.Hex(surfaceHr));
                            outcome = "SURFACE-BUFFER-FAILED";
                            return code;
                        }
                    }
                    else
                    {
                        buffer = CreateNv12SystemBuffer(target.Width, target.Height);
                    }

                    if (Native.MFCreateSample(out sample) < 0)
                    {
                        outcome = "SAMPLE-FAILED";
                        return code;
                    }

                    Native.Invoke<Native.AddBufferDelegate>(sample, Native.SlotSampleAddBuffer)(sample, buffer);
                    Native.Invoke<Native.SetTimeDelegate>(sample, Native.SlotSampleSetSampleTime)(
                        sample,
                        frame * duration);
                    Native.Invoke<Native.SetTimeDelegate>(sample, Native.SlotSampleSetSampleDuration)(
                        sample,
                        duration);

                    if (target.Texture && target.Variant >= 1)
                    {
                        Native.SetUInt32(sample, Native.MFSaD3D11Aware, 1u);
                    }

                    if (target.Texture && target.Variant >= 2)
                    {
                        Native.SetUInt32(sample, Native.MFSAD3D11BindFlags, Native.D3d11BindShaderResource);
                    }

                    var writeHr = Native.Invoke<Native.WriteSampleDelegate>(
                        writer,
                        Native.SlotSinkWriterWriteSample)(writer, streamIndex, sample, 0);

                    if (writeHr >= 0)
                    {
                        written++;
                    }
                    else
                    {
                        failures++;

                        if (firstFailureHr == 0)
                        {
                            firstFailureHr = writeHr;
                        }
                    }
                }
                finally
                {
                    Native.Release(sample);
                    Native.Release(buffer);
                }
            }

            Say("FINISH_WRITING_HR", Native.Hex(
                Native.Invoke<Native.NoArgDelegate>(writer, Native.SlotSinkWriterFinishWriting)(writer)));

            outcome = written > 0 ? "WRITER-ENCODED" : "WRITER-FAILED";
            code = written > 0 ? 0 : 2;
            return code;
        }
        finally
        {
            foreach (var texture in pool)
            {
                Native.Release(texture);
            }

            Native.Release(writer);
            Native.Release(inputType);
            Native.Release(outputType);
            Native.Release(attributes);
            Native.Release(manager);
            Native.Release(context);
            Native.Release(device);
            Native.MFShutdown();

            stopwatch.Stop();

            var cpuMs = (long)Process.GetCurrentProcess().TotalProcessorTime.TotalMilliseconds;

            Say("CPU_MS_TOTAL", cpuMs.ToString());

            var bytes = System.IO.File.Exists(path) ? new System.IO.FileInfo(path).Length : -1L;

            // 只有真的写出字节才算成功：样本被接受但最终化失败的写法会产出 0 字节文件。
            if (outcome == "WRITER-ENCODED" && bytes <= 0)
            {
                outcome = "WRITER-NO-BYTES";
                code = 2;
            }

            Say("WRITTEN", written + "/" + target.Frames);
            Say("FAILED_WRITES", failures + " first " + Native.Hex(firstFailureHr));
            Say("ELAPSED_MS", stopwatch.ElapsedMilliseconds.ToString());
            Say("FILE_BYTES", bytes.ToString());
            Say(
                "WORKING_SET_MB",
                beforeWorkingSet / 1048576 + "->" + Environment.WorkingSet / 1048576);
            Say("OUTCOME", outcome);

            if (System.IO.File.Exists(path))
            {
                try
                {
                    System.IO.File.Delete(path);
                }
                catch
                {
                    // 临时文件删不掉不影响结论。
                }
            }
        }
    }

    /// <summary>变体 5 / 8 用 BGRA 纹理输入，7 / 8 需要显式写样本长度。</summary>
    private static bool IsBgra(Target target) => target.Variant is 5 or 8;

    private static int FrameBytes(Target target) => IsBgra(target)
        ? target.Width * target.Height * 4
        : target.Width * target.Height * 3 / 2;

    /// <summary>按 wcap 的做法：输入 NV12 只写主要属性，输出 H.264 带码率与档位。</summary>
    private static IntPtr BuildWriterType(Target target, bool forInput)
    {
        if (Native.MFCreateMediaType(out var type) < 0)
        {
            return IntPtr.Zero;
        }

        var bytesPerPixel = forInput && IsBgra(target) ? 4 : 1;
        var major = Native.MFMediaTypeVideo;
        var subtype = forInput
            ? (IsBgra(target) ? Native.MFVideoFormatRgb32 : Native.MFVideoFormatNv12)
            : Native.MFVideoFormatH264;

        Native.SetGuid(type, Native.MFMTMajorType, major);
        Native.SetGuid(type, Native.MFMTSubtype, subtype);
        Native.SetUInt32(type, Native.MFMTInterlaceMode, 2);
        Native.SetUInt64(type, Native.MFMTFrameRate, Native.Pack(30, 1));
        Native.SetUInt64(type, Native.MFMTFrameSize, Native.Pack((uint)target.Width, (uint)target.Height));

        if (forInput)
        {
            // 参考实现（wcap / makepad）不写这两个属性，用变体逐个试出编码器要什么。
            if (target.Variant != 4)
            {
                Native.SetUInt32(type, Native.MFMTDefaultStride, (uint)(target.Width * bytesPerPixel));
            }

            if (target.Variant < 3)
            {
                var sampleSize = bytesPerPixel == 4
                    ? target.Width * target.Height * 4
                    : target.Width * target.Height * 3 / 2;

                Native.SetUInt32(type, Native.MFMTSampleSize, (uint)sampleSize);
            }
        }
        else
        {
            Native.SetUInt32(type, Native.MFMTMpeg2Profile, 77);
            Native.SetUInt32(
                type,
                Native.MFMTAvgBitrate,
                (uint)Math.Clamp(
                    (long)(target.Width * target.Height * 0.11 * target.Frames),
                    1_000_000,
                    45_000_000));
        }

        return type;
    }

    /// <summary>逐个 DXGI 适配器分别起子进程跑纹理直入，避免一块卡的崩溃带走整份报告。</summary>
    private static int RunAdapters(Target target)
    {
        var exe = Environment.ProcessPath ?? "HaloCap.EncoderD3DSpike.exe";

        for (var index = 0; index < 8; index++)
        {
            var factory = IntPtr.Zero;
            var adapter = OpenAdapter(index, out factory);
            Native.Release(factory);

            if (adapter == IntPtr.Zero)
            {
                break;
            }

            Native.Release(adapter);

            var arguments = "writer "
                + (target.Hardware ? "hw" : "sw") + " "
                + (target.Texture ? "texture" : "cpu") + " "
                + target.Width + " " + target.Height + " " + target.Frames + " "
                + target.Variant + " " + index;

            Console.WriteLine("adapter" + index + " " + RunChild(exe, arguments));
        }

        return 0;
    }

    // ---- 编排 ----

    private static int RunAll(Target target)
    {
        var exe = Environment.ProcessPath ?? "HaloCap.EncoderD3DSpike.exe";

        Say("MFSTARTUP_HR", Native.Hex(Native.MFStartup(Native.MfVersion, 0)));
        Enumerate(true, out var hardwareCount);
        Enumerate(false, out var softwareCount);
        Native.MFShutdown();

        Say("HW_COUNT", hardwareCount.ToString());
        Say("SW_COUNT", softwareCount.ToString());

        for (var kind = 0; kind < 2; kind++)
        {
            var tag = kind == 0 ? "hw" : "sw";
            var count = kind == 0 ? hardwareCount : softwareCount;

            for (var index = 0u; index < count; index++)
            {
                foreach (var mode in new[] { "probe", "control" })
                {
                    var arguments = mode + " " + tag + " " + index
                        + " " + target.Width + " " + target.Height + " " + target.Frames;

                    Console.WriteLine(tag + index + " " + mode.PadRight(8) + RunChild(exe, arguments));
                }
            }
        }

        return 0;
    }

    private static string Shorten(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return "-";
        }

        // 只留友好名那一段，方便一行看完一台机器的结论。
        const string FriendlyNameKey = "{314FFBAE-5B41-4C95-9C19-4E7D586FACE3}:vt31:";
        var at = value.IndexOf(FriendlyNameKey, StringComparison.OrdinalIgnoreCase);
        var text = at >= 0 ? value[(at + FriendlyNameKey.Length)..] : value;
        var cut = text.Length > 72 ? text[..72] : text;

        return cut.Trim();
    }

    private static string RunChild(string exe, string arguments)
    {
        var info = new ProcessStartInfo
        {
            FileName = exe,
            Arguments = arguments,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
            CreateNoWindow = true
        };

        using var process = Process.Start(info);

        if (process is null)
        {
            return "SPAWN-FAILED";
        }

        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        process.WaitForExit();

        var fields = new System.Collections.Generic.Dictionary<string, string>();

        foreach (var raw in output.Split('\n'))
        {
            var line = raw.Trim();
            var split = line.IndexOf('=');

            if (split > 0)
            {
                fields[line[..split]] = line[(split + 1)..];
            }
        }

        string Get(string key) => fields.TryGetValue(key, out var value) ? value : "-";

        var adapter = "-";

        foreach (var pair in fields)
        {
            if (pair.Key.StartsWith("ADAPTER", StringComparison.Ordinal) && pair.Key != "ADAPTER_INDEX")
            {
                adapter = pair.Key.Substring(7) + " " + Shorten(pair.Value);
            }
        }

        var exit = "exit 0x" + process.ExitCode.ToString("X8");
        var tail = error.Trim().Length > 0 ? " stderr:" + error.Replace("\n", " ").Trim() : string.Empty;

        return string.Join(
            " ",
            "outcome=" + Get("OUTCOME"),
            "adapter=" + adapter,
            "chosen=" + Shorten(Get("CHOSEN_ENCODER")),
            "setD3D=" + Get("SET_D3D_MANAGER_HR"),
            "inputPatched=" + Get("SET_INPUT_PATCHED_HR"),
            "inputAsIs=" + Get("SET_INPUT_ASIS_HR"),
            "output=" + Get("SET_OUTPUT_TYPE_HR"),
            "processInput=" + Get("INPUT_HR_FIRST"),
            "processOutput=" + Get("OUTPUT_HR_FIRST"),
            "accepted=" + Get("ACCEPTED"),
            "produced=" + Get("PRODUCED"),
            "bytes=" + Get("ENCODED_BYTES"),
            "ms=" + Get("ELAPSED_MS"),
            "workingSet=" + Get("WORKING_SET_MB"),
            "dxgiBacked=" + Get("DXGI_BACKED"),
            "dxgiKey=" + Get("DXGI_FORMAT_KEY"),
            exit + tail);
    }
}
