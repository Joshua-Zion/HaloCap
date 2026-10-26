using System;
using System.Threading;
using System.Windows;
using System.Windows.Media.Imaging;
using HaloCap.Diagnostics;

namespace HaloCap.Services;

internal static class ImageClipboard
{
    public static void SetImage(BitmapSource source)
    {
        Exception? lastError = null;

        for (var attempt = 0; attempt < 6; attempt++)
        {
            try
            {
                var data = new DataObject();
                data.SetImage(source);
                Clipboard.SetDataObject(data, true);
                return;
            }
            catch (Exception ex)
            {
                lastError = ex;

                if (attempt < 5)
                {
                    Thread.Sleep(35);
                }
            }
        }

        Log.Error("复制图片到剪贴板失败", lastError);
        throw new InvalidOperationException("剪贴板正被其他程序占用，请稍后重试。", lastError);
    }

    public static void SetText(string text)
    {
        Exception? lastError = null;

        for (var attempt = 0; attempt < 6; attempt++)
        {
            try
            {
                Clipboard.SetText(text);
                return;
            }
            catch (Exception ex)
            {
                lastError = ex;

                if (attempt < 5)
                {
                    Thread.Sleep(35);
                }
            }
        }

        Log.Error("复制文字到剪贴板失败", lastError);
        throw new InvalidOperationException("剪贴板正被其他程序占用，请稍后重试。", lastError);
    }
}
