using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32;
using Windows.Graphics.Capture;
using Windows.Media.Ocr;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.Diagnostics;

/// <summary>
/// 环境摘要：启动时写进日志，设置页也能一键复制。
/// 「录屏没声音」「为什么是软件编码」「识别不出中文」这类问题，先看这份而不是猜。
/// </summary>
internal static class CapabilityReport
{
    private static readonly Guid MftCategoryVideoEncoder =
        new("f79eac7d-e545-4387-bdee-d647d7bde42a");

    private static readonly Guid MftCategoryAudioEncoder =
        new("91c64bd0-f91e-4d8c-9276-db248279d975");

    // MFAudioFormat_AAC 的 Data1 是 WAVE_FORMAT_MPEG_HEAAC。
    private static readonly Guid AacSubtype =
        new("00001610-0000-0010-8000-00AA00389B71");

    private const int MfVersion = 0x00020070;
    private const uint EnumFlags = 0x00000001 | 0x00000002 | 0x00000004 | 0x00000010 | 0x00000020;

    private static string? cached;

    internal static string Text => cached ??= Build();

    internal static void LogStartupBanner()
    {
        foreach (var raw in Text.Split('\n'))
        {
            var line = raw.TrimEnd();

            if (line.Length > 0)
            {
                Log.Info("[诊断] " + line);
            }
        }
    }

    private static string Build()
    {
        var builder = new StringBuilder();
        var build = WindowsCompatibility.CurrentBuild;

        builder.AppendLine($"{AppInfo.DisplayName} {AppInfo.VersionLabel} · {(Environment.Is64BitProcess ? "x64" : "x86")}");
        builder.AppendLine("系统：" + DescribeOs(build));
        builder.AppendLine("运行时：" + RuntimeInformation.FrameworkDescription);
        builder.AppendLine("显示器：" + DescribeMonitors());
        builder.AppendLine("屏幕捕获：" + DescribeCapture(build));
        builder.AppendLine("视频编码器：" + DescribeEncoders(MftCategoryVideoEncoder, null));
        builder.AppendLine("音频编码器：" + DescribeEncoders(MftCategoryAudioEncoder, AacSubtype));
        builder.AppendLine("文字识别：" + DescribeOcr());
        builder.AppendLine("内存：" + DescribeMemory());
        builder.AppendLine("设置：" + SettingsStore.FilePath);
        builder.AppendLine("日志：" + Log.LogDirectory);

        return builder.ToString().TrimEnd();
    }

    private static string DescribeOs(int build)
    {
        var caption = $"{Environment.OSVersion.Version.Major}.{Environment.OSVersion.Version.Minor}  build {build}";

        try
        {
            using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");

            if (key is not null)
            {
                var name = key.GetValue("ProductName") as string;
                var display = key.GetValue("DisplayVersion") as string;
                var ubr = key.GetValue("UBR") as int?;

                caption = (string.IsNullOrWhiteSpace(name) ? "Windows" : name) +
                    " " + Environment.OSVersion.Version.Major + "." + Environment.OSVersion.Version.Minor +
                    (string.IsNullOrWhiteSpace(display) ? string.Empty : " " + display) +
                    " build " + build +
                    (ubr is null ? string.Empty : "." + ubr);
            }
        }
        catch
        {
            // 读不到注册表就用版本号文件，摘要本身不该失败。
        }

        return caption + (WindowsCompatibility.MeetsFloor(build)
            ? "（支持下限 1903 / build " + WindowsCompatibility.MinimumBuild + "）"
            : "（低于支持下限 1903 / build " + WindowsCompatibility.MinimumBuild + "）");
    }

    private static string DescribeMonitors()
    {
        try
        {
            var monitors = MonitorService.GetMonitors();

            if (monitors.Count == 0)
            {
                return "未枚举到显示器";
            }

            var parts = new List<string>(monitors.Count);

            foreach (var monitor in monitors)
            {
                parts.Add((monitor.IsPrimary ? "主 " : string.Empty) +
                    monitor.Bounds +
                    " @" + monitor.Scale.ToString("0.##") + "x");
            }

            return monitors.Count + " 块 · " + string.Join(" · ", parts);
        }
        catch (Exception ex)
        {
            return "枚举失败：" + ex.Message;
        }
    }

    private static string DescribeCapture(int build)
    {
        var supported = false;

        try
        {
            supported = GraphicsCaptureSession.IsSupported();
        }
        catch (Exception ex)
        {
            return "探测异常：" + ex.Message;
        }

        var optional = WindowsCompatibility.SupportsCaptureToggle(build)
            ? "光标采集与无边框采集可用"
            : "光标/无边框采集需 build " + WindowsCompatibility.CaptureToggleBuild +
              "+（当前 " + build + "，会始终带上光标并保留彩色边框）";

        return (supported ? "WGC 可用" : "WGC 不可用（无法录屏或全屏截图）") + " · " + optional;
    }

