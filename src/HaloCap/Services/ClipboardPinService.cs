using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Interop;

namespace HaloCap.Services;

internal sealed class ClipboardPinService
{
    private const long MaxImagePixels = 64_000_000;
    private const long MaxImageBytes = 256_000_000;
    private const int MaxFileImagePins = 16;
    private static readonly TimeSpan RegexTimeout = TimeSpan.FromSeconds(1);
    private static readonly Regex RgbPattern = new(
        @"^\s*(\d+(?:\.\d*)?|\.\d+)(?:\s*[,，]\s*|\s+)(\d+(?:\.\d*)?|\.\d+)(?:\s*[,，]\s*|\s+)(\d+(?:\.\d*)?|\.\d+)\s*$",
        RegexOptions.CultureInvariant | RegexOptions.NonBacktracking, RegexTimeout);
    private ClipboardSnapshot? _snapshot;
    private uint? _pastedFileImageSequence;
    private bool _readingClipboard;

    public bool CanPin => ReadClipboard() is { HasContent: true };

    public IReadOnlyList<ClipboardPinContent> CreatePins(bool convertFilePathsToImages, double scale)
    {
        var snapshot = ReadClipboard();
        return snapshot is null ? Array.Empty<ClipboardPinContent>() : CreatePins(snapshot, convertFilePathsToImages, scale);
    }

    internal IReadOnlyList<ClipboardPinContent> CreatePins(ClipboardSnapshot snapshot, bool convertFilePathsToImages, double scale)
    {
        var result = new List<ClipboardPinContent>();
        if (snapshot.FilePaths.Length > 0)
        {
            var paths = new List<string>();
            var remainingImageBytes = MaxImageBytes;
            var convertImages = convertFilePathsToImages && _pastedFileImageSequence != snapshot.Sequence;
            foreach (var path in snapshot.FilePaths)
            {
                var image = convertImages && result.Count < MaxFileImagePins
                    ? TryLoadLocalImage(path, remainingImageBytes)
                    : null;
                if (image is not null)
                {
                    remainingImageBytes -= ImageByteCount(image);
                    result.Add(new ClipboardPinContent(image, ClipboardPinKind.FileImage, path, snapshot.Sequence));
                }
                else
                {
                    paths.Add(path);
                }
            }

            if (paths.Count > 0)
            {
                var text = string.Join(Environment.NewLine, paths);
                result.Add(new ClipboardPinContent(RenderText(text, scale), ClipboardPinKind.FilePath, text, snapshot.Sequence));
            }
        }
        else if (snapshot.Image is not null)
        {
            result.Add(new ClipboardPinContent(snapshot.Image, ClipboardPinKind.Image, null, snapshot.Sequence));
        }
        else if (!string.IsNullOrWhiteSpace(snapshot.Text))
        {
            var isColor = TryParseColor(snapshot.Text, out var color);
            result.Add(new ClipboardPinContent(
                isColor ? RenderColor(color, scale) : RenderText(snapshot.Text, scale),
                isColor ? ClipboardPinKind.Color : ClipboardPinKind.Text,
                snapshot.Text,
                snapshot.Sequence));
        }

        return result;
    }

    public void MarkPasted(ClipboardPinContent content)
    {
        if (content.Kind == ClipboardPinKind.FileImage)
        {
            _pastedFileImageSequence = content.Sequence;
        }
    }

    private ClipboardSnapshot? ReadClipboard()
    {
        var sequence = NativeMethods.GetClipboardSequenceNumber();
        if (_snapshot?.Sequence == sequence)
        {
            return _snapshot;
        }
        if (_readingClipboard)
        {
            return null;
        }

        _readingClipboard = true;
        try
        {
            for (var attempt = 0; attempt < 3; attempt++)
            {
                sequence = NativeMethods.GetClipboardSequenceNumber();
                var data = Clipboard.GetDataObject();
                var snapshot = data is null
                    ? new ClipboardSnapshot(sequence, null, null, Array.Empty<string>())
                    : ReadDataObject(data, sequence);
                if (sequence == NativeMethods.GetClipboardSequenceNumber())
                {
                    _snapshot = snapshot;
                    return snapshot;
                }
            }
        }
        catch (ExternalException)
        {
        }
        finally
        {
            _readingClipboard = false;
        }

        _snapshot = null;
        return null;
    }

