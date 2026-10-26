using System;
using System.IO;
using System.Text;

namespace HaloCap.Diagnostics;

internal static class Log
{
    private static readonly object Gate = new();

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

            var file = Path.Combine(LogDirectory, $"app-{DateTime.Now:yyyyMMdd}.log");
            lock (Gate)
            {
                File.AppendAllText(file, builder.AppendLine().ToString(), Encoding.UTF8);
            }
        }
        catch
        {
            // 日志失败不能影响主流程。
        }
    }
}