    private static string DescribeEncoders(Guid category, Guid? wantedSubtype)
    {
        var names = new List<string>();
        var hasWanted = false;
        var buffer = IntPtr.Zero;
        var started = false;

        try
        {
            started = Native.MFStartup(MfVersion, 0) >= 0;
            var hr = Native.MFTEnum(category, EnumFlags, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero, out buffer, out var count);

            if (hr < 0 || buffer == IntPtr.Zero)
            {
                return "枚举失败 0x" + hr.ToString("X8");
            }

            var stride = Marshal.SizeOf<Guid>();

            for (var i = 0; i < (int)count; i++)
            {
                var clsid = Marshal.PtrToStructure<Guid>(buffer + (i * stride));
                var name = ReadEncoder(clsid, wantedSubtype, out var matches);

                if (!string.IsNullOrWhiteSpace(name))
                {
                    names.Add(name);
                }

                hasWanted |= matches;
            }
        }
        catch (Exception ex)
        {
            return "探测异常：" + ex.GetType().Name;
        }
        finally
        {
            Native.CoTaskMemFree(buffer);

            if (started)
            {
                Native.MFShutdown();
            }
        }

        if (names.Count == 0)
        {
            return "无";
        }

        var summary = names.Count + " 个：" + string.Join("、", names);

        if (wantedSubtype.HasValue)
        {
            summary += hasWanted ? " · 含 AAC" : " · 无 AAC 子类型（录屏会没有声音）";
        }

        return summary;
    }

    private static string ReadEncoder(Guid clsid, Guid? wantedSubtype, out bool matchesWanted)
    {
        matchesWanted = false;
        var name = string.Empty;
        IntPtr namePointer = IntPtr.Zero;
        IntPtr inputs = IntPtr.Zero;
        IntPtr outputs = IntPtr.Zero;

        try
        {
            if (Native.MFTGetInfo(clsid, out namePointer, out inputs, out var inputCount, out outputs, out var outputCount, IntPtr.Zero) < 0)
            {
                return namePointer == IntPtr.Zero ? clsid.ToString("N") : Marshal.PtrToStringUni(namePointer) ?? clsid.ToString("N");
            }

            if (namePointer != IntPtr.Zero)
            {
                name = Marshal.PtrToStringUni(namePointer) ?? clsid.ToString("N");
            }

            matchesWanted = ContainsSubtype(outputs, outputCount, wantedSubtype);
        }
        catch
        {
            // 单个编码器读不出来不影响整体摘要。
        }
        finally
        {
            Native.CoTaskMemFree(namePointer);
            Native.CoTaskMemFree(inputs);
            Native.CoTaskMemFree(outputs);
        }

        return string.IsNullOrWhiteSpace(name) ? clsid.ToString("N") : name;
    }

    private static bool ContainsSubtype(IntPtr pointer, uint count, Guid? wantedSubtype)
    {
        if (pointer == IntPtr.Zero || !wantedSubtype.HasValue)
        {
            return false;
        }

        var stride = Marshal.SizeOf<MftTypeInfo>();

        for (var i = 0; i < (int)count; i++)
        {
            var info = Marshal.PtrToStructure<MftTypeInfo>(pointer + (i * stride));

            if (info.Subtype == wantedSubtype.Value)
            {
                return true;
            }
        }

        return false;
    }

    private static string DescribeOcr()
    {
        var languages = new List<string>();

        try
        {
            foreach (var language in OcrEngine.AvailableRecognizerLanguages)
            {
                languages.Add(language.LanguageTag);
            }
        }
        catch (Exception ex)
        {
            return "原生引擎枚举异常：" + ex.GetType().Name;
        }

        var native = languages.Count == 0
            ? "原生引擎无可用语言（需在「可选功能」里安装光学字符识别）"
            : "原生引擎 " + languages.Count + " 种：" + string.Join(",", languages);

        return native + " · 模型引擎随程序内置";
    }

    private static string DescribeMemory()
    {
        try
        {
            using var process = System.Diagnostics.Process.GetCurrentProcess();
            return "私有 " + (process.PrivateMemorySize64 / 1024 / 1024) + " MB · 工作集 " +
                (process.WorkingSet64 / 1024 / 1024) + " MB";
        }
        catch
        {
            return "未知";
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct MftTypeInfo
    {
        public Guid MajorType;
        public Guid Subtype;
    }

    private static class Native
    {
        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFStartup(int version, int flags);

        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFShutdown();

        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFTEnum(
            Guid category,
            uint flags,
            IntPtr inputType,
            IntPtr outputType,
            IntPtr attributes,
            out IntPtr clsidArray,
            out uint count);

        [DllImport("mfplat.dll", ExactSpelling = true)]
        public static extern int MFTGetInfo(
            Guid clsid,
            out IntPtr name,
            out IntPtr inputTypes,
            out uint inputCount,
            out IntPtr outputTypes,
            out uint outputCount,
            IntPtr attributes);

        [DllImport("ole32.dll", ExactSpelling = true)]
        public static extern void CoTaskMemFree(IntPtr pointer);
    }
}
