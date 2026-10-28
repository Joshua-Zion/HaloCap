using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Diagnostics;
using Windows.Globalization;
using Windows.Media.Ocr;
using Windows.Storage.Streams;
using WindowsBitmapAlphaMode = Windows.Graphics.Imaging.BitmapAlphaMode;
using WindowsBitmapDecoder = Windows.Graphics.Imaging.BitmapDecoder;
using WindowsBitmapEncoder = Windows.Graphics.Imaging.BitmapEncoder;
using WindowsBitmapPixelFormat = Windows.Graphics.Imaging.BitmapPixelFormat;

namespace HaloCap.Services;

internal enum OcrStatus
{
    Success,
    Empty,
    NoLanguage,
    Failed
}

internal sealed class OcrOutcome
{
    public OcrStatus Status { get; init; }

    public IReadOnlyList<string> Lines { get; init; } = Array.Empty<string>();

    public string? Message { get; init; }

    public string LanguageTag { get; init; } = string.Empty;

    public bool ChineseAvailable { get; init; }
}

internal static class OcrService
{
    public const string ChineseLanguageGuidance =
        "未找到中文文字识别语言包。请打开「设置 › 时间和语言 › 语言和区域」，添加「中文(简体，中国)」，" +
        "在语言选项的「可选语言功能」中安装「光学字符识别」，安装后重新启动 HaloCap。";

    private const string GeneralGuidance =
        "当前系统没有可用的文字识别语言组件。请打开「设置 › 时间和语言 › 语言和区域」，" +
        "为已安装的语言添加「光学字符识别」功能，安装后重新启动 HaloCap。";

    public static async Task<OcrOutcome> RecognizeAsync(BitmapSource source)
    {
        try
        {
            var selection = SelectEngine(out var chineseAvailable);

            if (selection.Engine is null)
            {
                return new OcrOutcome
                {
                    Status = OcrStatus.NoLanguage,
                    Message = GeneralGuidance,
                    ChineseAvailable = false
                };
            }

            using var softwareBitmap = await ToSoftwareBitmapAsync(source);
            var result = await selection.Engine.RecognizeAsync(softwareBitmap);

            var lines = result.Lines
                .Select(line => line.Text?.Trim())
                .Where(text => !string.IsNullOrWhiteSpace(text))
                .Select(text => text!)
                .ToArray();

            var warning = !chineseAvailable
                ? ChineseLanguageGuidance
                : null;

            return new OcrOutcome
            {
                Status = lines.Length > 0 ? OcrStatus.Success : OcrStatus.Empty,
                Lines = lines,
                Message = warning,
                LanguageTag = selection.LanguageTag,
                ChineseAvailable = chineseAvailable
            };
        }
        catch (Exception ex)
        {
            Log.Error("OCR 识别失败", ex);
            return new OcrOutcome
            {
                Status = OcrStatus.Failed,
                Message = "文字识别失败：" + ex.Message + "\n\n" + GeneralGuidance
            };
        }
    }

    private static EngineSelection SelectEngine(out bool chineseAvailable)
    {
        try
        {
            var languages = OcrEngine.AvailableRecognizerLanguages.ToList();
            var chinese = languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("zh-Hans", StringComparison.OrdinalIgnoreCase))
                          ?? languages.FirstOrDefault(language =>
                              string.Equals(language.LanguageTag, "zh-CN", StringComparison.OrdinalIgnoreCase))
                          ?? languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("zh", StringComparison.OrdinalIgnoreCase));

            chineseAvailable = chinese is not null;

            var english = languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("en-US", StringComparison.OrdinalIgnoreCase))
                          ?? languages.FirstOrDefault(language =>
                              language.LanguageTag.StartsWith("en", StringComparison.OrdinalIgnoreCase));

            var chosen = chinese ?? english ?? languages.FirstOrDefault();
            var engine = chosen is null ? null : OcrEngine.TryCreateFromLanguage(chosen);

            if (engine is null)
            {
                engine = OcrEngine.TryCreateFromUserProfileLanguages();
            }

            var finalTag = engine?.RecognizerLanguage?.LanguageTag ?? string.Empty;
            var finalIsChinese = finalTag.StartsWith("zh", StringComparison.OrdinalIgnoreCase);
            chineseAvailable = finalIsChinese;

            return new EngineSelection(engine, finalTag);
        }
        catch (Exception ex)
        {
            Log.Error("枚举 OCR 语言失败", ex);
            chineseAvailable = false;
            return new EngineSelection(null, string.Empty);
        }
    }

    private static async Task<Windows.Graphics.Imaging.SoftwareBitmap> ToSoftwareBitmapAsync(
        BitmapSource source)
    {
        var maxDimension = OcrEngine.MaxImageDimension;

        if (maxDimension > 0 &&
            ((uint)source.PixelWidth > maxDimension || (uint)source.PixelHeight > maxDimension))
        {
            var scale = Math.Min(
                maxDimension / (double)source.PixelWidth,
                maxDimension / (double)source.PixelHeight);

            var transformed = new TransformedBitmap(source, new ScaleTransform(scale, scale));
            transformed.Freeze();
            source = transformed;
        }

        BitmapSource converted = source.Format == PixelFormats.Bgra32
            ? source
            : new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0);

        var width = converted.PixelWidth;
        var height = converted.PixelHeight;
        var stride = width * 4;
        var pixels = new byte[stride * height];
        converted.CopyPixels(pixels, stride, 0);

        using var stream = new InMemoryRandomAccessStream();
        var encoder = await WindowsBitmapEncoder.CreateAsync(
            WindowsBitmapEncoder.PngEncoderId,
            stream);

        encoder.SetPixelData(
            WindowsBitmapPixelFormat.Bgra8,
            WindowsBitmapAlphaMode.Premultiplied,
            (uint)width,
            (uint)height,
            96,
            96,
            pixels);
        await encoder.FlushAsync();

        stream.Seek(0);
        var decoder = await WindowsBitmapDecoder.CreateAsync(stream);
        return await decoder.GetSoftwareBitmapAsync(
            WindowsBitmapPixelFormat.Bgra8,
            WindowsBitmapAlphaMode.Premultiplied);
    }

    private sealed class EngineSelection
    {
        public EngineSelection(OcrEngine? engine, string languageTag)
        {
            Engine = engine;
            LanguageTag = languageTag;
        }

        public OcrEngine? Engine { get; }

        public string LanguageTag { get; }
    }
}
