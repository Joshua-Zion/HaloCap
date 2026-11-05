using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace HaloCap.Spikes3;

/// <summary>
/// S0 spike 3：离屏 Direct2D 覆盖层的帧时间与回读成本。
///   measure [宽] [高] [帧数] [图元数]
/// 不建窗口、不上屏：渲染目标是常驻的 4K D3D11 纹理，回读走 STAGING 纹理。
/// </summary>
internal static class Program
{
    private const uint CpuAccessRead = 0x00020000;

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;

        // args[0] 是命令名，位置参数从 args[1] 起算。
        var width = Args(args, 1, 3840);
        var height = Args(args, 2, 2160);
        var frames = Args(args, 3, 120);
        var primitives = Args(args, 4, 300);

        Say("SIZE", width + "x" + height);
        Say("FRAMES", frames.ToString());
        Say("PRIMITIVES_PER_FRAME", primitives.ToString());

        var hr = Com.D3D11CreateDevice(
            IntPtr.Zero,
            Com.D3dDriverTypeHardware,
            IntPtr.Zero,
            Com.D3d11CreateDeviceBgraSupport,
            IntPtr.Zero,
            0,
            Com.D3d11SdkVersion,
            out var device,
            IntPtr.Zero,
            out var context);

        Say("D3D11CREATE_HR", Com.Hex(hr));

        if (hr < 0)
        {
            return 2;
        }

        var target = CreateTexture(device, width, height, Com.D3d11UsageDefault, Com.D3d11BindRenderTarget | Com.D3d11BindShaderResource, 0);
        var staging = CreateTexture(device, width, height, Com.D3d11UsageStaging, 0, CpuAccessRead);
        var surface = IntPtr.Zero;
        var dxgiDevice = IntPtr.Zero;
        var factory = IntPtr.Zero;
        var d2dDevice = IntPtr.Zero;
        var dc = IntPtr.Zero;
        var targetBitmap = IntPtr.Zero;
        var sourceBitmap = IntPtr.Zero;
        var brushLine = IntPtr.Zero;
        var brushFill = IntPtr.Zero;
        var brushText = IntPtr.Zero;
        var textFormat = IntPtr.Zero;
        var dwrite = IntPtr.Zero;
        var pixels = IntPtr.Zero;

