using System;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using HaloCap.Diagnostics;
using HaloCap.Models;

namespace HaloCap.Services;

internal static class SettingsStore
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        PropertyNameCaseInsensitive = true,
        Converters = { new JsonStringEnumConverter() }
    };

    public static string DirectoryPath => Log.AppDataDirectory;

    public static string FilePath => Path.Combine(DirectoryPath, "settings.json");

    public static AppSettings Load()
    {
        try
        {
            if (!File.Exists(FilePath))
            {
                return new AppSettings();
            }

            var json = File.ReadAllText(FilePath);
            var settings = JsonSerializer.Deserialize<AppSettings>(json, JsonOptions) ?? new AppSettings();

            if (settings.Hotkey is null || !settings.Hotkey.IsValid)
            {
                settings.Hotkey = HotkeyDefinition.Default;
            }

            var defaults = new AppSettings();
            foreach (var action in new[]
                     {
                         RadialAction.Screenshot, RadialAction.Ocr, RadialAction.Record, RadialAction.Pin
                     })
            {
                var hotkey = settings.GetActionHotkey(action);
                if (hotkey is null || !hotkey.IsValid)
                {
                    settings.SetActionHotkey(action, defaults.GetActionHotkey(action).Clone());
                }
            }

            return settings;
        }
        catch (Exception ex)
        {
            Log.Error("读取设置失败，回退到默认设置", ex);
            return new AppSettings();
        }
    }

    public static void Save(AppSettings settings)
    {
        Directory.CreateDirectory(DirectoryPath);
        var json = JsonSerializer.Serialize(settings, JsonOptions);
        var temporaryPath = FilePath + ".tmp";

        File.WriteAllText(temporaryPath, json, new UTF8Encoding(false));
        File.Move(temporaryPath, FilePath, true);
    }
}
