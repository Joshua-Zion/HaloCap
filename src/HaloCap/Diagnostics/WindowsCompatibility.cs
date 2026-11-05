using System;

namespace HaloCap.Diagnostics;

/// <summary>
/// 系统版本下限：本项目声明支持 Windows 10 1903（build 18362）起。
/// 更早的系统不在支持范围内，但也不该静默崩掉——先把话说清楚，再让能用的功能继续用。
/// </summary>
internal static class WindowsCompatibility
{
    internal const int MinimumBuild = 18362;

    /// <summary>GraphicsCaptureSession.IsCursorCaptureEnabled 与无边框开关随 2004 加入。</summary>
    internal const int CaptureToggleBuild = 19041;

    internal static int CurrentBuild
    {
        get
        {
            try
            {
                return Environment.OSVersion.Version.Build;
            }
            catch
            {
                return 0;
            }
        }
    }

    internal static bool MeetsFloor(int build) => build >= MinimumBuild;

    internal static bool SupportsCaptureToggle(int build) => build >= CaptureToggleBuild;

    /// <summary>低于下限时给用户的说明；满足要求时返回 null。</summary>
    internal static string? DescribeFloorProblem(int build)
    {
        if (MeetsFloor(build))
        {
            return null;
        }

        return build == 0
            ? "无法确认 Windows 版本，HaloCap 需要 Windows 10 1903 或更高版本；录屏可能无法工作。"
            : $"HaloCap 需要 Windows 10 1903 或更高版本（当前 build {build}）。截图、文字识别和贴图仍可使用，录屏可能无法工作。";
    }

    /// <summary>用户要求隐藏光标但系统不支持时，把「哪个设置被忽略」说清楚，别让它静默失效。</summary>
    internal static string DescribeCursorFallback(int build) =>
        $"当前 Windows 版本（build {build}）不支持关闭录制光标，画面里会始终带上光标；" +
        "升级到 Windows 10 2004 以上后此设置才会生效。";
}