        try
        {
            if (target == IntPtr.Zero || staging == IntPtr.Zero)
            {
                Say("OUTCOME", "TEXTURE-FAILED");
                return 2;
            }

            Say("QI_SURFACE_HR", Com.Hex(Com.QueryInterface(target, Com.IIDIDXGISurface, out surface)));
            Say("QI_DXGIDEVICE_HR", Com.Hex(Com.QueryInterface(device, Com.IIDIDXGIDevice, out dxgiDevice)));

            var iidFactory = Com.IIDID2D1Factory1;
            Say("D2D1_FACTORY_HR", Com.Hex(Com.D2D1CreateFactory(Com.D2dFactorySingleThreaded, ref iidFactory, IntPtr.Zero, out factory)));

            var iidDWrite = Com.IIDIDWriteFactory;
            Say("DWRITE_FACTORY_HR", Com.Hex(Com.DWriteCreateFactory(Com.DwriteFactoryShared, ref iidDWrite, out dwrite)));

            if (factory == IntPtr.Zero || dwrite == IntPtr.Zero)
            {
                Say("OUTCOME", "FACTORY-FAILED");
                return 2;
            }

            // 走 D2D 1.0 的离屏路径：把 4K DXGI 纹理直接做成渲染目标，不需要 ID2D1Device。
            var rtProperties = new Com.RenderTargetProperties
            {
                Type = 0,
                PixelFormat = new Com.PixelFormat
                {
                    Format = Com.DxgiFormatB8G8R8A8Unorm,
                    AlphaMode = Com.D2dAlphaModePremultiplied
                },
                DpiX = 96f,
                DpiY = 96f,
                Usage = 0,
                MinLevel = 0
            };

            Say("CREATE_RT_HR", Com.Hex(
                Com.Invoke<Com.CreateDxgiSurfaceRenderTargetDelegate>(
                    factory,
                    Com.SlotFactoryCreateDxgiSurfaceRenderTarget)(factory, surface, ref rtProperties, out dc)));

            if (dc == IntPtr.Zero)
            {
                Say("OUTCOME", "RENDER-TARGET-FAILED");
                return 2;
            }

            // 「冻结的屏幕画面」：一帧 BGRA 位图，2.0 里它就是选区背景。
            var pitch = width * 4;
            pixels = Marshal.AllocHGlobal(pitch * height);

            for (var y = 0; y < height; y++)
            {
                var row = pixels + (y * pitch);

                for (var x = 0; x < width; x++)
                {
                    var value = (byte)(((x >> 3) + (y >> 3)) & 0xFF);
                    Marshal.WriteByte(row, x * 4, value);
                    Marshal.WriteByte(row, x * 4 + 1, (byte)(255 - value));
                    Marshal.WriteByte(row, x * 4 + 2, 128);
                    Marshal.WriteByte(row, x * 4 + 3, 255);
                }
            }

            var lineColor = new Com.ColorF { R = 1f, G = 0.2f, B = 0.2f, A = 1f };
            var fillColor = new Com.ColorF { R = 0.1f, G = 0.6f, B = 1f, A = 0.35f };
            var textColor = new Com.ColorF { R = 1f, G = 1f, B = 1f, A = 1f };
            var sourceProps = new Com.BitmapProperties
            {
                PixelFormat = new Com.PixelFormat
                {
                    Format = Com.DxgiFormatB8G8R8A8Unorm,
                    AlphaMode = Com.D2dAlphaModePremultiplied
                },
                DpiX = 96f,
                DpiY = 96f
            };
            var size = new Com.SizeU { Width = (uint)width, Height = (uint)height };

            Say("SOURCE_BITMAP_HR", Com.Hex(
                Com.Invoke<Com.RtCreateBitmapDelegate>(dc, Com.SlotRtCreateBitmap)(
                    dc,
                    size,
                    pixels,
                    (uint)pitch,
                    ref sourceProps,
                    out sourceBitmap)));

            if (sourceBitmap == IntPtr.Zero)
            {
                Say("OUTCOME", "SOURCE-BITMAP-FAILED");
                return 2;
            }

            Say("BRUSH_HR", Com.Hex(
                Com.Invoke<Com.CreateBrushDelegate>(dc, Com.SlotRtCreateSolidColorBrush)(
                    dc,
                    ref lineColor,
                    IntPtr.Zero,
                    out brushLine)));

            Com.Invoke<Com.CreateBrushDelegate>(dc, Com.SlotRtCreateSolidColorBrush)(
                dc,
                ref fillColor,
                IntPtr.Zero,
                out brushFill);

            Com.Invoke<Com.CreateBrushDelegate>(dc, Com.SlotRtCreateSolidColorBrush)(
                dc,
                ref textColor,
                IntPtr.Zero,
                out brushText);

            var iidFormat = Com.IIDIDWriteFactory;
            Say("TEXT_FORMAT_HR", Com.Hex(
                Com.Invoke<Com.CreateTextFormatDelegate>(dwrite, Com.SlotDWriteCreateTextFormat)(
                    dwrite,
                    "Segoe UI",
                    IntPtr.Zero,
                    Com.DwriteWeightNormal,
                    Com.DwriteStyleNormal,
                    Com.DwriteStretchNormal,
                    24f,
                    "en-US",
                    out textFormat)));

            Com.Invoke<Com.SetAntialiasModeDelegate>(dc, Com.SlotRtSetAntialiasMode)(dc, Com.D2dAntialiasPerPrimitive);

            GC.Collect();
            GC.WaitForPendingFinalizers();
            GC.Collect();

            var beforeFrames = Environment.WorkingSet / 1048576;

            var submit = MeasureSubmit(dc, context, target, staging, sourceBitmap, brushLine, brushFill, brushText, textFormat, width, height, frames, primitives);
            var readback = MeasureReadback(dc, context, target, staging, sourceBitmap, brushLine, brushFill, textFormat, width, height, Math.Min(frames, 60), primitives);

            Say("WORKING_SET_START_MB", beforeFrames.ToString());
            Say("WORKING_SET_END_MB", (Environment.WorkingSet / 1048576).ToString());
            Say(
                "SURFACE_ACCOUNTING_MB",
                "target " + width * height * 4 / 1048576
                + " staging " + width * height * 4 / 1048576
                + " cpu-source " + width * height * 4 / 1048576);
            Say("OUTCOME", submit > 0 ? "MEASURED" : "FAILED");

            return submit > 0 ? 0 : 2;
        }
        finally
        {
            Com.Release(textFormat);
            Com.Release(brushText);
            Com.Release(brushFill);
            Com.Release(brushLine);
            Com.Release(sourceBitmap);
            Com.Release(targetBitmap);
            Com.Release(dc);
            Com.Release(d2dDevice);
            Com.Release(factory);
            Com.Release(dwrite);
            Com.Release(surface);
            Com.Release(dxgiDevice);
            Com.Release(staging);
            Com.Release(target);
            Com.Release(context);
            Com.Release(device);

            if (pixels != IntPtr.Zero)
            {
                Marshal.FreeHGlobal(pixels);
            }
        }
    }

    private static void Say(string key, string value) => Console.WriteLine(key + "=" + value);

    private static int Args(string[] args, int index, int fallback) =>
        args.Length > index && int.TryParse(args[index], out var value) ? value : fallback;

    private static IntPtr CreateTexture(IntPtr device, int width, int height, uint usage, uint bindFlags, uint cpuAccess)
    {
        var description = new Com.Texture2DDescription
        {
            Width = (uint)width,
            Height = (uint)height,
            MipLevels = 1,
            ArraySize = 1,
            Format = Com.DxgiFormatB8G8R8A8Unorm,
            SampleCount = 1,
            SampleQuality = 0,
            Usage = usage,
            BindFlags = bindFlags,
            CpuAccessFlags = cpuAccess,
            MiscFlags = 0
        };

        var hr = Com.Invoke<Com.CreateTexture2DDel>(device, Com.SlotDeviceCreateTexture2D)(
            device,
            ref description,
            IntPtr.Zero,
            out var texture);

        Say("CREATE_TEXTURE_" + usage, Com.Hex(hr));
        return hr >= 0 ? texture : IntPtr.Zero;
    }

    private static double MeasureSubmit(
        IntPtr dc,
        IntPtr context,
        IntPtr target,
        IntPtr staging,
        IntPtr sourceBitmap,
        IntPtr brushLine,
        IntPtr brushFill,
        IntPtr brushText,
        IntPtr textFormat,
        int width,
        int height,
        int frames,
        int primitives)
    {
        var stopwatch = Stopwatch.StartNew();

        for (var frame = 0; frame < frames; frame++)
        {
            Com.Invoke<Com.NoArgDelegate>(dc, Com.SlotRtBeginDraw)(dc);
            DrawFrame(dc, sourceBitmap, brushLine, brushFill, brushText, textFormat, width, height, primitives, frame);
            Com.Invoke<Com.EndDrawDelegate>(dc, Com.SlotRtEndDraw)(dc, out _, out _);
        }

        // 队到 GPU 上的活要在这里才算完，所以把 Flush 与一次回读算进总时间。
        Com.Invoke<Com.FlushDelegate>(dc, Com.SlotRtFlush)(dc, out _, IntPtr.Zero);
        Com.Invoke<Com.CopyResourceDelegate>(context, Com.SlotContextCopyResource)(context, staging, target);
        Com.Invoke<Com.NoArgDelegate>(context, Com.SlotContextFlush)(context);

        var mapHr = Com.Invoke<Com.MapDelegate>(context, Com.SlotContextMap)(
            context,
            staging,
            0,
            Com.D3d11MapRead,
            0,
            out var mapped);

        if (mapHr >= 0)
        {
            Say("READBACK_MAP_HR", Com.Hex(mapHr) + " rowPitch " + mapped.RowPitch);
            Com.Invoke<Com.UnmapDelegate>(context, Com.SlotContextUnmap)(context, staging, 0);
        }
        else
        {
            Say("READBACK_MAP_HR", Com.Hex(mapHr));
        }

        stopwatch.Stop();

        var perFrame = stopwatch.Elapsed.TotalMilliseconds / frames;

        Say("SUBMIT_TOTAL_MS", stopwatch.Elapsed.TotalMilliseconds.ToString("F1"));
        Say("SUBMIT_MS_PER_FRAME", perFrame.ToString("F3"));

        return perFrame;
    }

    private static double MeasureReadback(
        IntPtr dc,
        IntPtr context,
        IntPtr target,
        IntPtr staging,
        IntPtr sourceBitmap,
        IntPtr brushLine,
        IntPtr brushFill,
        IntPtr textFormat,
        int width,
        int height,
        int frames,
        int primitives)
    {
        var stopwatch = Stopwatch.StartNew();

        for (var frame = 0; frame < frames; frame++)
        {
            Com.Invoke<Com.NoArgDelegate>(dc, Com.SlotRtBeginDraw)(dc);
            DrawFrame(dc, sourceBitmap, brushLine, brushFill, IntPtr.Zero, textFormat, width, height, primitives, frame);
            Com.Invoke<Com.EndDrawDelegate>(dc, Com.SlotRtEndDraw)(dc, out _, out _);

            Com.Invoke<Com.CopyResourceDelegate>(context, Com.SlotContextCopyResource)(context, staging, target);

            var mapHr = Com.Invoke<Com.MapDelegate>(context, Com.SlotContextMap)(
                context,
                staging,
                0,
                Com.D3d11MapRead,
                0,
                out var mapped);

            if (mapHr < 0)
            {
                stopwatch.Stop();
                Say("READBACK_HR", Com.Hex(mapHr));
                Say("READBACK_MS_PER_FRAME", "-");
                return 0;
            }

            // 真取一个像素，避免整个映射被优化掉。
            Marshal.ReadByte(mapped.Data, (int)mapped.RowPitch * (height / 2));
            Com.Invoke<Com.UnmapDelegate>(context, Com.SlotContextUnmap)(context, staging, 0);
        }

        stopwatch.Stop();

        var perFrame = stopwatch.Elapsed.TotalMilliseconds / frames;

        Say("READBACK_TOTAL_MS", stopwatch.Elapsed.TotalMilliseconds.ToString("F1"));
        Say("READBACK_MS_PER_FRAME", perFrame.ToString("F3"));

        return perFrame;
    }

    private static unsafe void DrawFrame(
        IntPtr dc,
        IntPtr sourceBitmap,
        IntPtr brushLine,
        IntPtr brushFill,
        IntPtr brushText,
        IntPtr textFormat,
        int width,
        int height,
        int primitives,
        int frame)
    {
        var full = new Com.RectF { Left = 0f, Top = 0f, Right = (float)width, Bottom = (float)height };
        Com.Invoke<Com.DrawBitmapDelegate>(dc, Com.SlotRtDrawBitmap)(
            dc,
            sourceBitmap,
            new IntPtr(&full),
            1f,
            Com.D2dBitmapInterpolationLinear,
            new IntPtr(&full));

        // 放大镜：从源位图里取一块 200x200 放大到 600x600。
        var source = new Com.RectF { Left = 400f, Top = 400f, Right = 600f, Bottom = 600f };
        var destination = new Com.RectF { Left = 80f, Top = 80f, Right = 680f, Bottom = 680f };
        Com.Invoke<Com.DrawBitmapDelegate>(dc, Com.SlotRtDrawBitmap)(
            dc,
            sourceBitmap,
            new IntPtr(&destination),
            1f,
            Com.D2dBitmapInterpolationLinear,
            IntPtr.Zero);

        var shift = frame % 64;

        for (var i = 0; i < primitives; i++)
        {
            var x = (i * 37 + shift) % (width - 200);
            var y = (i * 61) % (height - 200);

            var p0 = new Com.Point2F { X = x, Y = y };
            var p1 = new Com.Point2F { X = x + 140f, Y = y + 90f };
            Com.Invoke<Com.DrawLineDelegate>(dc, Com.SlotRtDrawLine)(dc, p0, p1, brushLine, 2.5f, IntPtr.Zero);

            var rect = new Com.RectF { Left = x + 20f, Top = y + 20f, Right = x + 120f, Bottom = y + 80f };

            if ((i & 1) == 0)
            {
                Com.Invoke<Com.FillRectangleDelegate>(dc, Com.SlotRtFillRectangle)(dc, ref rect, brushFill);
            }
            else
            {
                Com.Invoke<Com.DrawRectangleDelegate>(dc, Com.SlotRtDrawRectangle)(dc, ref rect, brushLine, 1.5f, IntPtr.Zero);
            }
        }

        if (brushText != IntPtr.Zero && textFormat != IntPtr.Zero)
        {
            var layout = new Com.RectF { Left = 200f, Top = 200f, Right = 1200f, Bottom = 320f };
            const string text = "HaloCap overlay spike 光圈覆盖层帧时间";
            Com.Invoke<Com.DrawTextDelegate>(dc, Com.SlotRtDrawText)(
                dc,
                text,
                (uint)text.Length,
                textFormat,
                ref layout,
                brushText,
                Com.D2dDrawTextOptionsNone,
                Com.DwriteMeasuringNatural);
        }
    }
}
