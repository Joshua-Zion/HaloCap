using System;
using System.Collections.Generic;
using System.Windows.Input;
using HaloCap.Models;
using HaloCap.UI;

namespace HaloCap.CoordinateTests;

internal static class Program
{
    private static int Main()
    {
        var tests = new List<(string Name, Action Test)>
        {
            ("NormalizeEdges sorts reversed drags", NormalizeEdgesSortsReversedDrags),
            ("NormalizeSelection clamps to monitor", NormalizeSelectionClampsToMonitor),
            ("NormalizeSelection handles inverted coordinates", NormalizeSelectionHandlesInvertedCoordinates),
            ("Intersect returns overlapping area", IntersectReturnsOverlap),
            ("Intersect returns empty for disjoint rects", IntersectReturnsEmptyForDisjointRects),
            ("ClampTo keeps rect inside container", ClampToKeepsRectInside),
            ("ClampTo preserves size when possible", ClampToPreservesSize),
            ("ToLocal and ToVirtual round trip", LocalVirtualRoundTrip),
            ("Negative monitor origin converts correctly", NegativeMonitorOrigin),
            ("MakeEven trims odd dimensions", MakeEvenTrimsOddDimensions),
            ("MakeEven never returns negative size", MakeEvenNeverReturnsNegative),
            ("IsUsableSelection enforces minimum", IsUsableSelectionEnforcesMinimum),
            ("Dip and pixel conversion round trip", DipPixelRoundTrip),
            ("Contains excludes right and bottom edges", ContainsExcludesEdges),
            ("150 percent secondary monitor crop", HighDpiSecondaryMonitorCrop),
            ("Single function key hotkey is valid", SingleFunctionKeyHotkeyIsValid),
            ("Single letter hotkey is valid", SingleLetterHotkeyIsValid),
            ("Single control key hotkey is rejected", SingleControlKeyHotkeyIsRejected),
            ("Bare modifier key is rejected", BareModifierKeyIsRejected),
            ("Combination hotkey stays valid", CombinationHotkeyIsValid),
            ("Single key display has no plus sign", SingleKeyDisplayIsPlain),
            ("Radial size presets map to window sizes", RadialSizePresetsMapToWindowSizes)
        };

        var failures = 0;

        foreach (var (name, test) in tests)
        {
            try
            {
                test();
                Console.WriteLine("PASS  " + name);
            }
            catch (Exception ex)
            {
                failures++;
                Console.WriteLine("FAIL  " + name);
                Console.WriteLine("      " + ex.Message);
            }
        }

        Console.WriteLine();
        Console.WriteLine($"{tests.Count - failures}/{tests.Count} passed.");

        return failures == 0 ? 0 : 1;
    }

    private static void NormalizeEdgesSortsReversedDrags()
    {
        var rect = ScreenGeometry.NormalizeEdges(500, 400, 120, 80);
        Equal(new PhysicalRect(120, 80, 380, 320), rect);
    }

    private static void NormalizeSelectionClampsToMonitor()
    {
        var rect = ScreenGeometry.NormalizeSelection(300, 200, -100, 50, 1000, 800);
        Equal(new PhysicalRect(0, 50, 300, 150), rect);
    }

    private static void NormalizeSelectionHandlesInvertedCoordinates()
    {
        var rect = ScreenGeometry.NormalizeSelection(900, 700, 100, 100, 1920, 1080);
        Equal(new PhysicalRect(100, 100, 800, 600), rect);
    }

    private static void IntersectReturnsOverlap()
    {
        var overlap = ScreenGeometry.Intersect(
            new PhysicalRect(100, 100, 400, 300),
            new PhysicalRect(300, 200, 400, 300));

        Equal(new PhysicalRect(300, 200, 200, 200), overlap);
    }

    private static void IntersectReturnsEmptyForDisjointRects()
    {
        var overlap = ScreenGeometry.Intersect(
            new PhysicalRect(0, 0, 100, 100),
            new PhysicalRect(200, 200, 100, 100));

        True(overlap.IsEmpty, "disjoint intersect must be empty");
    }

    private static void ClampToKeepsRectInside()
    {
        var clamped = ScreenGeometry.ClampTo(
            new PhysicalRect(-50, 50, 300, 200),
            new PhysicalRect(0, 0, 1920, 1080));

        Equal(new PhysicalRect(0, 50, 300, 200), clamped);
    }

    private static void ClampToPreservesSize()
    {
        var clamped = ScreenGeometry.ClampTo(
            new PhysicalRect(1800, 1000, 400, 300),
            new PhysicalRect(0, 0, 1920, 1080));

        Equal(400, clamped.Width);
        Equal(300, clamped.Height);
        True(clamped.Right <= 1920, "right edge must stay inside");
        True(clamped.Bottom <= 1080, "bottom edge must stay inside");
    }

    private static void LocalVirtualRoundTrip()
    {
        var monitor = new PhysicalRect(1920, 0, 2560, 1440);
        var local = new PhysicalRect(280, 300, 800, 600);

        var virtualRect = ScreenGeometry.ToVirtual(local, monitor);
        Equal(new PhysicalRect(2200, 300, 800, 600), virtualRect);
        Equal(local, ScreenGeometry.ToLocal(virtualRect, monitor));
    }

    private static void NegativeMonitorOrigin()
    {
        var monitor = new PhysicalRect(-1920, -200, 1920, 1080);
        var selection = new PhysicalRect(-1500, -100, 300, 200);

        Equal(new PhysicalRect(420, 100, 300, 200), ScreenGeometry.ToLocal(selection, monitor));
        Equal(selection, ScreenGeometry.ToVirtual(
            new PhysicalRect(420, 100, 300, 200),
            monitor));
    }