    internal static ClipboardSnapshot ReadDataObject(IDataObject data, uint sequence)
    {
        if (data.GetDataPresent(DataFormats.FileDrop, true) && data.GetData(DataFormats.FileDrop, true) is string[] files)
        {
            var paths = files.Where(path => !string.IsNullOrWhiteSpace(path)).ToArray();
            if (paths.Length > 0)
            {
                return new ClipboardSnapshot(sequence, null, null, paths);
            }
        }

        BitmapSource? image = null;
        if (data.GetDataPresent("PNG", false))
        {
            try
            {
                var png = data.GetData("PNG", false);
                image = png switch
                {
                    Stream stream => DecodeImage(stream),
                    byte[] bytes => DecodeImage(new MemoryStream(bytes, writable: false)),
                    _ => null
                };
            }
            catch (Exception ex) when (ex is IOException or FileFormatException or NotSupportedException or ArgumentException)
            {
            }
        }

        if (image is null && data.GetDataPresent(DataFormats.Bitmap, true))
        {
            var bitmap = data.GetData(DataFormats.Bitmap, true);
            if (bitmap is BitmapSource source && (long)source.PixelWidth * source.PixelHeight <= MaxImagePixels && ImageByteCount(source) <= MaxImageBytes)
            {
                image = source.IsFrozen ? source : source.CloneCurrentValue();
                image.Freeze();
            }
            else if (bitmap is System.Drawing.Bitmap drawingBitmap &&
                     (long)drawingBitmap.Width * drawingBitmap.Height * Math.Max(4, (System.Drawing.Image.GetPixelFormatSize(drawingBitmap.PixelFormat) + 7) / 8) <= MaxImageBytes)
            {
                using var stream = new MemoryStream();
                drawingBitmap.Save(stream, System.Drawing.Imaging.ImageFormat.Png);
                stream.Position = 0;
                image = DecodeImage(stream);
            }
        }

        if (image is not null)
        {
            return new ClipboardSnapshot(sequence, image, null, Array.Empty<string>());
        }

        string? text = null;
        if (data.GetDataPresent(DataFormats.Html, false))
        {
            var htmlData = data.GetData(DataFormats.Html, false);
            var html = htmlData switch
            {
                string value => value,
                byte[] bytes => Encoding.UTF8.GetString(bytes),
                _ => null
            };
            if (htmlData is Stream stream)
            {
                if (stream.CanSeek)
                {
                    stream.Position = 0;
                }
                using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true, leaveOpen: true);
                html = reader.ReadToEnd();
            }
            if (!string.IsNullOrEmpty(html))
            {
                try
                {
                    text = HtmlToText(html);
                }
                catch (RegexMatchTimeoutException)
                {
                }
            }
        }

        if (string.IsNullOrWhiteSpace(text))
        {
            text = data.GetDataPresent(DataFormats.UnicodeText, true)
                ? data.GetData(DataFormats.UnicodeText, true) as string
                : data.GetDataPresent(DataFormats.Text, true)
                    ? data.GetData(DataFormats.Text, true) as string
                    : null;
        }

