using System.Reflection;

namespace HaloCap.Models;

internal static class AppInfo
{
    public const string DisplayName = "HaloCap 光圈";

    public static string Version { get; } = ResolveVersion();

    public static string VersionLabel => "v" + Version;

    private static string ResolveVersion()
    {
        var assembly = typeof(AppInfo).Assembly;
        var informational = assembly
            .GetCustomAttribute<AssemblyInformationalVersionAttribute>()?
            .InformationalVersion;

        if (!string.IsNullOrWhiteSpace(informational))
        {
            // SDK 会在信息版本后追加 +<commit>，界面上只显示版本号本身。
            var plus = informational.IndexOf('+');
            return plus > 0 ? informational[..plus] : informational;
        }

        var version = assembly.GetName().Version;
        return version is null
            ? "未知"
            : $"{version.Major}.{version.Minor}.{version.Build}";
    }
}
