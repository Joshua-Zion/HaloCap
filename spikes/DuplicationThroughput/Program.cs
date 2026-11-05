using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace HaloCap.Spikes2;

/// <summary>
/// S0 spike 2：桌面复制（DXGI Desktop Duplication）在真实画面变化下的送帧率与脏矩形量级。
///   observe <秒> [adapter] [output]        只观察自然活动，不创建任何窗口
///   run <秒> <变化/秒> [adapter] [output] [宽] [高]   另开一个不抢焦点的小窗持续移动，制造可知变化
/// </summary>
internal static class Program
{
    private const int RectCapacity = 64;
    private static volatile bool _stop;
    private static long _movesIssued;

    private static int Main(string[] args)
    {
        Console.OutputEncoding = Encoding.UTF8;

        if (args.Length < 2)
        {
            return 1;
        }

        var mode = args[0].ToLowerInvariant();
        var seconds = int.TryParse(args[1], out var parsed) ? Math.Clamp(parsed, 1, 120) : 5;
        var fps = mode == "run" && args.Length >= 3 && int.TryParse(args[2], out var rate) ? rate : 0;
        var adapterWanted = ParseOptional(args, mode == "run" ? 3 : 2, -1);
        var outputWanted = ParseOptional(args, mode == "run" ? 4 : 3, 0);
        var windowWidth = ParseOptional(args, 5, 640);
        var windowHeight = ParseOptional(args, 6, 360);

        try
        {
            return Measure(seconds, fps, adapterWanted, outputWanted, windowWidth, windowHeight);
        }
        catch (Exception ex)
        {
            Say("FAIL", ex.GetType().Name + " " + ex.Message);
            return 3;
        }
    }

    private static int ParseOptional(string[] args, int index, int fallback) =>
        args.Length > index && int.TryParse(args[index], out var value) ? value : fallback;

    private static void Say(string key, string value) => Console.WriteLine(key + "=" + value);

    private static int Measure(
        int seconds,
        int fps,
        int adapterWanted,
        int outputWanted,
        int windowWidth,
        int windowHeight)
    {
        var iid = Dxgi.IIDIDXGIFactory1;
        Say("FACTORY_HR", Dxgi.Hex(Dxgi.CreateDXGIFactory1(ref iid, out var factory)));

        if (factory == IntPtr.Zero)
        {
            return 2;
        }

        var target = FindOutput(factory, adapterWanted, outputWanted, out var adapterIndex);

        if (target.Output == IntPtr.Zero)
        {
            Say("OUTCOME", "OUTPUT-NOT-FOUND");
            return 2;
        }

        Say("TARGET_ADAPTER", adapterIndex.ToString());
        Say("TARGET_OUTPUT", target.Description);
        Say("DESKTOP_RECT", target.Rect.Left + "," + target.Rect.Top + " " + target.Rect.Area);

        var deviceHr = Dxgi.D3D11CreateDevice(
            target.Adapter,
            target.Adapter == IntPtr.Zero ? 1u : 0u,
            IntPtr.Zero,
            0x20,
            IntPtr.Zero,
            0,
            7u,
            out var device,
            IntPtr.Zero,
            out var context);

        Say("D3D11CREATE_HR", Dxgi.Hex(deviceHr));

        // DuplicateOutput 要求设备与输出在同一块卡上，所以这里用的是输出所属适配器。
        var duplication = DuplicateOn(target.Output, device);

        if (duplication == IntPtr.Zero)
        {
            Say("OUTCOME", "DUPLICATE-FAILED");
            return 2;
        }

        var stats = new Stats();
        _stop = false;
        Interlocked.Exchange(ref _movesIssued, 0);

        var capture = new Thread(() => CaptureLoop(duplication, stats, target.Rect.Area))
        {
            IsBackground = true
        };

        var stopwatch = Stopwatch.StartNew();
        var cpuBefore = Process.GetCurrentProcess().TotalProcessorTime;
        capture.Start();

        Thread? mover = null;

        if (fps > 0)
        {
            mover = new Thread(() => MoveLoop(fps, windowWidth, windowHeight, target.Rect))
            {
                IsBackground = true
            };

            mover.Start();
        }

        Thread.Sleep(seconds * 1000);
        _stop = true;
        capture.Join(TimeSpan.FromSeconds(5));

        if (mover is not null)
        {
            mover.Join(TimeSpan.FromSeconds(5));
        }

        stopwatch.Stop();

        var cpuMs = (long)(Process.GetCurrentProcess().TotalProcessorTime - cpuBefore).TotalMilliseconds;
        var moves = Interlocked.Read(ref _movesIssued);

        stats.Report(seconds, stopwatch.Elapsed.TotalSeconds, cpuMs, moves, fps);
        Dxgi.Release(duplication);
        Dxgi.Release(device);
        Dxgi.Release(context);
        Dxgi.Release(target.Adapter);
        Dxgi.Release(target.Output);
        Dxgi.Release(factory);

        return 0;
    }

