using System;
using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;
using HaloCap.Models;

namespace HaloCap.Services;

internal static class ImageFileService
{
    public static string GetDefaultExtension(ScreenshotImageFormat format) =>
        format == ScreenshotImageFormat.Jpeg ? ".jpg" : ".png";

    public static string? SaveWithDialog(
        BitmapSource source,
        ScreenshotImageFormat format,
        string? suggestedDirectory = null,
        Window? owner = null)
    {
        var directory = suggestedDirectory;
        if (string.IsNullOrWhiteSpace(directory))
        {
            directory = AppPaths.DefaultScreenshotDirectory;
        }

        try
        {
            Directory.CreateDirectory(directory);
        }
        catch
        {
            directory = Environment.GetFolderPath(Environment.SpecialFolder.Desktop);
        }

        var isJpeg = format == ScreenshotImageFormat.Jpeg;
        var extension = GetDefaultExtension(format);
        var dialog = new Microsoft.Win32.SaveFileDialog
        {
            Title = "保存截图",
            InitialDirectory = directory,
            FileName = "HaloCap_" + DateTime.Now.ToString("yyyyMMdd_HHmmss") + extension,
            DefaultExt = extension,
            AddExtension = true,
            OverwritePrompt = true,
            Filter = isJpeg ? "JPEG 图片 (*.jpg)|*.jpg|所有文件 (*.*)|*.*" : "PNG 图片 (*.png)|*.png|所有文件 (*.*)|*.*"
        };

        var accepted = owner is null ? dialog.ShowDialog() : dialog.ShowDialog(owner);
        if (accepted != true)
        {
            return null;
        }

        Save(source, dialog.FileName, format);
        return dialog.FileName;
    }

    public static void Save(BitmapSource source, string path, ScreenshotImageFormat format)
    {
        var directory = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(directory))
        {
            Directory.CreateDirectory(directory);
        }

        BitmapEncoder encoder = format == ScreenshotImageFormat.Jpeg
            ? new JpegBitmapEncoder { QualityLevel = 92 }
            : new PngBitmapEncoder();

        encoder.Frames.Add(BitmapFrame.Create(source));

        using var stream = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None);
        encoder.Save(stream);
    }
}
