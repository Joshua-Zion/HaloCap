using System;
using System.IO;
using System.Reflection;

namespace HaloCap.Services;

internal sealed record OcrModelPaths(string Detector, string Classifier, string Recognizer, string Dictionary);

internal static class OcrModelAssets
{
    private const string Version = "ppocrv6-tiny-1";
    private const string DetectorResource = "HaloCap.OcrModel.det.onnx";
    private const string ClassifierResource = "HaloCap.OcrModel.cls.onnx";
    private const string RecognizerResource = "HaloCap.OcrModel.rec.onnx";
    private const string DictionaryResource = "HaloCap.OcrModel.dict.txt";

    public static OcrModelPaths EnsureExtracted()
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

        if (string.IsNullOrWhiteSpace(root))
        {
            root = Path.GetTempPath();
        }

        var directory = Path.Combine(root, "HaloCap", "models", Version);
        Directory.CreateDirectory(directory);

        return new OcrModelPaths(
            Extract(DetectorResource, Path.Combine(directory, "det.onnx")),
            Extract(ClassifierResource, Path.Combine(directory, "cls.onnx")),
            Extract(RecognizerResource, Path.Combine(directory, "rec.onnx")),
            Extract(DictionaryResource, Path.Combine(directory, "dict.txt")));
    }

    private static string Extract(string resourceName, string destination)
    {
        var assembly = typeof(OcrModelAssets).Assembly;
        using var stream = assembly.GetManifestResourceStream(resourceName)
                           ?? throw new InvalidOperationException("缺少内置 OCR 模型资源：" + resourceName);

        if (File.Exists(destination) && TryGetLength(stream) is { } expected &&
            new FileInfo(destination).Length == expected)
        {
            return destination;
        }

        var temporary = destination + ".tmp";

        try
        {
            using (var file = new FileStream(
                       temporary,
                       FileMode.Create,
                       FileAccess.Write,
                       FileShare.None))
            {
                stream.CopyTo(file);
            }

            File.Move(temporary, destination, true);
            return destination;
        }
        catch
        {
            TryDelete(temporary);
            throw new IOException("无法准备内置 OCR 模型：" + destination);
        }
    }

    private static long? TryGetLength(Stream stream)
    {
        try
        {
            return stream.Length;
        }
        catch (NotSupportedException)
        {
            return null;
        }
    }

    private static void TryDelete(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                File.Delete(path);
            }
        }
        catch
        {
            // 临时文件清理失败不影响主流程。
        }
    }
}