    private sealed class OutputTarget
    {
        public IntPtr Output;
        public IntPtr Adapter;
        public Dxgi.Rect Rect;
        public string Description = string.Empty;
    }

    /// <summary>adapterWanted = -1 时扫描全部适配器与输出，挑面积最大的那块（本机就是 4K 面板）。</summary>
    private static OutputTarget FindOutput(IntPtr factory, int adapterWanted, int outputWanted, out int adapterIndex)
    {
        adapterIndex = -1;
        var best = new OutputTarget { Output = IntPtr.Zero, Adapter = IntPtr.Zero };
        var bestArea = -1L;

        for (var index = 0u; index < 8; index++)
        {
            var adapterHr = Dxgi.Invoke<Dxgi.EnumAdapters1Delegate>(
                factory,
                Dxgi.SlotFactoryEnumAdapters1)(factory, index, out var adapter);

            if (adapterHr < 0)
            {
                break;
            }

            var wantedAdapter = adapterWanted < 0 || index == (uint)adapterWanted;

            if (!wantedAdapter)
            {
                Dxgi.Release(adapter);
                continue;
            }

            var found = PickFromAdapter(adapter, index, outputWanted, ref bestArea, adapterWanted >= 0);

            if (found is not null)
            {
                if (best.Output != IntPtr.Zero && found.Output != best.Output)
                {
                    // 换适配器了，上一轮的输出与适配器引用要放手，但不能放手当前这轮的。
                    if (found.Adapter != best.Adapter)
                    {
                        Dxgi.Release(best.Adapter);
                    }

                    Dxgi.Release(best.Output);
                }

                best = found;
                adapterIndex = (int)index;

                if (adapterWanted >= 0)
                {
                    return best;
                }
            }

            if (found is null)
            {
                Dxgi.Release(adapter);
            }
        }

        return best;
    }