        return new ClipboardSnapshot(sequence, null, text?.TrimEnd('\0'), Array.Empty<string>());
    }

    internal static bool TryParseColor(string text, out Color color)
    {
        color = default;
        var value = text.Trim();
        if (value.StartsWith('#'))
        {
            var hex = value[1..];
            if (hex.Length is not (3 or 4 or 6 or 8) || !hex.All(Uri.IsHexDigit))
            {
                return false;
            }

            if (hex.Length is 3 or 4)
            {
                hex = string.Concat(hex.Select(character => new string(character, 2)));
            }

            var red = byte.Parse(hex[..2], NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var green = byte.Parse(hex.Substring(2, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var blue = byte.Parse(hex.Substring(4, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture);
            var alpha = hex.Length == 8
                ? byte.Parse(hex.Substring(6, 2), NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                : byte.MaxValue;
            color = Color.FromArgb(alpha, red, green, blue);
            return true;
        }

        if (value.StartsWith("RGB", StringComparison.OrdinalIgnoreCase))
        {
            value = value[3..].TrimStart(' ', '\t', ':', '：');
        }

        if (value.StartsWith('('))
        {
            if (!value.EndsWith(')'))
            {
                return false;
            }
            value = value[1..^1];
        }

        var match = RgbPattern.Match(value);
        if (!match.Success)
        {
            return false;
        }

        var normalized = value.Contains('.');
        var channels = new byte[3];
        for (var index = 0; index < channels.Length; index++)
        {
            if (!double.TryParse(match.Groups[index + 1].Value, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out var channel) ||
                channel < 0 || channel > (normalized ? 1 : 255))
            {
                return false;
            }
            channels[index] = (byte)Math.Round(normalized ? channel * 255 : channel, MidpointRounding.AwayFromZero);
        }

        color = Color.FromRgb(channels[0], channels[1], channels[2]);
        return true;
    }

    internal static string HtmlToText(string html)
    {
        const string startMarker = "<!--StartFragment-->";
        const string endMarker = "<!--EndFragment-->";
        var start = html.IndexOf(startMarker, StringComparison.OrdinalIgnoreCase);
        var end = html.IndexOf(endMarker, StringComparison.OrdinalIgnoreCase);
        if (start >= 0 && end >= start + startMarker.Length)
        {
            html = html.Substring(start + startMarker.Length, end - start - startMarker.Length);
        }
        else if (html.StartsWith("Version:", StringComparison.OrdinalIgnoreCase))
        {
            var startMatch = Regex.Match(html, @"StartFragment:\s*(\d+)", RegexOptions.CultureInvariant, RegexTimeout);
            var endMatch = Regex.Match(html, @"EndFragment:\s*(\d+)", RegexOptions.CultureInvariant, RegexTimeout);
            var bytes = Encoding.UTF8.GetBytes(html);
            if (int.TryParse(startMatch.Groups[1].Value, out start) && int.TryParse(endMatch.Groups[1].Value, out end) &&
                start >= 0 && end > start && end <= bytes.Length)
            {
                html = Encoding.UTF8.GetString(bytes, start, end - start);
            }
            else
            {
                start = html.IndexOf('<');
                html = start >= 0 ? html[start..] : string.Empty;
            }
        }

        var options = RegexOptions.IgnoreCase | RegexOptions.Singleline | RegexOptions.CultureInvariant;
        html = Regex.Replace(html, @"<(script|style|head|iframe|object)\b[^>]*>.*?</\1\s*>", string.Empty, options, RegexTimeout);
        html = Regex.Replace(html, @"<!--.*?-->", string.Empty, options, RegexTimeout);
        html = Regex.Replace(html, @"<br\b[^>]*>|</(?:p|div|li|h[1-6]|tr|pre|blockquote|section)\s*>", "\n", options, RegexTimeout);
        html = Regex.Replace(html, @"</t[dh]\s*>", "\t", options, RegexTimeout);
        html = Regex.Replace(html, @"<[^>]*>", string.Empty, options, RegexTimeout);
        return WebUtility.HtmlDecode(html).Replace("\r\n", "\n").Trim();
    }

    private static BitmapSource? TryLoadLocalImage(string path, long remainingImageBytes)
    {
        try
        {
            if (!Path.IsPathFullyQualified(path))
            {
                return null;
            }
            var root = Path.GetPathRoot(path)!;
            if (root.Length != 3 || root[1] != ':' || new DriveInfo(root).DriveType == DriveType.Network)
            {
                return null;
            }

            // 重解析路径可能指向网络位置，逐级检查后再打开文件。
            var current = root;
            foreach (var segment in path[root.Length..].Split(new[] { '\\', '/' }, StringSplitOptions.RemoveEmptyEntries))
            {
                current = Path.Combine(current, segment);
                if ((File.GetAttributes(current) & FileAttributes.ReparsePoint) != 0)
                {
                    return null;
                }
            }

            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
            return DecodeImage(stream, remainingImageBytes);
        }
        catch (Exception ex) when (ex is IOException or FileFormatException or UnauthorizedAccessException or NotSupportedException or ArgumentException)
        {
            return null;
        }
    }

    private static long ImageByteCount(BitmapSource image) =>
        (long)image.PixelWidth * image.PixelHeight * Math.Max(4, (image.Format.BitsPerPixel + 7) / 8);

    private static BitmapSource? DecodeImage(Stream stream, long maxImageBytes = MaxImageBytes)
    {
        if (stream.CanSeek)
        {
            stream.Position = 0;
        }
        var decoder = BitmapDecoder.Create(stream, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnDemand);
        var frame = decoder.Frames[0];
        if ((long)frame.PixelWidth * frame.PixelHeight > MaxImagePixels || ImageByteCount(frame) > maxImageBytes)
        {
            return null;
        }
        var image = new CachedBitmap(frame, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
        image.Freeze();
        return image;
    }

    internal static BitmapSource RenderText(string text, double scale)
    {
        var visibleText = text.Length > 32_768 ? text[..32_768] + "…" : text;
        var formatted = new FormattedText(visibleText, CultureInfo.CurrentUICulture, FlowDirection.LeftToRight,
            new Typeface("Microsoft YaHei UI, Segoe UI"), 17, new SolidColorBrush(Color.FromRgb(35, 41, 51)), scale)
        {
            MaxTextWidth = 640,
            MaxTextHeight = 2800,
            Trimming = TextTrimming.CharacterEllipsis
        };
        var width = Math.Clamp(Math.Ceiling(formatted.WidthIncludingTrailingWhitespace) + 32, 160, 672);
        var height = Math.Clamp(Math.Ceiling(formatted.Height) + 32, 56, 2832);
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            context.DrawRectangle(Brushes.White, null, new Rect(0, 0, width, height));
            context.DrawText(formatted, new Point(16, 16));
        }
        return Render(visual, width, height, scale);
    }

    internal static BitmapSource RenderColor(Color color, double scale)
    {
        const int width = 280;
        const int height = 190;
        var visual = new DrawingVisual();
        using (var context = visual.RenderOpen())
        {
            context.DrawRectangle(Brushes.White, null, new Rect(0, 120, width, height - 120));
            context.DrawRectangle(new SolidColorBrush(color), null, new Rect(0, 0, width, 120));
            var hex = $"#{color.R:X2}{color.G:X2}{color.B:X2}" + (color.A < 255 ? $"{color.A:X2}" : string.Empty);
            context.DrawText(new FormattedText(hex, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
                new Typeface("Segoe UI"), 18, Brushes.Black, scale), new Point(16, 132));
            context.DrawText(new FormattedText($"RGB  {color.R}, {color.G}, {color.B}", CultureInfo.InvariantCulture,
                FlowDirection.LeftToRight, new Typeface("Segoe UI"), 13, Brushes.DimGray, scale), new Point(16, 160));
        }
        return Render(visual, width, height, scale);
    }

    private static BitmapSource Render(DrawingVisual visual, double width, double height, double scale)
    {
        var image = new RenderTargetBitmap((int)Math.Ceiling(width * scale), (int)Math.Ceiling(height * scale),
            96 * scale, 96 * scale, PixelFormats.Pbgra32);
        image.Render(visual);
        image.Freeze();
        return image;
    }
}

internal sealed record ClipboardSnapshot(uint Sequence, BitmapSource? Image, string? Text, string[] FilePaths)
{
    public bool HasContent => Image is not null || !string.IsNullOrWhiteSpace(Text) || FilePaths.Length > 0;
}

internal enum ClipboardPinKind
{
    Image,
    Color,
    Text,
    FileImage,
    FilePath
}

internal sealed record ClipboardPinContent(BitmapSource Image, ClipboardPinKind Kind, string? Text, uint Sequence);
