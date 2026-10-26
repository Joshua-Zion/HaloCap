using System.Collections.Generic;
using System.Text.Json.Serialization;
using System.Windows.Input;

namespace HaloCap.Models;

public sealed class HotkeyDefinition
{
    public ModifierKeys Modifiers { get; set; } = ModifierKeys.Control | ModifierKeys.Alt;

    public Key Key { get; set; } = Key.D;

    public HotkeyDefinition()
    {
    }

    public HotkeyDefinition(ModifierKeys modifiers, Key key)
    {
        Modifiers = modifiers;
        Key = key;
    }

    public static HotkeyDefinition Default => new();

    [JsonIgnore]
    public int VirtualKey => KeyInterop.VirtualKeyFromKey(Key);

    [JsonIgnore]
    public bool IsValid =>
        Key != Key.None &&
        !IsModifierKey(Key) &&
        Modifiers != ModifierKeys.None &&
        ((int)Modifiers & ~(int)AllowedModifiers) == 0;

    [JsonIgnore]
    public string DisplayText => Format(Modifiers, Key);

    public static ModifierKeys AllowedModifiers =>
        ModifierKeys.Control | ModifierKeys.Alt | ModifierKeys.Shift | ModifierKeys.Windows;

    public HotkeyDefinition Clone() => new(Modifiers, Key);

    public static bool IsModifierKey(Key key) => key is
        Key.LeftCtrl or Key.RightCtrl or
        Key.LeftAlt or Key.RightAlt or
        Key.LeftShift or Key.RightShift or
        Key.LWin or Key.RWin or
        Key.System;

    public static string Format(ModifierKeys modifiers, Key key)
    {
        var parts = new List<string>(5);
        if ((modifiers & ModifierKeys.Control) != 0) parts.Add("Ctrl");
        if ((modifiers & ModifierKeys.Alt) != 0) parts.Add("Alt");
        if ((modifiers & ModifierKeys.Shift) != 0) parts.Add("Shift");
        if ((modifiers & ModifierKeys.Windows) != 0) parts.Add("Win");
        parts.Add(GetKeyName(key));
        return string.Join(" + ", parts);
    }

    public static string GetKeyName(Key key)
    {
        var code = (int)key;

        if (code >= (int)Key.A && code <= (int)Key.Z)
        {
            return ((char)('A' + (code - (int)Key.A))).ToString();
        }

        if (code >= (int)Key.D0 && code <= (int)Key.D9)
        {
            return ((char)('0' + (code - (int)Key.D0))).ToString();
        }

        return key switch
        {
            Key.None => "—",
            Key.Space => "Space",
            Key.Escape => "Esc",
            Key.Enter => "Enter",
            Key.Tab => "Tab",
            Key.Back => "Backspace",
            Key.Delete => "Delete",
            Key.Insert => "Insert",
            Key.Home => "Home",
            Key.End => "End",
            Key.PageUp => "PageUp",
            Key.PageDown => "PageDown",
            Key.Up => "Up",
            Key.Down => "Down",
            Key.Left => "Left",
            Key.Right => "Right",
            Key.CapsLock => "CapsLock",
            Key.Snapshot => "PrintScreen",
            _ => key.ToString()
        };
    }
}
