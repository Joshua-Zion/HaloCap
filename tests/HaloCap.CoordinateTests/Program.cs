using System;
using System.Collections.Generic;
using System.Threading;
using System.Windows;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Models;
using HaloCap.Services;
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
            ("Radial size presets map to window sizes", RadialSizePresetsMapToWindowSizes),
            ("MoveWithin keeps selection inside", MoveWithinKeepsSelectionInside),
            ("MoveWithin preserves size", MoveWithinPreservesSize),
            ("ResizeEdges enforces minimum size", ResizeEdgesEnforcesMinimum),
            ("ResizeEdges clamps to container", ResizeEdgesClampsToContainer),
            ("Annotation compose draws stroke", AnnotationComposeDrawsStroke),
            ("Annotation compose fills shape", AnnotationComposeFillsShape),
            ("Square constraint keeps sides equal", SquareConstraintKeepsSidesEqual),
            ("Arrow snap locks to 45 degrees", ArrowSnapLocksToAngles),
            ("Mosaic snap aligns to blocks", MosaicSnapAlignsToBlocks),
            ("Mosaic tile averages blocks", MosaicTileAveragesBlocks)
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
        Equal(360.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Small), 0.0001);
        Equal(420.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Medium), 0.0001);
        Equal(480.0, RadialMenuWindow.GetWindowSizeDip(RadialMenuSize.Large), 0.0001);

        var padding = RadialMenuWindow.MenuPadding;
        True(padding >= 12, "padding must leave room for the drop shadow");

        Equal((360.0 - (2 * padding)) / RadialMenuWindow.MenuSize,
            RadialMenuWindow.GetContentScale(RadialMenuSize.Small), 0.0001);
        Equal(1.0, RadialMenuWindow.GetContentScale(RadialMenuSize.Medium), 0.0001);
        Equal((480.0 - (2 * padding)) / RadialMenuWindow.MenuSize,
            RadialMenuWindow.GetContentScale(RadialMenuSize.Large), 0.0001);
    }

    private static void MoveWithinKeepsSelectionInside()
    {
        var origin = new PhysicalRect(200, 150, 300, 200);
        var moved = ScreenGeometry.MoveWithin(origin, -500, -400, 1920, 1080);

        Equal(new PhysicalRect(0, 0, 300, 200), moved);

        var pushed = ScreenGeometry.MoveWithin(origin, 5000, 5000, 1920, 1080);
        Equal(new PhysicalRect(1620, 880, 300, 200), pushed);
        True(pushed.Right <= 1920 && pushed.Bottom <= 1080, "moved selection must stay inside");
    }

    private static void MoveWithinPreservesSize()
    {
        var origin = new PhysicalRect(40, 40, 320, 240);
        var moved = ScreenGeometry.MoveWithin(origin, 25, -25, 1920, 1080);

        Equal(320, moved.Width);
        Equal(240, moved.Height);
        Equal(65, moved.Left);
        Equal(15, moved.Top);
    }

    private static void ResizeEdgesEnforcesMinimum()
    {
        var origin = new PhysicalRect(100, 100, 400, 300);

        var draggedRight = ScreenGeometry.ResizeEdges(origin, 2, 60, 0, 1920, 1080, 8);
        Equal(8, draggedRight.Width);
        Equal(100, draggedRight.Left);

        var draggedTop = ScreenGeometry.ResizeEdges(origin, 4, 0, 399, 1920, 1080, 8);
        Equal(8, draggedTop.Height);
    }

    private static void ResizeEdgesClampsToContainer()
    {
        var origin = new PhysicalRect(100, 100, 400, 300);

        var draggedLeft = ScreenGeometry.ResizeEdges(origin, 1, -200, 0, 1920, 1080, 8);
        Equal(0, draggedLeft.Left);

        var draggedBottom = ScreenGeometry.ResizeEdges(origin, 8, 0, 5000, 1920, 1080, 8);
        Equal(1080, draggedBottom.Bottom);
    }

    private static void AnnotationComposeDrawsStroke()
    {
        RunSta(() =>
        {
            var source = CreateSolidBitmap(64, 64, Colors.White);

            var stroke = new AnnotationStroke
            {
                Tool = AnnotationTool.Pen,
                Color = Colors.Red,
                Thickness = 6
            };

            stroke.Points.Add(new Point(8, 8));
            stroke.Points.Add(new Point(56, 56));

            var composed = AnnotationRenderer.Compose(source, new[] { stroke });

            Equal(64, composed.PixelWidth);
            Equal(64, composed.PixelHeight);

            var middle = ReadPixel(composed, 32, 32);
            True(middle.R > 150 && middle.G < 120, "stroke should paint red over the diagonal");

            var corner = ReadPixel(composed, 2, 60);
            True(corner.R > 200 && corner.G > 200 && corner.B > 200, "untouched corner should stay white");
        });
    }

    private static void SquareConstraintKeepsSidesEqual()
    {
        var end = AnnotationGeometry.ConstrainSquare(new Point(10, 10), new Point(30, 20));
        Equal(30.0, end.X, 0.001);
        Equal(30.0, end.Y, 0.001);

        var negative = AnnotationGeometry.ConstrainSquare(new Point(10, 10), new Point(-5, 20));
        Equal(-5.0, negative.X, 0.001);
        Equal(25.0, negative.Y, 0.001);
    }

    private static void ArrowSnapLocksToAngles()
    {
        var horizontal = AnnotationGeometry.SnapToAngle(new Point(0, 0), new Point(100, 5));
        Equal(0.0, horizontal.Y, 0.001);
        True(horizontal.X > 99, "horizontal snap should preserve length");

        var vertical = AnnotationGeometry.SnapToAngle(new Point(0, 0), new Point(10, 100));
        Equal(0.0, vertical.X, 0.001);
        True(vertical.Y > 99, "vertical snap should preserve length");

        var diagonal = AnnotationGeometry.SnapToAngle(new Point(0, 0), new Point(100, 90));
        Equal(Math.Abs(diagonal.X), Math.Abs(diagonal.Y), 0.001);
    }

    private static void MosaicSnapAlignsToBlocks()
    {
        var limit = new PhysicalRect(10, 10, 100, 100);
        var snapped = MosaicRenderer.SnapToBlocks(23, 27, 58, 61, limit, 10);
        Equal(20, snapped.X);
        Equal(20, snapped.Y);
        Equal(40, snapped.Width);
        Equal(50, snapped.Height);

        var clipped = MosaicRenderer.SnapToBlocks(0, 0, 200, 200, limit, 10);
        Equal(10, clipped.X);
        Equal(10, clipped.Y);
        Equal(100, clipped.Width);
        Equal(100, clipped.Height);
    }

    private static void MosaicTileAveragesBlocks()
    {
        RunSta(() =>
        {
            const int size = 16;
            var pixels = new byte[size * size * 4];

            for (var y = 0; y < size; y++)
            {
                for (var x = 0; x < size; x++)
                {
                    var index = ((y * size) + x) * 4;
                    var red = x < size / 2;
                    pixels[index] = (byte)(red ? 0 : 255);
                    pixels[index + 1] = 0;
                    pixels[index + 2] = (byte)(red ? 255 : 0);
                    pixels[index + 3] = 255;
                }
            }

            var source = BitmapSource.Create(
                size,
                size,
                96,
                96,
                PixelFormats.Bgra32,
                null,
                pixels,
                size * 4);

            var tile = MosaicRenderer.CreateTile(source, new Int32Rect(0, 0, size, size), 8);

            if (tile is null)
            {
                throw new InvalidOperationException("tile should not be null");
            }

            Equal(2, tile.PixelWidth);
            Equal(2, tile.PixelHeight);

            var redPixel = ReadPixel(tile, 0, 0);
            True(redPixel.R > 250 && redPixel.B < 5, "left block should be red");

            var bluePixel = ReadPixel(tile, 1, 0);
            True(bluePixel.B > 250 && bluePixel.R < 5, "right block should be blue");
        });
    }

    private static void AnnotationComposeFillsShape()
    {
        RunSta(() =>
        {
            var source = CreateSolidBitmap(64, 64, Colors.White);

            var stroke = new AnnotationStroke
            {
                Tool = AnnotationTool.Rectangle,
                Color = Colors.Red,
                Thickness = 4,
                Filled = true
            };

            stroke.Points.Add(new Point(8, 8));
            stroke.Points.Add(new Point(56, 56));

            var composed = AnnotationRenderer.Compose(source, new[] { stroke });
            var middle = ReadPixel(composed, 32, 32);
            True(middle.R > 150 && middle.G < 120 && middle.B < 120, "filled rectangle interior should be red");

            var outside = ReadPixel(composed, 1, 1);
            True(outside.R > 200 && outside.G > 200 && outside.B > 200, "outside the rectangle should stay white");

            var hollow = new AnnotationStroke
            {
                Tool = AnnotationTool.Rectangle,
                Color = Colors.Red,
                Thickness = 4
            };

            hollow.Points.Add(new Point(8, 8));
            hollow.Points.Add(new Point(56, 56));

            var hollowImage = AnnotationRenderer.Compose(source, new[] { hollow });
            var hollowMiddle = ReadPixel(hollowImage, 32, 32);
            True(hollowMiddle.R > 200 && hollowMiddle.G > 200, "outline rectangle interior should stay white");
        });
    }

    private static void RunSta(Action action)
    {
        Exception? failure = null;

        var thread = new Thread(() =>
        {
            try
            {
                action();
            }
            catch (Exception ex)
            {
                failure = ex;
            }
        });

        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join();

        if (failure is not null)
        {
            throw new InvalidOperationException(failure.Message, failure);
        }
    }

    private static BitmapSource CreateSolidBitmap(int width, int height, Color color)
    {
        var stride = width * 4;
        var pixels = new byte[stride * height];

        for (var i = 0; i < pixels.Length; i += 4)
        {
            pixels[i] = color.B;
            pixels[i + 1] = color.G;
            pixels[i + 2] = color.R;
            pixels[i + 3] = 0xFF;
        }

        return BitmapSource.Create(width, height, 96, 96, PixelFormats.Bgra32, null, pixels, stride);
    }

    private static Color ReadPixel(BitmapSource source, int x, int y)
    {
        var converted = new FormatConvertedBitmap(source, PixelFormats.Bgra32, null, 0.0);
        var pixels = new byte[4];
        converted.CopyPixels(new Int32Rect(x, y, 1, 1), pixels, 4, 0);
        return Color.FromArgb(pixels[3], pixels[2], pixels[1], pixels[0]);
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