    private static void MakeEvenTrimsOddDimensions()
    {
        var even = ScreenGeometry.MakeEven(new PhysicalRect(10, 20, 801, 599));
        Equal(new PhysicalRect(10, 20, 800, 598), even);
    }

    private static void MakeEvenNeverReturnsNegative()
    {
        var even = ScreenGeometry.MakeEven(new PhysicalRect(10, 20, 1, 0));
        Equal(0, even.Width);
        Equal(0, even.Height);
    }

    private static void IsUsableSelectionEnforcesMinimum()
    {
        True(!ScreenGeometry.IsUsableSelection(new PhysicalRect(0, 0, 3, 100)), "width below minimum");
        True(!ScreenGeometry.IsUsableSelection(new PhysicalRect(0, 0, 100, 3)), "height below minimum");
        True(ScreenGeometry.IsUsableSelection(new PhysicalRect(0, 0, 4, 4)), "exact minimum is valid");
    }

    private static void DipPixelRoundTrip()
    {
        var pixels = ScreenGeometry.DipToPixel(320, 1.5);
        Equal(480.0, pixels, 0.0001);
        Equal(320.0, ScreenGeometry.PixelToDip(480, 1.5), 0.0001);
    }

    private static void ContainsExcludesEdges()
    {
        var rect = new PhysicalRect(10, 20, 100, 50);
        True(rect.Contains(10, 20), "left/top inclusive");
        True(rect.Contains(109, 69), "last pixel inside");
        True(!rect.Contains(110, 40), "right exclusive");
        True(!rect.Contains(50, 70), "bottom exclusive");
    }

    private static void HighDpiSecondaryMonitorCrop()
    {
        const double scale = 1.5;
        var monitor = new PhysicalRect(1920, 0, 2560, 1440);
        var virtualSelection = new PhysicalRect(2200, 301, 801, 599);
        var local = ScreenGeometry.ToLocal(virtualSelection, monitor);
        var even = ScreenGeometry.MakeEven(local);

        Equal(new PhysicalRect(280, 301, 800, 598), even);
        Equal(800, (int)Math.Round(ScreenGeometry.PixelToDip(even.Width, scale) * scale));
        True(even.Right <= monitor.Width, "crop must fit the monitor");
    }

    private static void SingleFunctionKeyHotkeyIsValid()
    {
        var functionKey = new HotkeyDefinition(ModifierKeys.None, Key.F8);
        True(functionKey.IsValid, "F8 alone must be valid");
        True(functionKey.IsSingleKey, "F8 alone is a single-key hotkey");
    }

    private static void SingleLetterHotkeyIsValid()
    {
        var letter = new HotkeyDefinition(ModifierKeys.None, Key.K);
        True(letter.IsValid, "bare letter must be valid");
        True(letter.IsSingleKey, "bare letter is a single-key hotkey");
    }

    private static void SingleControlKeyHotkeyIsRejected()
    {
        True(!new HotkeyDefinition(ModifierKeys.None, Key.Space).IsValid, "space alone must be rejected");
        True(!new HotkeyDefinition(ModifierKeys.None, Key.Enter).IsValid, "enter alone must be rejected");
        True(!new HotkeyDefinition(ModifierKeys.None, Key.Escape).IsValid, "escape alone must be rejected");
        True(!new HotkeyDefinition(ModifierKeys.None, Key.Left).IsValid, "arrow alone must be rejected");
    }

    private static void BareModifierKeyIsRejected()
    {
        True(!new HotkeyDefinition(ModifierKeys.None, Key.LeftCtrl).IsValid, "modifier only must be rejected");
        True(!new HotkeyDefinition(ModifierKeys.Control, Key.LeftCtrl).IsValid, "ctrl plus ctrl must be rejected");
    }

    private static void CombinationHotkeyIsValid()
    {
        var combo = new HotkeyDefinition(ModifierKeys.Control | ModifierKeys.Alt, Key.D);
        True(combo.IsValid, "ctrl+alt+d must be valid");
        True(!combo.IsSingleKey, "combo is not a single-key hotkey");
        Equal("Ctrl + Alt + D", combo.DisplayText);
    }

    private static void SingleKeyDisplayIsPlain()
    {
        Equal("F8", new HotkeyDefinition(ModifierKeys.None, Key.F8).DisplayText);
        Equal("5", new HotkeyDefinition(ModifierKeys.None, Key.D5).DisplayText);
    }

    private static void RadialSizePresetsMapToWindowSizes()
    {
        Equal(320.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Small), 0.0001);
        Equal(380.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Medium), 0.0001);
        Equal(440.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Large), 0.0001);
    }

    private static void Equal(PhysicalRect expected, PhysicalRect actual)
    {
        if (!expected.Equals(actual))
        {
            throw new InvalidOperationException($"expected {expected}, got {actual}");
        }
    }

    private static void Equal(int expected, int actual)
    {
        if (expected != actual)
        {
            throw new InvalidOperationException($"expected {expected}, got {actual}");
        }
    }

    private static void Equal(double expected, double actual, double tolerance)
    {
        if (Math.Abs(expected - actual) > tolerance)
        {
            throw new InvalidOperationException($"expected {expected}, got {actual}");
        }
    }

    private static void Equal(string expected, string actual)
    {
        if (expected != actual)
        {
            throw new InvalidOperationException($"expected {expected}, got {actual}");
        }
    }

    private static void True(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
