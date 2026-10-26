using System;
using System.IO;

namespace HaloCap.Services;

internal static class AppPaths
{
    public static string DefaultRecordingDirectory
    {
        get
        {
            var videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
            if (string.IsNullOrWhiteSpace(videos))
            {
                videos = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            }

            if (string.IsNullOrWhiteSpace(videos))
            {
                videos = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Videos");
            }

            return Path.Combine(videos, "Captures");
        }
    }

    public static string DefaultScreenshotDirectory
    {
        get
        {
            var pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyPictures);
            if (string.IsNullOrWhiteSpace(pictures))
            {
                pictures = Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
            }

            return Path.Combine(pictures, "HaloCap");
        }
    }

    public static string GetRecordingDirectory(string configuredDirectory)
    {
        var directory = string.IsNullOrWhiteSpace(configuredDirectory)
            ? DefaultRecordingDirectory
            : NormalizeDirectory(configuredDirectory);

        Directory.CreateDirectory(directory);
        return directory;
    }

    public static string NormalizeDirectory(string rawPath)
    {
        var value = rawPath.Trim().Trim('"');
        if (value.Length == 0)
        {
            return DefaultRecordingDirectory;
        }

        value = Environment.ExpandEnvironmentVariables(value);
        var fullPath = Path.GetFullPath(value);

        if (File.Exists(fullPath))
        {
            throw new IOException("该路径指向一个文件，请选择文件夹。");
        }

        var root = Path.GetPathRoot(fullPath) ?? string.Empty;
        if (fullPath.Length <= root.Length)
        {
            return fullPath;
        }

        var trimmed = fullPath.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        return trimmed.Length == 0 ? fullPath : trimmed;
    }

    public static string CreateRecordingPath(string configuredDirectory, DateTime timestamp)
    {
        var directory = GetRecordingDirectory(configuredDirectory);
        var baseName = "HaloCap_" + timestamp.ToString("yyyy-MM-dd_HH-mm-ss");
        var path = Path.Combine(directory, baseName + ".mp4");
        var index = 2;

        while (File.Exists(path))
        {
            path = Path.Combine(directory, baseName + "_" + index + ".mp4");
            index++;
        }

        return path;
    }
}