    private static OutputTarget? PickFromAdapter(
        IntPtr adapter,
        uint adapterIndex,
        int outputWanted,
        ref long bestArea,
        bool stopAtRequestedOutput)
    {
        OutputTarget? picked = null;

        for (var outputIndex = 0u; outputIndex < 8; outputIndex++)
        {
            var outputHr = Dxgi.Invoke<Dxgi.EnumOutputsDelegate>(
                adapter,
                Dxgi.SlotAdapterEnumOutputs)(adapter, outputIndex, out var output);

            if (outputHr < 0)
            {
                break;
            }

            var buffer = Marshal.AllocHGlobal(360);

            try
            {
                var descHr = Dxgi.Invoke<Dxgi.GetOutputDescDelegate>(
                    output,
                    Dxgi.SlotOutputGetDesc)(output, buffer);

                if (descHr < 0)
                {
                    Dxgi.Release(output);
                    continue;
                }

                // DXGI_OUTPUT_DESC：WCHAR DeviceName[32]（64 字节）→ RECT（64）→ BOOL attached（80）。
                var rect = Marshal.PtrToStructure<Dxgi.Rect>(buffer + 64);
                var area = rect.Area;
                var name = Marshal.PtrToStringUni(buffer, 32)?.Split('\0')[0] ?? string.Empty;

                Say(
                    "OUT" + adapterIndex + "_" + outputIndex,
                    name + " " + rect.Width() + "x" + rect.Height() + " area " + area
                    + " attached " + Marshal.ReadInt32(buffer, 80));

                var wantsThis = stopAtRequestedOutput
                    ? outputIndex == (uint)outputWanted
                    : area > bestArea;

                if (wantsThis)
                {
                    if (!stopAtRequestedOutput)
                    {
                        bestArea = area;
                    }

                    picked = new OutputTarget
                    {
                        Output = output,
                        Adapter = adapter,
                        Rect = rect,
                        Description = name + " " + rect.Width() + "x" + rect.Height()
                    };

                    if (stopAtRequestedOutput)
                    {
                        return picked;
                    }
                }
                else
                {
                    Dxgi.Release(output);
                }
            }
            finally
            {
                Marshal.FreeHGlobal(buffer);
            }
        }

        return picked;
    }

    private static IntPtr DuplicateOn(IntPtr output, IntPtr device)
    {
        var iid = Dxgi.IIDIDXGIOutput1;

        if (Dxgi.QueryInterface(output, iid, out var output1) < 0)
        {
            Say("QI_OUTPUT1", "failed");
            return IntPtr.Zero;
        }

        // 桌面复制每个输出同时只允许一个持有者；被占用时是 E_ACCESSDENIED，重试几次再看。
        var hr = 0;

        for (var attempt = 0; attempt < 6; attempt++)
        {
            hr = Dxgi.Invoke<Dxgi.DuplicateOutputDelegate>(
                output1,
                Dxgi.SlotOutput1DuplicateOutput)(output1, device, out var duplication);

            if (hr >= 0)
            {
                Say("DUPLICATE_HR", Dxgi.Hex(hr) + " attempt " + (attempt + 1));
                Dxgi.Release(output1);

                return duplication;
            }

            Thread.Sleep(400);
        }

        Say("DUPLICATE_HR", Dxgi.Hex(hr) + " (复制权被其他程序占用)");
        Dxgi.Release(output1);

        return IntPtr.Zero;
    }

    private sealed class Stats
    {
        public long BogusRects;
        public long Frames;
        public long ContentFrames;
        public long Timeouts;
        public long AccessLost;
        public long MouseOnlyFrames;
        public long CoalescedFrames;
        public long DirtyRectCount;
        public long DirtyArea;
        public long MoveRectCount;
        public long MoveArea;
        public long MaxAccumulated;
        public long SumAccumulated;
        public long LargestDirty;
        public long FirstHr;

        public void Report(double requestedSeconds, double wallSeconds, long cpuMs, long moves, int fps)
        {
            var elapsed = wallSeconds;

            Say("SECONDS", requestedSeconds + " wall " + elapsed.ToString("F2"));
            Say("FRAMES_DELIVERED", Frames.ToString());
            Say("DELIVERED_FPS", (Frames / elapsed).ToString("F1"));
            Say("TIMEOUTS", Timeouts.ToString());
            Say("ACCESS_LOST", AccessLost.ToString());
            Say("MOUSE_ONLY_FRAMES", MouseOnlyFrames.ToString());
            Say("CONTENT_FRAMES", ContentFrames.ToString());
            Say("COALESCED_FRAMES", CoalescedFrames.ToString());
            Say("MOVES_ISSUED", moves + " target " + fps + " achieved " + (moves / elapsed).ToString("F1"));
            Say("DIRTY_RECTS", DirtyRectCount.ToString());
            Say("DIRTY_RECTS_PER_FRAME", ContentFrames > 0 ? (DirtyRectCount / (double)ContentFrames).ToString("F1") : "-");
            Say("DIRTY_PIXELS_TOTAL", DirtyArea.ToString("N0"));
            Say("DIRTY_PIXELS_PER_FRAME", ContentFrames > 0 ? (DirtyArea / ContentFrames).ToString("N0") : "-");
            Say("LARGEST_DIRTY_RECT", LargestDirty.ToString("N0"));
            Say("BOGUS_RECTS", BogusRects.ToString());
            Say("MOVE_RECTS", MoveRectCount.ToString());
            Say("MOVE_PIXELS_TOTAL", MoveArea.ToString("N0"));
            Say("ACCUMULATED_MAX", MaxAccumulated.ToString());
            Say("ACCUMULATED_SUM", SumAccumulated.ToString());
            Say("CAPTURE_CPU_MS", cpuMs.ToString());
            Say("WORKING_SET_MB", (Environment.WorkingSet / 1048576).ToString());
            Say("FIRST_ACQUIRE_HR", Dxgi.Hex((int)FirstHr));
        }
    }

