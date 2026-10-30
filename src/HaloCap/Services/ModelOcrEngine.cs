using System;
using System.Collections.Generic;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Media.Imaging;
using HaloCap.Models;
using Microsoft.ML.OnnxRuntime;
using RapidOcrNet;
using SkiaSharp;

namespace HaloCap.Services;

internal sealed class ModelOcrEngine : IDisposable
{
    private static readonly RapidOcrOptions OcrOptions = RapidOcrOptions.PPOCRv6 with
    {
        DoAngle = false,
        Padding = 6,
        TextScore = 0.4f,
        ReturnWordBox = false
    };

    private readonly SemaphoreSlim _gate = new(1, 1);
    private RapidOcr? _ocr;
    private bool _disposed;

    public OcrEngineResult Recognize(BitmapSource image)
    {
        _gate.Wait();

        try
        {
            var ocr = EnsureInitialized();
            var prepared = OcrImageProcessor.Prepare(image, OcrImageProcessor.MaxWorkingDimension);
            OcrImageProcessor.InvertIfDark(prepared.Pixels);
            using var bitmap = ToSkBitmap(prepared);
            var result = ocr.Detect(bitmap, OcrOptions);
            var lines = result.TextBlocks
                .OrderBy(block => block.BoxPoints.Length == 0 ? 0 : block.BoxPoints.Min(point => point.Y))
                .ThenBy(block => block.BoxPoints.Length == 0 ? 0 : block.BoxPoints.Min(point => point.X))
                .SelectMany(block => block.Text.Split('\n'))
                .Select(line => line.Trim())
                .Where(line => line.Length > 0)
                .ToArray();

            return new OcrEngineResult(
                lines.Length > 0 ? OcrStatus.Success : OcrStatus.Empty,
                lines,
                string.Empty,
                "PP-OCRv6 tiny");
        }
        finally
        {
            _gate.Release();
        }
    }

    public void Dispose()
    {
        _gate.Wait();

        try
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _ocr?.Dispose();
            _ocr = null;
        }
        finally
        {
            _gate.Release();
        }
    }

    private RapidOcr EnsureInitialized()
    {
        if (_disposed)
        {
            throw new ObjectDisposedException(nameof(ModelOcrEngine));
        }

        if (_ocr is not null)
        {
            return _ocr;
        }

        var paths = OcrModelAssets.EnsureExtracted();
        var ocr = new RapidOcr();

        try
        {
            using var sessionOptions = RapidOcr.GetDefaultSessionOptions(
                Math.Clamp(Environment.ProcessorCount / 2, 2, 4));
            ocr.InitModels(
                paths.Detector,
                paths.Classifier,
                paths.Recognizer,
                paths.Dictionary,
                sessionOptions);
            _ocr = ocr;
            return ocr;
        }
        catch
        {
            ocr.Dispose();
            throw;
        }
    }

    private static SKBitmap ToSkBitmap(PreparedOcrImage image)
    {
        var info = new SKImageInfo(
            image.Width,
            image.Height,
            SKColorType.Bgra8888,
            SKAlphaType.Premul);
        var bitmap = new SKBitmap(info);
        var rowBytes = bitmap.RowBytes;
        var sourceStride = image.Width * 4;
        var target = bitmap.GetPixels();

        if (rowBytes == sourceStride)
        {
            Marshal.Copy(image.Pixels, 0, target, image.Pixels.Length);
            return bitmap;
        }

        for (var y = 0; y < image.Height; y++)
        {
            Marshal.Copy(
                image.Pixels,
                y * sourceStride,
                IntPtr.Add(target, y * rowBytes),
                sourceStride);
        }

        return bitmap;
    }
}
