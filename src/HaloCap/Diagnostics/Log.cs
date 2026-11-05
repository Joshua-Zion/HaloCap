using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Threading;

namespace HaloCap.Diagnostics;

internal static class Log
{
    // 日志以前只会按天新建文件，从不清理也不限长；单次异常刷屏就能把磁盘写满。
    private const long MaxFileBytes = 8L * 1024 * 1024;
    private const int RetainDays = 14;
    private const int MaxPartsPerDay = 6;

    private static readonly object Gate = new();
    private static int pruned;
    private static int capacityWarned;

    public static string AppDataDirectory { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
        "HaloCap");

    public static string LogDirectory { get; } = Path.Combine(AppDataDirectory, "logs");

    public static void Info(string message) => Write("INFO", message, null);

    public static void Error(string message, Exception? exception = null) =>
        Write("ERROR", message, exception);

    private static void Write(string level, string message, Exception? exception)
    {
        try
        {
            Directory.CreateDirectory(LogDirectory);

            var builder = new StringBuilder()
                .Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff"))
                .Append(" [").Append(level).Append("] ")
                .Append(message);

            if (exception is not null)
            {
                builder.AppendLine().Append(exception);
            }

            lock (Gate)
            {
                var file = ResolveLogFile();

                if (file is not null)
                {
                    File.AppendAllText(file, builder.AppendLine().ToString(), Encoding.UTF8);
                }
            }

            if (Interlocked.Exchange(ref pruned, 1) == 0)
            {
                PruneOldLogs();
            }
        }
        catch
        {
            // 日志失败不能影响主流程。
        }
    }

    private static string? ResolveLogFile()
    {
        var stamp = DateTime.Now.ToString("yyyyMMdd");

        for (var part = 0; part < MaxPartsPerDay; part++)
        {
            var name = part == 0 ? $"app-{stamp}.log" : $"app-{stamp}-{part + 1}.log";
            var path = Path.Combine(LogDirectory, name);

            if (!File.Exists(path) || new FileInfo(path).Length < MaxFileBytes)
            {
                return path;
            }
        }

        if (Interlocked.Exchange(ref capacityWarned, 1) == 0)
        {
            TryAppend(
                Path.Combine(LogDirectory, $"app-{stamp}-last.log"),
                "本日日志已达上限，后续日志不再写入；清理 " + LogDirectory + " 后恢复。");
        }

        return null;
    }

    private static void PruneOldLogs()
    {
        try
        {
            var entries = new List<(string Path, DateTime Written)>();

            foreach (var path in Directory.GetFiles(LogDirectory, "app-*.log"))
            {
                try
                {
                    entries.Add((path, File.GetLastWriteTime(path)));
                }
                catch
                {
                    // 读不到时间戳就跳过这个文件。
                }
            }

            foreach (var path in SelectExpired(entries, DateTime.Now, RetainDays))
            {
                try
                {
                    File.Delete(path);
                }
                catch
                {
                    // 单个文件删不掉不影响其余清理。
                }
            }
        }
        catch
        {
            // 清理失败可以忽略。
        }
    }

    /// <summary>挑出超过保留天数的日志；单独抽出来是为了能离线验证这条清理规则。</summary>
    internal static List<string> SelectExpired(
        IEnumerable<(string Path, DateTime Written)> entries,
        DateTime now,
        int retainDays)
    {
        var cutoff = now.AddDays(-Math.Max(1, retainDays));
        var expired = new List<string>();

        foreach (var entry in entries)
        {
            if (entry.Written < cutoff)
            {
                expired.Add(entry.Path);
            }
        }

        return expired;
    }

    private static void TryAppend(string file, string message)
    {
        try
        {
            var line = DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + " [INFO] " + message + Environment.NewLine;
            File.AppendAllText(file, line, Encoding.UTF8);
        }
        catch
        {
            // 忽略。
        }
    }
}