    private static void CaptureLoop(IntPtr duplication, Stats stats, long desktopArea)
    {
        var dirtyBuffer = Marshal.AllocHGlobal(RectCapacity * 16);
        var moveBuffer = Marshal.AllocHGlobal(RectCapacity * 24);

        try
        {
            while (!_stop)
            {
                var hr = Dxgi.Invoke<Dxgi.AcquireNextFrameDelegate>(
                    duplication,
                    Dxgi.SlotDuplicationAcquireNextFrame)(duplication, 100, out var info, out var resource);

                if (hr == Dxgi.DxgiErrorWaitTimeout)
                {
                    Interlocked.Increment(ref stats.Timeouts);
                    continue;
                }

                if (hr == Dxgi.DxgiErrorAccessLost)
                {
                    Interlocked.Increment(ref stats.AccessLost);
                    Thread.Sleep(50);
                    continue;
                }

                if (hr < 0)
                {
                    Interlocked.CompareExchange(ref stats.FirstHr, hr, 0);
                    continue;
                }

                Interlocked.Increment(ref stats.Frames);
                Interlocked.Add(ref stats.SumAccumulated, info.AccumulatedFrames);
                InterlockedMax(ref stats.MaxAccumulated, info.AccumulatedFrames);

                if (info.AccumulatedFrames > 0)
                {
                    Interlocked.Increment(ref stats.ContentFrames);
                }

                if (info.RectsCoalesced != 0)
                {
                    Interlocked.Increment(ref stats.CoalescedFrames);
                }

                if (info.AccumulatedFrames == 0)
                {
                    Interlocked.Increment(ref stats.MouseOnlyFrames);
                }

                CollectRects(duplication, stats, dirtyBuffer, moveBuffer, desktopArea);

                Dxgi.Invoke<Dxgi.NoArgDelegate>(duplication, Dxgi.SlotDuplicationReleaseFrame)(duplication);
                Dxgi.Release(resource);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(dirtyBuffer);
            Marshal.FreeHGlobal(moveBuffer);
        }
    }

    private static void CollectRects(
        IntPtr duplication,
        Stats stats,
        IntPtr dirtyBuffer,
        IntPtr moveBuffer,
        long desktopArea)
    {
        // 注意：这两个 API 的容量与 required 都是**字节数**，不是矩形个数（踩过一次）。
        const int DirtyBytes = RectCapacity * 16;
        const int MoveBytes = RectCapacity * 24;

        var dirtyHr = Dxgi.Invoke<Dxgi.GetFrameRectsDelegate>(
            duplication,
            Dxgi.SlotDuplicationGetFrameDirtyRects)(duplication, DirtyBytes, dirtyBuffer, out var dirtyRequired);

        if (dirtyHr >= 0)
        {
            var count = (long)Math.Min(dirtyRequired / 16, RectCapacity);

            Interlocked.Add(ref stats.DirtyRectCount, count);

            for (var i = 0; i < count; i++)
            {
                var rect = Marshal.PtrToStructure<Dxgi.Rect>(dirtyBuffer + i * 16);
                var area = rect.Area;

                // 尺寸超出桌面说明读到的不是这一帧的矩形，单独计数，不混进总量。
                if (rect.Width() > 8192 || rect.Height() > 8192)
                {
                    Interlocked.Increment(ref stats.BogusRects);
                    continue;
                }

                Interlocked.Add(ref stats.DirtyArea, area);
                InterlockedMax(ref stats.LargestDirty, area);
            }
        }

        var moveHr = Dxgi.Invoke<Dxgi.GetFrameRectsDelegate>(
            duplication,
            Dxgi.SlotDuplicationGetFrameMoveRects)(duplication, MoveBytes, moveBuffer, out var moveRequired);

        if (moveHr >= 0)
        {
            var count = (long)Math.Min(moveRequired / 24, RectCapacity);
            Interlocked.Add(ref stats.MoveRectCount, count);

            for (var i = 0; i < count; i++)
            {
                var move = Marshal.PtrToStructure<Dxgi.MoveRect>(moveBuffer + i * 24);
                Interlocked.Add(ref stats.MoveArea, move.Destination.Area);
            }
        }

        if (desktopArea <= 0)
        {
            return;
        }
    }

    private static void MoveLoop(int fps, int width, int height, Dxgi.Rect desk)
    {
        // 直接用系统 "Static" 类画一块实心矩形：不需要注册自己的窗口过程，
        // 目的是制造可知的画面变化，不是测窗口本身。
        // 窗口必须落在"被捕获的那块屏"里，否则脏矩形落在别的输出上，测不到东西。
        var left = desk.Left;
        var top = desk.Top;
        var fullWidth = desk.Width();
        var stepPixels = Math.Max(8, fullWidth * 40 / (fps * 100));
        var x = left + fullWidth - width - 40;
        var direction = -1;
        var y = top + desk.Height() - height - 60;

        var hWnd = Win32.CreateWindowExW(
            Win32.WsExToolWindow | Win32.WsExNoActivate,
            "Static",
            "HaloCap spike mover",
            Win32.WsPopup | Win32.WsVisible | Win32.SsBlackRect,
            x,
            y,
            width,
            height,
            IntPtr.Zero,
            IntPtr.Zero,
            Win32.GetModuleHandle(null),
            IntPtr.Zero);

        if (hWnd == IntPtr.Zero)
        {
            Say("CREATE_WINDOW", "failed err " + Marshal.GetLastWin32Error());
            return;
        }

        var interval = Math.Max(1, 1000 / Math.Max(1, fps));

        try
        {
            while (!_stop)
            {
                x += direction * stepPixels;

                if (x <= left + 10 || x + width >= left + fullWidth - 10)
                {
                    direction = -direction;
                    x = Math.Clamp(x, left + 10, left + fullWidth - width - 10);
                }

                Win32.SetWindowPos(
                    hWnd,
                    IntPtr.Zero,
                    x,
                    y,
                    width,
                    height,
                    Win32.SwpNoZOrder | Win32.SwpNoActivate);

                Interlocked.Increment(ref _movesIssued);

                while (Win32.PeekMessageW(out var msg, IntPtr.Zero, 0, 0, Win32.PmRemove))
                {
                    Win32.TranslateMessage(ref msg);
                    Win32.DispatchMessageW(ref msg);
                }

                Thread.Sleep(interval);
            }
        }
        finally
        {
            Win32.DestroyWindow(hWnd);
        }
    }


    private static void InterlockedMax(ref long target, long value)
    {
        var current = Interlocked.Read(ref target);

        while (value > current)
        {
            var previous = Interlocked.CompareExchange(ref target, value, current);

            if (previous == current)
            {
                return;
            }

            current = previous;
        }
    }
}

internal static class Native
{
    public static string Hex(int hr) => "0x" + hr.ToString("X8");

    public static int Release(IntPtr pointer) => pointer == IntPtr.Zero ? 0 : Dxgi.Release(pointer);
}
