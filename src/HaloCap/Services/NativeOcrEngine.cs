using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Media.Imaging;
using HaloCap.Models;
using Windows.Graphics.Imaging;
using Windows.Media.Ocr;
using Windows.Storage.Streams;

namespace HaloCap.Services;

internal sealed class NativeOcrEngine
{
    private readonly SemaphoreSlim _gate = new(1, 1);
    private OcrEngine? _engine;
    private bool _engineChecked;
    private bool _chineseAvailable;

    public async Task<OcrEngineResult> RecognizeAsync(BitmapSource image)
    {
        await _gate.WaitAsync().ConfigureAwait(false);

        try
        {
            var engine = EnsureEngine();

            if (engine is null)
            {
                return new OcrEngineResult(
                    OcrStatus.NoLanguage,
                    Array.Empty<string>(),
                    OcrService.ChineseLanguageGuidance,
                    "Windows 原生");
            }

            var maxDimension = (int)Math.Min(
                OcrEngine.MaxImageDimension,
                (uint)OcrImageProcessor.MaxWorkingDimension);
            var prepared = OcrImageProcessor.Prepare(image, maxDimension);

            using var softwareBitmap = await CreateSoftwareBitmapAsync(
                    prepared.Pixels,
                    prepared.Width,
                    prepared.Height)
                .ConfigureAwait(false);
            var result = await engine.RecognizeAsync(softwareBitmap);

            var lines = result.Lines
                .Select(line => line.Text?.Trim())
                .Where(text => !string.IsNullOrWhiteSpace(text))
                .Select(text => text!)
                .ToArray();

            var warning = _chineseAvailable
                ? string.Empty
                : OcrService.ChineseLanguageGuidance +
                  Environment.NewLine + Environment.NewLine +
                  "也可以切换到「模型」识别，不依赖系统语言组件。";

            return new OcrEngineResult(
                lines.Length > 0 ? OcrStatus.Success : OcrStatus.Empty,
                lines,
                warning,
                "Windows 原生");
        }
        finally
        {
            _gate.Release();
        }
    }

    public void Clear()
    {
        _engine = null;
        _engineChecked = false;
        _chineseAvailable = false;
    }

    private OcrEngine? EnsureEngine()
    {
        if (_engineChecked)
        {
            return _engine;
        }

        _engineChecked = true;

        try
        {
            var languages = OcrEngine.AvailableRecognizerLanguages.ToList();
            _chineseAvailable = languages.Any(language =>
                language.LanguageTag.StartsWith("zh", StringComparison.OrdinalIgnoreCase));

            var chinese = languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("zh-Hans", StringComparison.OrdinalIgnoreCase))
                          ?? languages.FirstOrDefault(language =>
                              string.Equals(language.LanguageTag, "zh-CN", StringComparison.OrdinalIgnoreCase))
                          ?? languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("zh", StringComparison.OrdinalIgnoreCase));
            var english = languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("en", StringComparison.OrdinalIgnoreCase));

            foreach (var language in new[] { chinese, english }
                         .Where(language => language is not null)
                         .Concat(languages)
                         .Distinct())
            {
                try
                {
                    _engine = OcrEngine.TryCreateFromLanguage(language);

                    if (_engine is not null)
                    {
                        return _engine;
                    }
                }
                catch
                {
                    // 尝试下一个已安装语言。
                }
            }

            _engine = OcrEngine.TryCreateFromUserProfileLanguages();
        }
        catch
        {
            _engine = null;
        }

        return _engine;
    }

    private static async Task<SoftwareBitmap> CreateSoftwareBitmapAsync(
        byte[] pixels,
        int width,
        int height)
    {
        using var stream = new InMemoryRandomAccessStream();

        using (var writer = new DataWriter(stream.GetOutputStreamAt(0)))
        {
            writer.WriteBytes(pixels);
            await writer.StoreAsync();
            writer.DetachStream();
        }

        stream.Seek(0);
        var buffer = new Windows.Storage.Streams.Buffer((uint)pixels.Length);
        await stream.ReadAsync(buffer, (uint)pixels.Length, InputStreamOptions.None);

        return SoftwareBitmap.CreateCopyFromBuffer(
            buffer,
            BitmapPixelFormat.Bgra8,
            width,
            height,
            BitmapAlphaMode.Premultiplied);
    }
}
