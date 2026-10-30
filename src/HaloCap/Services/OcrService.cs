using System;
using System.Diagnostics;
using System.Threading.Tasks;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Diagnostics;
using HaloCap.Models;

namespace HaloCap.Services;

internal sealed class OcrService : IDisposable
{
    public const string ChineseLanguageGuidance =
        "未找到中文文字识别语言包。请打开「设置 › 时间和语言 › 语言和区域」，添加「中文(简体，中国)」，" +
        "在语言选项的「可选语言功能」中安装「光学字符识别」，安装后重新启动 HaloCap。";

    public const string GeneralGuidance =
        "当前系统没有可用的文字识别语言组件。请打开「设置 › 时间和语言 › 语言和区域」，" +
        "为已安装的语言添加「光学字符识别」功能，安装后重新启动 HaloCap。";

    private readonly object _engineLock = new();
    private readonly NativeOcrEngine _native = new();
    private ModelOcrEngine? _model;
    private bool _disposed;

    public async Task<OcrOutcome> RecognizeAsync(
        BitmapSource image,
        OcrSettings settings,
        OcrEngineKind? engineOverride = null)
    {
        var normalized = settings.Clone();
        normalized.Normalize();
        var engineKind = engineOverride ?? normalized.Engine;
        var started = Stopwatch.GetTimestamp();

        try
        {
            var source = PrepareSource(image);
            OcrEngineResult result;

            if (engineKind == OcrEngineKind.Model)
            {
                var model = GetModel();
                result = await Task.Run(() => model.Recognize(source)).ConfigureAwait(false);
            }
            else
            {
                result = await Task.Run(() => _native.RecognizeAsync(source)).ConfigureAwait(false);
            }

            return new OcrOutcome
            {
                Status = result.Status,
                Lines = result.Lines,
                Text = string.Join(Environment.NewLine, result.Lines),
                Message = result.Message,
                EngineLabel = result.EngineLabel,
                Engine = engineKind,
                ElapsedMilliseconds = ElapsedMilliseconds(started)
            };
        }
        catch (Exception ex)
        {
            Log.Error($"OCR 识别失败（{engineKind}）", ex);
            return new OcrOutcome
            {
                Status = OcrStatus.Failed,
                Message = engineKind == OcrEngineKind.Model
                    ? "模型识别失败：" + ex.Message + Environment.NewLine + Environment.NewLine +
                      "可以在顶部切回「原生」识别。"
                    : "文字识别失败：" + ex.Message,
                EngineLabel = engineKind == OcrEngineKind.Model
                    ? "PP-OCRv6 tiny"
                    : "Windows 原生",
                Engine = engineKind,
                ElapsedMilliseconds = ElapsedMilliseconds(started)
            };
        }
    }

    public void ClearEngineCache()
    {
        _native.Clear();
        ModelOcrEngine? stale;

        lock (_engineLock)
        {
            stale = _model;
            _model = null;
        }

        if (stale is not null)
        {
            _ = Task.Run(() =>
            {
                try
                {
                    stale.Dispose();
                }
                catch (Exception ex)
                {
                    Log.Error("释放 OCR 模型引擎失败", ex);
                }
            });
        }
    }

    public void Dispose()
    {
        if (_disposed)
        {
            return;
        }

        _disposed = true;
        _native.Clear();
        ModelOcrEngine? stale;

        lock (_engineLock)
        {
            stale = _model;
            _model = null;
        }

        stale?.Dispose();
    }

    private ModelOcrEngine GetModel()
    {
        lock (_engineLock)
        {
            if (_disposed)
            {
                throw new ObjectDisposedException(nameof(OcrService));
            }

            return _model ??= new ModelOcrEngine();
        }
    }

    private static BitmapSource PrepareSource(BitmapSource image)
    {
        var snapshot = image.IsFrozen
            ? image
            : (BitmapSource)image.CloneCurrentValue();

        if (!snapshot.IsFrozen)
        {
            snapshot.Freeze();
        }

        if (snapshot.Format == PixelFormats.Bgra32)
        {
            return snapshot;
        }

        var converted = new FormatConvertedBitmap(snapshot, PixelFormats.Bgra32, null, 0);
        converted.Freeze();
        return converted;
    }

    private static long ElapsedMilliseconds(long started) =>
        (long)Math.Round((Stopwatch.GetTimestamp() - started) * 1000.0 / Stopwatch.Frequency);
}
