using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Controls.Primitives;
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
            ("Mosaic tile averages blocks", MosaicTileAveragesBlocks),
            ("Mosaic composite includes prior strokes", MosaicCompositeIncludesPriorStrokes),
            ("Highlighter keeps uniform opacity at joints", HighlighterKeepsUniformOpacity),
            ("Eraser hit test follows pen path", EraserHitTestFollowsPenPath),
            ("Eraser hit test respects shape fill", EraserHitTestRespectsShapeFill),
            ("Eraser hit test covers step and mosaic", EraserHitTestCoversStepAndMosaic),
            ("Eraser hit test measures text bounds", EraserHitTestMeasuresTextBounds),
            ("Arrow both mode paints start head", ArrowBothModePaintsStartHead),
            ("Clipboard colors accept integer RGB separators and prefixes", ClipboardColorsAcceptIntegerRgb),
            ("Clipboard colors normalize fractional RGB with half rounding up", ClipboardColorsNormalizeFractionalRgb),
            ("Clipboard colors use CSS trailing alpha in all hex lengths", ClipboardColorsUseCssHexAlpha),
            ("Invalid clipboard colors remain pinnable ordinary text", InvalidClipboardColorsRemainText),
            ("Clipboard bitmap wins over text and becomes an immutable snapshot", ClipboardBitmapWinsOverTextAndFreezes),
            ("Clipboard PNG streams and bytes preserve alpha", ClipboardPngPreservesAlpha),
            ("Clipboard Unicode multiline text preserves its content", ClipboardUnicodeMultilinePreservesContent),
            ("Clipboard HTML extracts safe visible fragment text", ClipboardHtmlExtractsSafeVisibleText),
            ("CF HTML fragment offsets count UTF8 bytes including Chinese", ClipboardHtmlUsesUtf8ByteOffsets),
            ("Empty unsupported and whitespace clipboard snapshots disable pinning", EmptyClipboardSnapshotsDisablePinning),
            ("Clipboard file images are consumed only by marking a paste", ClipboardFileImagesRespectPasteSequence),
            ("Clipboard file drops mix images with safe path fallbacks", ClipboardFileDropsMixImagesAndFallbackPaths),
            ("Clipboard file batches enforce count and predecode byte budgets", ClipboardFileBatchesEnforceBudgets),
            ("Clipboard rendering bounds pixels and DPI stay consistent", ClipboardRenderingPreservesBoundsPixelsAndDpi),
            ("Clipboard file conversion settings default true and round trip false", ClipboardFileConversionSettingsRoundTrip),
            ("WPF toolbars, clipboard actions and capture hover render correctly", ToolbarVectorIconsPreserveBoundsAndColors)
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

    private static void MosaicCompositeIncludesPriorStrokes()
    {
        RunSta(() =>
        {
            var source = CreateSolidBitmap(40, 40, Colors.White);

            var pen = new AnnotationStroke
            {
                Tool = AnnotationTool.Pen,
                Color = Colors.Red,
                Thickness = 12
            };

            pen.Points.Add(new Point(5, 20));
            pen.Points.Add(new Point(35, 20));

            var tile = MosaicRenderer.CreateCompositedTile(
                source,
                new Int32Rect(0, 0, 40, 40),
                new[] { pen },
                Matrix.Identity,
                10);

            if (tile is null)
            {
                throw new InvalidOperationException("composited tile should not be null");
            }

            Equal(4, tile.PixelWidth);
            Equal(4, tile.PixelHeight);

            var middle = ReadPixel(tile, 2, 2);
            True(
                middle.R > 200 && middle.G < 150,
                "mosaic block should include the red stroke below it");

            var top = ReadPixel(tile, 2, 0);
            True(
                top.R > 200 && top.G > 200,
                "block without strokes should stay white");
        });
    }

    private static void HighlighterKeepsUniformOpacity()
    {
        RunSta(() =>
        {
            var source = CreateSolidBitmap(800, 200, Colors.White);
            var stroke = new AnnotationStroke
            {
                Tool = AnnotationTool.Highlighter,
                Color = Colors.Red,
                Thickness = 36
            };

            stroke.Points.Add(new Point(30, 100));
            stroke.Points.Add(new Point(200, 100));
            stroke.Points.Add(new Point(400, 100));
            stroke.Points.Add(new Point(600, 100));
            stroke.Points.Add(new Point(770, 100));

            var composed = AnnotationRenderer.Compose(source, new[] { stroke });

            var segment = ReadPixel(composed, 100, 100);
            var joint = ReadPixel(composed, 200, 100);
            var middle = ReadPixel(composed, 300, 100);

            True(
                Math.Abs(segment.R - joint.R) <= 4 &&
                Math.Abs(segment.G - joint.G) <= 4 &&
                Math.Abs(segment.B - joint.B) <= 4,
                "joint pixel should not become darker than the segment");

            True(
                Math.Abs(segment.R - middle.R) <= 4 &&
                Math.Abs(segment.G - middle.G) <= 4 &&
                Math.Abs(segment.B - middle.B) <= 4,
                "whole highlighter stroke should keep one opacity");

            True(
                segment.R > 240 && segment.G is > 100 and < 200,
                "highlighter should be semi-transparent over white");
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

    private static void EraserHitTestFollowsPenPath()
    {
        var pen = new AnnotationStroke
        {
            Tool = AnnotationTool.Pen,
            Thickness = 4
        };

        pen.Points.Add(new Point(10, 10));
        pen.Points.Add(new Point(50, 50));

        True(AnnotationHitTester.HitTest(pen, new Point(30, 30), 2), "point on the stroke should hit");
        True(!AnnotationHitTester.HitTest(pen, new Point(30, 45), 2), "point away from the stroke should miss");
        True(AnnotationHitTester.HitTest(pen, new Point(10, 10), 1), "stroke start should hit");
    }

    private static void EraserHitTestRespectsShapeFill()
    {
        var outline = new AnnotationStroke
        {
            Tool = AnnotationTool.Rectangle,
            Thickness = 2
        };

        outline.Points.Add(new Point(10, 10));
        outline.Points.Add(new Point(90, 70));

        True(AnnotationHitTester.HitTest(outline, new Point(50, 10), 1), "border should hit");
        True(!AnnotationHitTester.HitTest(outline, new Point(50, 40), 1), "hollow interior should miss");

        var filled = new AnnotationStroke
        {
            Tool = AnnotationTool.Rectangle,
            Thickness = 2,
            Filled = true
        };

        filled.Points.Add(new Point(10, 10));
        filled.Points.Add(new Point(90, 70));

        True(AnnotationHitTester.HitTest(filled, new Point(50, 40), 1), "filled interior should hit");
        True(!AnnotationHitTester.HitTest(filled, new Point(120, 40), 1), "outside should miss");
    }

    private static void EraserHitTestCoversStepAndMosaic()
    {
        var step = new AnnotationStroke { Tool = AnnotationTool.Step };
        step.Points.Add(new Point(50, 50));

        True(AnnotationHitTester.HitTest(step, new Point(58, 50), 2), "step edge should hit");
        True(!AnnotationHitTester.HitTest(step, new Point(70, 50), 2), "far step point should miss");

        var mosaic = new AnnotationStroke { Tool = AnnotationTool.Mosaic };
        mosaic.Points.Add(new Point(10, 10));
        mosaic.Points.Add(new Point(50, 50));

        True(AnnotationHitTester.HitTest(mosaic, new Point(54, 54), 6), "mosaic edge should hit");
        True(!AnnotationHitTester.HitTest(mosaic, new Point(20, 80), 6), "outside mosaic should miss");
    }

    private static void EraserHitTestMeasuresTextBounds()
    {
        RunSta(() =>
        {
            var text = new AnnotationStroke
            {
                Tool = AnnotationTool.Text,
                Text = "光圈 HaloCap",
                TextSize = 18
            };

            text.Points.Add(new Point(20, 20));

            True(AnnotationHitTester.HitTest(text, new Point(30, 28), 1), "text anchor should hit");
            True(!AnnotationHitTester.HitTest(text, new Point(30, 80), 1), "below text should miss");
        });
    }

    private static void ArrowBothModePaintsStartHead()
    {
        RunSta(() =>
        {
            var source = CreateSolidBitmap(160, 80, Colors.White);

            AnnotationStroke Create(ArrowHeadMode mode)
            {
                var stroke = new AnnotationStroke
                {
                    Tool = AnnotationTool.Arrow,
                    Color = Colors.Red,
                    Thickness = 8,
                    ArrowHead = mode
                };

                stroke.Points.Add(new Point(30, 40));
                stroke.Points.Add(new Point(130, 40));
                return stroke;
            }

            var line = AnnotationRenderer.Compose(source, new[] { Create(ArrowHeadMode.None) });
            var both = AnnotationRenderer.Compose(source, new[] { Create(ArrowHeadMode.Both) });

            var lineStart = CountInk(line, 30, 20, 28, 40);
            var bothStart = CountInk(both, 30, 20, 28, 40);
            True(bothStart > lineStart + 15, "both mode should paint a head at the start");

            var lineEnd = CountInk(line, 104, 20, 34, 40);
            var bothEnd = CountInk(both, 104, 20, 34, 40);
            True(bothEnd > lineEnd + 15, "both mode should keep the head at the end");
        });
    }

    private static int CountInk(BitmapSource bitmap, int x, int y, int width, int height)
    {
        var count = 0;

        for (var row = y; row < y + height; row++)
        {
            for (var column = x; column < x + width; column++)
            {
                var pixel = ReadPixel(bitmap, column, row);
                if (pixel.R > 180 && pixel.G < 150 && pixel.B < 150)
                {
                    count++;
                }
            }
        }

        return count;
    }

    private static void ClipboardColorsAcceptIntegerRgb()
    {
        for (var channel = 0; channel <= 255; channel++)
        {
            var text = $"{channel} {255 - channel} {channel}";
            True(ClipboardPinService.TryParseColor(text, out var color), $"integer RGB should parse: {text}");
            True(color == Color.FromRgb((byte)channel, (byte)(255 - channel), (byte)channel),
                $"integer channels should remain in the 0..255 range: {text}");
        }

        foreach (var text in new[]
        {
            "12 34 255", "12\t34\t255", "12,34,255", "12, 34, 255", "12，34，255",
            "RGB 12 34 255", "rgb(12,34,255)", "RgB: 12，34，255", " RGB： (12, 34, 255) "
        })
        {
            True(ClipboardPinService.TryParseColor(text, out var color), $"RGB separator or prefix should parse: {text}");
            True(color == Color.FromRgb(12, 34, 255), $"RGB separator must not change channels: {text}");
        }
        True(ClipboardPinService.TryParseColor("0 1 1", out var smallIntegers), "small integers should parse");
        True(smallIntegers == Color.FromRgb(0, 1, 1), "integer-only RGB must not be normalized as fractions");
    }

    private static void ClipboardColorsNormalizeFractionalRgb()
    {
        foreach (var (text, expected) in new[]
        {
            ("0.0 0.5 1.0", Color.FromRgb(0, 128, 255)),
            ("0, .5, 1", Color.FromRgb(0, 128, 255)),
            ("RGB(0，0.5，1)", Color.FromRgb(0, 128, 255)),
            ("rgb: 1 0 .5", Color.FromRgb(255, 0, 128)),
            (".25 .75 0", Color.FromRgb(64, 191, 0)),
            ("1. 0 1", Color.FromRgb(255, 0, 255)),
            ("0.0 0 0", Colors.Black)
        })
        {
            True(ClipboardPinService.TryParseColor(text, out var color), $"fractional RGB should parse: {text}");
            True(color == expected, $"fractional channels should round half away from zero: {text}");
        }
    }

    private static void ClipboardColorsUseCssHexAlpha()
    {
        foreach (var (text, expected) in new[]
        {
            ("#123", Color.FromArgb(255, 0x11, 0x22, 0x33)),
            ("#1234", Color.FromArgb(0x44, 0x11, 0x22, 0x33)),
            ("#12aBcD", Color.FromArgb(255, 0x12, 0xAB, 0xCD)),
            (" #12aBcD80 ", Color.FromArgb(0x80, 0x12, 0xAB, 0xCD)),
            ("#F000", Color.FromArgb(0, 255, 0, 0)),
            ("#FFFFFF00", Color.FromArgb(0, 255, 255, 255))
        })
        {
            True(ClipboardPinService.TryParseColor(text, out var color), $"CSS hex should parse: {text}");
            True(color == expected, $"CSS hex alpha belongs at the end, not in WPF ARGB order: {text}");
        }

        RunSta(() =>
        {
            var pins = new ClipboardPinService().CreatePins(new ClipboardSnapshot(17, null, "#1234", Array.Empty<string>()), true, 1);
            Equal(1, pins.Count);
            True(pins[0].Kind == ClipboardPinKind.Color, "valid CSS color should produce a color card");
            Equal("#1234", pins[0].Text!);
            Equal(17, (int)pins[0].Sequence);
        });
    }

    private static void InvalidClipboardColorsRemainText()
    {
        RunSta(() =>
        {
            var service = new ClipboardPinService();
            foreach (var text in new[]
            {
                "#12", "#12345", "#1234567", "#123456789", "#GG0000", "red", "普通文字",
                "256 0 0", "0 256 0", "0 0 256", "-1 0 0", "0.5 1.01 0", "0.5 128 0",
                "NaN 0 0", "Infinity 0 0", "1e2 0 0", "RGB(1,2)", "RGB(1,2,3", "1,2,3,4", "rgba(1,2,3,0.5)"
            })
            {
                True(!ClipboardPinService.TryParseColor(text, out _), $"invalid or out-of-range color must be rejected: {text}");
                var data = new DataObject();
                data.SetData(DataFormats.UnicodeText, text);
                var snapshot = ClipboardPinService.ReadDataObject(data, 18);
                True(snapshot.HasContent, "non-color text must still enable pinning");
                var pins = service.CreatePins(snapshot, true, 1);
                Equal(1, pins.Count);
                True(pins[0].Kind == ClipboardPinKind.Text && pins[0].Image.IsFrozen,
                    $"invalid color should render as immutable ordinary text: {text}");
                Equal(text, pins[0].Text!);
            }
        });
    }

    private static void ClipboardBitmapWinsOverTextAndFreezes()
    {
        RunSta(() =>
        {
            var original = new WriteableBitmap(CreateSolidBitmap(4, 3, Colors.Red));
            var data = new DataObject();
            data.SetData(DataFormats.Bitmap, original);
            data.SetData(DataFormats.UnicodeText, "文本不应覆盖位图");
            data.SetData(DataFormats.Html, "<p>HTML must not win</p>");
            var snapshot = ClipboardPinService.ReadDataObject(data, 19);
            True(snapshot.HasContent && snapshot.Image is { IsFrozen: true }, "bitmap snapshot should be frozen and available");
            True(snapshot.Text is null && snapshot.FilePaths.Length == 0, "bitmap must take priority over both text formats");
            True(!original.IsFrozen && !ReferenceEquals(original, snapshot.Image), "freezing a snapshot must not freeze the caller bitmap");
            original.WritePixels(new Int32Rect(0, 0, 1, 1), new byte[] { 255, 0, 0, 255 }, 4, 0);
            True(ReadPixel(snapshot.Image!, 0, 0) == Colors.Red, "snapshot must not change when the source bitmap changes");
            var pins = new ClipboardPinService().CreatePins(snapshot, true, 1.5);
            Equal(1, pins.Count);
            True(pins[0].Kind == ClipboardPinKind.Image && ReferenceEquals(snapshot.Image, pins[0].Image),
                "bitmap pin should reuse the frozen snapshot without text rendering or DPI resizing");
            Equal(4, pins[0].Image.PixelWidth);
            Equal(3, pins[0].Image.PixelHeight);
            Equal(19, (int)pins[0].Sequence);
            True(pins[0].Text is null, "bitmap pin should not retain lower-priority text");
        });
    }

    private static void ClipboardPngPreservesAlpha()
    {
        RunSta(() =>
        {
            var source = BitmapSource.Create(2, 1, 96, 96, PixelFormats.Bgra32, null,
                new byte[] { 0, 0, 0, 0, 40, 80, 200, 128 }, 8);
            var encoder = new PngBitmapEncoder();
            encoder.Frames.Add(BitmapFrame.Create(source));
            ClipboardSnapshot snapshot;
            byte[] bytes;
            using (var stream = new MemoryStream())
            {
                encoder.Save(stream);
                bytes = stream.ToArray();
                var data = new DataObject();
                data.SetData("PNG", stream);
                data.SetData(DataFormats.Bitmap, CreateSolidBitmap(2, 1, Colors.Blue));
                data.SetData(DataFormats.UnicodeText, "PNG must win");
                snapshot = ClipboardPinService.ReadDataObject(data, 20);
            }

            True(snapshot.Image is { IsFrozen: true } && snapshot.Text is null, "PNG stream should win and survive stream disposal");
            Equal(0, ReadPixel(snapshot.Image!, 0, 0).A);
            True(ReadPixel(snapshot.Image!, 1, 0) == Color.FromArgb(128, 200, 80, 40), "PNG must retain semi-transparent channel values");
            var byteData = new DataObject();
            byteData.SetData("PNG", bytes);
            var byteSnapshot = ClipboardPinService.ReadDataObject(byteData, 21);
            True(byteSnapshot.HasContent && byteSnapshot.Image is { IsFrozen: true }, "PNG byte arrays should also decode");
            True(ReadPixel(byteSnapshot.Image!, 1, 0) == Color.FromArgb(128, 200, 80, 40), "PNG bytes must retain alpha");
            var pins = new ClipboardPinService().CreatePins(snapshot, true, 1);
            True(pins.Count == 1 && pins[0].Kind == ClipboardPinKind.Image, "PNG should produce an image pin");
            Equal(20, (int)pins[0].Sequence);
        });
    }

    private static void ClipboardUnicodeMultilinePreservesContent()
    {
        RunSta(() =>
        {
            const string text = "  第一行：光圈\r\n第二行 café \uD840\uDC00\n\t第三行\n末行保留空格  ";
            var data = new DataObject();
            data.SetData(DataFormats.UnicodeText, text + "\0\0");
            data.SetData(DataFormats.Text, "lossy ANSI fallback");
            var snapshot = ClipboardPinService.ReadDataObject(data, 22);
            Equal(text, snapshot.Text!);
            True(snapshot.HasContent && snapshot.Image is null, "Unicode multiline text should enable a text pin");
            var pins = new ClipboardPinService().CreatePins(snapshot, true, 1.5);
            Equal(1, pins.Count);
            True(pins[0].Kind == ClipboardPinKind.Text, "Unicode text should not become a color");
            Equal(text, pins[0].Text!);
            Equal(22, (int)pins[0].Sequence);
            True(pins[0].Image.IsFrozen, "rendered text should be immutable");
        });
    }

    private static void ClipboardHtmlExtractsSafeVisibleText()
    {
        RunSta(() =>
        {
            const string fragment = "<head><title>HIDDEN HEAD</title><link href='https://invalid.invalid/style.css'></head>" +
                "<script>HIDDEN SCRIPT</script><style>HIDDEN STYLE</style><iframe src='https://invalid.invalid/'>HIDDEN FRAME</iframe>" +
                "<object data='file:///not-a-resource'>HIDDEN OBJECT</object><!--HIDDEN COMMENT-->" +
                "<div>可见 &amp; &#x4E2D;&#25991;<br/>第二行 &lt;文字&gt;</div>" +
                "<img src='https://invalid.invalid/image.png' alt='HIDDEN ALT'><img src='file:///not-a-resource'>" +
                "<table><tr><td>第三行</td><td>42</td></tr></table>";
            const string expected = "可见 & 中文\n第二行 <文字>\n第三行\t42";
            Equal(expected, ClipboardPinService.HtmlToText(fragment));
            var html = "Version:1.0\r\nStartHTML:0000000000\r\n<html><body>OUTSIDE BEFORE<!--StartFragment-->" +
                fragment + "<!--EndFragment-->OUTSIDE AFTER</body></html>";
            Equal(expected, ClipboardPinService.HtmlToText(html));
            var data = new DataObject();
            data.SetData(DataFormats.Html, html);
            data.SetData(DataFormats.UnicodeText, "lower-priority plain text");
            var snapshot = ClipboardPinService.ReadDataObject(data, 23);
            Equal(expected, snapshot.Text!);
            var pins = new ClipboardPinService().CreatePins(snapshot, true, 1);
            True(pins.Count == 1 && pins[0].Kind == ClipboardPinKind.Text, "HTML should pin visible text, never external resources");
            Equal(expected, pins[0].Text!);
        });
    }

    private static void ClipboardHtmlUsesUtf8ByteOffsets()
    {
        RunSta(() =>
        {
            const string headerTemplate = "Version:1.0\r\nStartHTML:{0:D10}\r\nEndHTML:{1:D10}\r\nStartFragment:{2:D10}\r\nEndFragment:{3:D10}\r\n";
            const string prefix = "<html><head><title>标题</title></head><body>前置中文";
            const string fragment = "<div>片段中文 &amp; café<br>第二行</div>";
            const string suffix = "尾部不可见</body></html>";
            var headerLength = Encoding.UTF8.GetByteCount(string.Format(CultureInfo.InvariantCulture, headerTemplate, 0, 0, 0, 0));
            var startFragment = headerLength + Encoding.UTF8.GetByteCount(prefix);
            var endFragment = startFragment + Encoding.UTF8.GetByteCount(fragment);
            var endHtml = endFragment + Encoding.UTF8.GetByteCount(suffix);
            var header = string.Format(CultureInfo.InvariantCulture, headerTemplate, headerLength, endHtml, startFragment, endFragment);
            var html = header + prefix + fragment + suffix;
            True(startFragment > header.Length + prefix.Length, "Chinese prefix must make UTF8 byte and character offsets different");
            Equal(endHtml, Encoding.UTF8.GetByteCount(html));
            Equal("片段中文 & café\n第二行", ClipboardPinService.HtmlToText(html));
            var data = new DataObject();
            data.SetData(DataFormats.Html, html);
            var snapshot = ClipboardPinService.ReadDataObject(data, 24);
            Equal("片段中文 & café\n第二行", snapshot.Text!);
            var pins = new ClipboardPinService().CreatePins(snapshot, true, 1);
            Equal(1, pins.Count);
            Equal(snapshot.Text!, pins[0].Text!);
        });
    }

    private static void EmptyClipboardSnapshotsDisablePinning()
    {
        RunSta(() =>
        {
            var unsupported = new DataObject();
            unsupported.SetData("HaloCap.Tests.Unsupported", new byte[] { 1, 2, 3 });
            var emptyFiles = new DataObject();
            emptyFiles.SetData(DataFormats.FileDrop, new[] { "", " ", "\t" });
            var invisibleHtml = new DataObject();
            invisibleHtml.SetData(DataFormats.Html, "<head>hidden</head><script>hidden</script><style>hidden</style><img src='https://invalid.invalid/'>");
            var cases = new List<DataObject> { new(), unsupported, emptyFiles, invisibleHtml };
            foreach (var text in new[] { "", " \t\r\n", "\0\0" })
            {
                var data = new DataObject();
                data.SetData(DataFormats.UnicodeText, text);
                cases.Add(data);
            }
            var service = new ClipboardPinService();
            foreach (var data in cases)
            {
                var snapshot = ClipboardPinService.ReadDataObject(data, 25);
                True(!snapshot.HasContent, "empty, unsupported, whitespace or invisible HTML should disable pinning");
                Equal(25, (int)snapshot.Sequence);
                Equal(0, snapshot.FilePaths.Length);
                Equal(0, service.CreatePins(snapshot, true, 1).Count);
            }

            invisibleHtml.SetData(DataFormats.UnicodeText, "HTML 不可见时回退文字");
            Equal("HTML 不可见时回退文字", ClipboardPinService.ReadDataObject(invisibleHtml, 26).Text!);
            var ansi = new DataObject();
            ansi.SetData(DataFormats.Text, "plain text fallback");
            Equal("plain text fallback", ClipboardPinService.ReadDataObject(ansi, 27).Text!);
        });
    }

    private static void ClipboardFileImagesRespectPasteSequence()
    {
        RunSta(() => WithTemporaryClipboardFiles(directory =>
        {
            var path = Path.Combine(directory, "image.png");
            var data = new DataObject();
            data.SetData(DataFormats.FileDrop, new[] { path });
            var snapshot = ClipboardPinService.ReadDataObject(data, 28);
            var service = new ClipboardPinService();
            var first = service.CreatePins(snapshot, true, 1);
            Equal(1, first.Count);
            True(first[0].Kind == ClipboardPinKind.FileImage && first[0].Image.IsFrozen, "first image file pin should decode a frozen image");
            Equal(path, first[0].Text!);
            Equal(28, (int)first[0].Sequence);
            True(ReadPixel(first[0].Image, 1, 1) == Colors.Red, "first image file pin should contain the actual PNG pixels");
            True(service.CreatePins(snapshot, true, 1)[0].Kind == ClipboardPinKind.FileImage,
                "creating a preview must not consume the file-image paste sequence");
            var pathOnly = service.CreatePins(snapshot, false, 1);
            True(pathOnly[0].Kind == ClipboardPinKind.FilePath, "disabled conversion should return a path");
            Equal(path, pathOnly[0].Text!);
            service.MarkPasted(pathOnly[0]);
            foreach (var kind in new[] { ClipboardPinKind.Image, ClipboardPinKind.Text, ClipboardPinKind.Color })
            {
                service.MarkPasted(new ClipboardPinContent(first[0].Image, kind, first[0].Text, snapshot.Sequence));
            }
            True(service.CreatePins(snapshot, true, 1)[0].Kind == ClipboardPinKind.FileImage,
                "marking any non-FileImage kind must not consume image-file conversion");
            service.MarkPasted(first[0]);
            var repeated = service.CreatePins(snapshot, true, 1.5);
            Equal(1, repeated.Count);
            True(repeated[0].Kind == ClipboardPinKind.FilePath, "same sequence should become a path only after a real image paste");
            Equal(path, repeated[0].Text!);
            var next = ClipboardPinService.ReadDataObject(data, 29);
            True(service.CreatePins(next, true, 1)[0].Kind == ClipboardPinKind.FileImage,
                "a new clipboard sequence should make the same file eligible for image conversion again");
            True(service.CreatePins(next, false, 1)[0].Kind == ClipboardPinKind.FilePath,
                "conversion false should force paths even on a fresh sequence");
        }));
    }

    private static void ClipboardFileDropsMixImagesAndFallbackPaths()
    {
        RunSta(() => WithTemporaryClipboardFiles(directory =>
        {
            var firstImage = Path.Combine(directory, "image.png");
            var secondImage = Path.Combine(directory, "second.png");
            var textFile = Path.Combine(directory, "notes.txt");
            var badImage = Path.Combine(directory, "broken.png");
            var missingImage = Path.Combine(directory, "missing.png");
            const string unc = @"\\halocap-do-not-contact.invalid\share\image.png";
            const string relative = "relative-image.png";
            var fallbackPaths = new[] { textFile, badImage, missingImage, unc, relative };
            var filePaths = new[] { firstImage, textFile, secondImage, badImage, missingImage, unc, relative };
            var data = new DataObject();
            data.SetData(DataFormats.FileDrop, new[] { "", firstImage, " ", textFile, secondImage, badImage, missingImage, unc, relative });
            data.SetData(DataFormats.Bitmap, CreateSolidBitmap(1, 1, Colors.Green));
            data.SetData(DataFormats.UnicodeText, "file drop should win");
            var snapshot = ClipboardPinService.ReadDataObject(data, 30);
            True(snapshot.HasContent && snapshot.Image is null && snapshot.Text is null, "file drop must take priority over bitmap and text");
            Equal(string.Join("|", filePaths), string.Join("|", snapshot.FilePaths));
            var service = new ClipboardPinService();
            var pins = service.CreatePins(snapshot, true, 1);
            Equal(3, pins.Count);
            True(pins[0].Kind == ClipboardPinKind.FileImage && pins[1].Kind == ClipboardPinKind.FileImage,
                "multiple valid images should each produce their own image pin");
            Equal(firstImage, pins[0].Text!);
            Equal(secondImage, pins[1].Text!);
            True(ReadPixel(pins[1].Image, 1, 1) == Colors.Blue, "second image should retain its own pixels");
            True(pins[2].Kind == ClipboardPinKind.FilePath && pins[2].Image.IsFrozen,
                "non-image, broken, missing, UNC and relative paths should fall back without network access");
            Equal(string.Join(Environment.NewLine, fallbackPaths), pins[2].Text!);
            foreach (var pin in pins)
            {
                Equal(30, (int)pin.Sequence);
            }
            var fallbackSnapshot = new ClipboardSnapshot(31, null, null, fallbackPaths);
            var fallback = service.CreatePins(fallbackSnapshot, true, 1);
            Equal(1, fallback.Count);
            True(fallback[0].Kind == ClipboardPinKind.FilePath, "all unusable images should still produce a path card");
            Equal(string.Join(Environment.NewLine, fallbackPaths), fallback[0].Text!);
            var disabled = service.CreatePins(snapshot, false, 1);
            Equal(1, disabled.Count);
            Equal(string.Join(Environment.NewLine, filePaths), disabled[0].Text!);
            service.MarkPasted(pins[1]);
            var repeated = service.CreatePins(snapshot, true, 1);
            Equal(1, repeated.Count);
            True(repeated[0].Kind == ClipboardPinKind.FilePath, "pasting one file image should switch the whole matching sequence to paths");
            Equal(string.Join(Environment.NewLine, filePaths), repeated[0].Text!);
        }));
    }

    private static void ClipboardFileBatchesEnforceBudgets()
    {
        RunSta(() => WithTemporaryClipboardFiles(directory =>
        {
            var path = Path.Combine(directory, "image.png");
            var paths = new string[17];
            Array.Fill(paths, path);
            var pins = new ClipboardPinService().CreatePins(new ClipboardSnapshot(33, null, null, paths), true, 1);
            Equal(17, pins.Count);
            for (var index = 0; index < 16; index++)
            {
                True(pins[index].Kind == ClipboardPinKind.FileImage, "eligible batch images should decode");
            }
            True(pins[16].Kind == ClipboardPinKind.FilePath, "the image beyond the count limit should become a path");
            Equal(path, pins[16].Text!);

            var decode = typeof(ClipboardPinService).GetMethod("DecodeImage", BindingFlags.Static | BindingFlags.NonPublic)!;
            using var stream = File.OpenRead(path);
            True(decode.Invoke(null, new object[] { stream, 23L }) is null, "metadata must reject a 24-byte image before decoding when only 23 bytes remain");
            var exact = (BitmapSource)decode.Invoke(null, new object[] { stream, 24L })!;
            True(exact.IsFrozen && ReadPixel(exact, 1, 1) == Colors.Red, "an image fitting the exact byte budget should decode");
        }));
    }

    private static void WithTemporaryClipboardFiles(Action<string> action)
    {
        var directory = Path.Combine(Path.GetTempPath(), "HaloCap.ClipboardTests." + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(directory);
        try
        {
            foreach (var (name, color) in new[] { ("image.png", Colors.Red), ("second.png", Colors.Blue) })
            {
                var encoder = new PngBitmapEncoder();
                encoder.Frames.Add(BitmapFrame.Create(CreateSolidBitmap(3, 2, color)));
                using var stream = new FileStream(Path.Combine(directory, name), FileMode.CreateNew, FileAccess.Write);
                encoder.Save(stream);
            }
            File.WriteAllText(Path.Combine(directory, "notes.txt"), "ordinary text, not an image", Encoding.UTF8);
            File.WriteAllBytes(Path.Combine(directory, "broken.png"), new byte[] { 137, 80, 78, 71, 13, 10, 26, 10, 0 });
            action(directory);
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    private static void ClipboardRenderingPreservesBoundsPixelsAndDpi()
    {
        RunSta(() =>
        {
            foreach (var scale in new[] { 1.0, 1.5, 2.0 })
            {
                foreach (var text in new[] { "x", new string('长', 40_000), new string('\n', 300).Replace("\n", "行\n") + "末行" })
                {
                    var image = ClipboardPinService.RenderText(text, scale);
                    True(image.IsFrozen, "rendered text should be frozen");
                    True(image.PixelWidth >= 160 * scale && image.PixelWidth <= 672 * scale,
                        "long text must remain within the minimum and maximum card widths");
                    True(image.PixelHeight >= 56 * scale && image.PixelHeight <= 2832 * scale,
                        "long multiline text must remain within bounded card heights");
                    Equal(96 * scale, image.DpiX, 0.001);
                    Equal(96 * scale, image.DpiY, 0.001);
                    True(ReadPixel(image, 0, 0) == Colors.White, "text cards should have an opaque white margin");
                    if (text == "x")
                    {
                        Equal((int)(160 * scale), image.PixelWidth);
                        Equal((int)(56 * scale), image.PixelHeight);
                        var hasTextInk = false;
                        for (var y = (int)(16 * scale); y < (int)(40 * scale); y++)
                        {
                            for (var x = (int)(16 * scale); x < (int)(40 * scale); x++)
                            {
                                var pixel = ReadPixel(image, x, y);
                                hasTextInk |= pixel.R < 150 && pixel.G < 150 && pixel.B < 150;
                            }
                        }
                        True(hasTextInk, "text cards must actually draw glyphs inside their padded bounds");
                    }
                }
                var color = Color.FromRgb(23, 101, 207);
                var swatch = ClipboardPinService.RenderColor(color, scale);
                True(swatch.IsFrozen, "color cards should be frozen");
                Equal((int)(280 * scale), swatch.PixelWidth);
                Equal((int)(190 * scale), swatch.PixelHeight);
                Equal(96 * scale, swatch.DpiX, 0.001);
                Equal(96 * scale, swatch.DpiY, 0.001);
                True(ReadPixel(swatch, (int)(6 * scale), (int)(6 * scale)) == color, "opaque color swatch should contain exact RGB pixels");
                True(ReadPixel(swatch, (int)(279 * scale), (int)(189 * scale)) == Colors.White, "color card label area should remain white");
                var transparent = ClipboardPinService.RenderColor(Color.FromArgb(0, 255, 0, 0), scale);
                Equal(0, ReadPixel(transparent, (int)(6 * scale), (int)(6 * scale)).A);
                True(ReadPixel(transparent, (int)(279 * scale), (int)(189 * scale)) == Colors.White,
                    "color labels should stay readable while the swatch remains transparent");
                var halfRed = ClipboardPinService.RenderColor(Color.FromArgb(128, 255, 0, 0), scale);
                var alphaPixel = ReadPixel(halfRed, (int)(18 * scale), (int)(6 * scale));
                True(alphaPixel.A == 128 && alphaPixel.R == 255 && alphaPixel.G == 0 && alphaPixel.B == 0,
                    "CSS alpha should survive rendering without an opaque checkerboard or background");
            }
            var longText = new string('字', 40_000);
            var pins = new ClipboardPinService().CreatePins(new ClipboardSnapshot(32, null, longText, Array.Empty<string>()), true, 1);
            Equal(longText, pins[0].Text!);
        });
    }

    private static void ClipboardFileConversionSettingsRoundTrip()
    {
        // Read only the serializer options; never invoke SettingsStore.Load or Save.
        var options = (JsonSerializerOptions)typeof(SettingsStore)
            .GetField("JsonOptions", BindingFlags.Static | BindingFlags.NonPublic)!.GetValue(null)!;
        True(new AppSettings().ConvertFilePathsToImages, "new settings should default image-file conversion to true");
        var legacy = JsonSerializer.Deserialize<AppSettings>("{}", options);
        True(legacy is { ConvertFilePathsToImages: true }, "legacy JSON without the new option should retain default true");
        var disabled = JsonSerializer.Deserialize<AppSettings>("{\"convertFilePathsToImages\":false}", options);
        True(disabled is { ConvertFilePathsToImages: false }, "explicit false must not be replaced by the default");
        var json = JsonSerializer.Serialize(disabled, options);
        using var document = JsonDocument.Parse(json);
        True(!document.RootElement.GetProperty("convertFilePathsToImages").GetBoolean(),
            "serialized JSON should explicitly preserve false using the application's camelCase format");
        True(JsonSerializer.Deserialize<AppSettings>(json, options) is { ConvertFilePathsToImages: false },
            "false should survive an in-memory JSON round trip without touching user settings");
    }

    private static void ToolbarVectorIconsPreserveBoundsAndColors()
    {
        RunSta(() =>
        {
            var app = new HaloCap.App();
            app.InitializeComponent();
            CaptureOverlayPreviewKeepsMouseSurface();
            var names = new[]
            {
                "Select", "Shape", "Arrow", "Pen", "Highlighter", "Eraser", "Text", "Mosaic", "Step",
                "Undo", "Redo", "Clear", "Save", "Copy", "Pin", "Ocr", "Close",
                "Rectangle", "Ellipse", "Outline", "Fill", "ArrowEnd", "ArrowBoth", "ArrowLine",
                "Bold", "Italic", "Decrease", "Increase", "Record", "Done"
            };

            foreach (var name in names)
            {
                var geometry = (Geometry)app.Resources[$"Toolbar{name}Geometry"];
                var bounds = geometry.Bounds;
                True(!bounds.IsEmpty && geometry.GetArea() > 0, $"{name} should have visible geometry");
                True(bounds.Left >= 0 && bounds.Top >= 0 && bounds.Right <= 200 && bounds.Bottom <= 200,
                    $"{name} should stay inside the source canvas");
                True(!geometry.FillContains(new Point(0, 0)), $"{name} should keep transparent padding");
            }

            var outline = (Geometry)app.Resources["ToolbarOutlineGeometry"];
            var fill = (Geometry)app.Resources["ToolbarFillGeometry"];
            True(!outline.FillContains(new Point(100, 100)), "outline should keep its center hollow");
            True(fill.FillContains(new Point(100, 100)), "fill should paint its center");

            var path = new System.Windows.Shapes.Path
            {
                Style = (Style)app.Resources["ToolbarIconStyle"],
                Data = (Geometry)app.Resources["ToolbarPenGeometry"]
            };
            var button = new Button
            {
                Foreground = Brushes.Red,
                Content = new Viewbox { Width = 20, Height = 20, Child = path }
            };
            button.Measure(new Size(40, 40));
            button.Arrange(new Rect(0, 0, 40, 40));
            button.UpdateLayout();
            True(((SolidColorBrush)path.Fill).Color == Colors.Red, "icon should use the button foreground");
            button.Foreground = Brushes.White;
            True(((SolidColorBrush)path.Fill).Color == Colors.White, "icon should update with the selected foreground");

            var alphaPixels = new byte[40 * 40 * 4];
            for (var index = 0; index < alphaPixels.Length; index += 4)
            {
                alphaPixels[index + 2] = 255;
                alphaPixels[index + 3] = 128;
            }
            var alphaSource = BitmapSource.Create(40, 40, 96, 96, PixelFormats.Bgra32, null, alphaPixels, 160);
            var alphaPin = new PinWindow(alphaSource, new PhysicalRect(0, 0, 40, 40), 1, ScreenshotImageFormat.Png);
            True(alphaPin.AllowsTransparency && ((SolidColorBrush)alphaPin.Background).Color.A == 0,
                "pin windows should preserve per-pixel transparency instead of painting an opaque background");
            var alphaRoot = (Grid)alphaPin.FindName("Root");
            True(((SolidColorBrush)alphaRoot.Background).Color.A == 0, "the pin root should not flatten image alpha");
            alphaRoot.Measure(new Size(40, 40));
            alphaRoot.Arrange(new Rect(0, 0, 40, 40));
            alphaRoot.UpdateLayout();
            var alphaPreview = new RenderTargetBitmap(40, 40, 96, 96, PixelFormats.Pbgra32);
            alphaPreview.Render(alphaRoot);
            Equal(128, ReadPixel(alphaPreview, 20, 20).A);
            alphaPin.Close();

            var pin = new PinWindow(CreateSolidBitmap(1200, 800, Colors.White), new PhysicalRect(0, 0, 1200, 800), 1, ScreenshotImageFormat.Png);
            var root = (FrameworkElement)pin.FindName("Root");
            root.Measure(new Size(1100, 600));
            root.Arrange(new Rect(0, 0, 1100, 600));
            typeof(PinWindow).GetMethod("SetAnnotationMode", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(pin, new object[] { true });
            ((Button)pin.FindName("ToolShapeButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            root.UpdateLayout();
            var mainBar = (Border)pin.FindName("AnnotationBar");
            var optionBar = (Border)pin.FindName("PinOptionBar");
            var host = (FrameworkElement)pin.FindName("AnnotationBarHost");
            var mainOrigin = mainBar.TransformToAncestor(host).Transform(new Point());
            var optionOrigin = optionBar.TransformToAncestor(host).Transform(new Point());
            True(optionOrigin.Y >= mainOrigin.Y + mainBar.ActualHeight + 5, "pin options should expand on a separate row");
            var tools = (StackPanel)mainBar.Child;
            var done = (Button)tools.Children[tools.Children.Count - 1];
            True(done.Content is Viewbox, "pin completion should contain only the checkmark icon");
            foreach (var name in new[] { "ToolShapeButton", "ToolArrowButton", "ToolPenButton", "ToolHighlighterButton", "ToolEraserButton", "ToolTextButton", "ToolMosaicButton", "ToolStepButton" })
            {
                ((Button)pin.FindName(name)).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                root.UpdateLayout();
                True(optionBar.ActualWidth <= mainBar.ActualWidth + 0.1, $"{name} options should not exceed the main toolbar width");
            }
            ((Button)pin.FindName("ToolMosaicButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            True(((FrameworkElement)pin.FindName("PinColorGroup")).Visibility == Visibility.Collapsed, "mosaic should hide color options");
            True(((FrameworkElement)pin.FindName("PinColorSeparator")).Visibility == Visibility.Collapsed, "mosaic should hide the color separator");

            // Reuse this Application and STA without showing windows or pumping startup callbacks.
            foreach (var initialConversion in new[] { new AppSettings().ConvertFilePathsToImages, false })
            {
                var appliedValues = new List<bool>();
                var settings = new SettingsWindow(HotkeyDefinition.Default, false, ScreenshotImageFormat.Png,
                    RadialMenuSize.Medium, string.Empty, initialConversion,
                    _ => null, _ => null, _ => null, _ => null, _ => null,
                    value =>
                    {
                        appliedValues.Add(value);
                        // Stop before DialogResult, which requires ShowDialog; no real setting is saved.
                        return "test-only stop before modal completion";
                    });
                try
                {
                    var toggle = (ToggleButton)settings.FindName("ConvertFilePathsToImagesToggle");
                    True(toggle.IsChecked == initialConversion, "settings toggle should reflect default true and explicit false constructor values");
                    Equal("settings.convertFilePathsToImages", AutomationProperties.GetAutomationId(toggle));
                    Equal(0, appliedValues.Count);
                    var save = (Button)settings.FindName("SaveButton");
                    save.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    True(appliedValues.Count == 1 && appliedValues[0] == initialConversion,
                        "save callback should receive the initial conversion option, without writing user settings");
                    toggle.IsChecked = !initialConversion;
                    save.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                    True(appliedValues.Count == 2 && appliedValues[1] == !initialConversion,
                        "save callback should receive the changed toggle value");
                }
                finally
                {
                    settings.Close();
                }
            }

            using var controller = new RadialMenuController(HotkeyDefinition.Default, RadialMenuSize.Medium);
            var controllerType = typeof(RadialMenuController);
            const BindingFlags privateInstance = BindingFlags.Instance | BindingFlags.NonPublic;
            var radialWindow = (RadialMenuWindow)controllerType.GetField("_window", privateInstance)!.GetValue(controller)!;
            var items = (Canvas)radialWindow.FindName("ItemLayer");
            True(items.Children.Count == 4, "radial menu should retain all four actions");
            FrameworkElement? pinItem = null;
            controller.PinAvailabilityProvider = () => false;
            controller.RefreshActionAvailability();
            foreach (FrameworkElement item in items.Children)
            {
                var id = AutomationProperties.GetAutomationId(item);
                if (id == "radial.pin")
                {
                    pinItem = item;
                    True(!item.IsEnabled, "clipboard without pinnable content should disable radial.pin");
                    Equal(0.35, item.Opacity, 0.001);
                }
                else
                {
                    True(id is "radial.screenshot" or "radial.ocr" or "radial.record", "other radial action IDs should remain stable");
                    True(item.IsEnabled, $"clipboard availability should not disable {id}");
                    Equal(1.0, item.Opacity, 0.001);
                }
            }
            True(pinItem is not null, "radial.pin should be identifiable by automation ID");
            radialWindow.SetHover(RadialAction.Pin);
            Equal("选择操作", ((TextBlock)radialWindow.FindName("CenterTitle")).Text);
            Equal("拖动移动 · 1–4 · Esc", ((TextBlock)radialWindow.FindName("CenterSubtitle")).Text);
            foreach (var action in new[] { RadialAction.Screenshot, RadialAction.Ocr, RadialAction.Record })
            {
                radialWindow.SetHover(action);
                Equal(RadialActionInfo.Get(action).Title, ((TextBlock)radialWindow.FindName("CenterTitle")).Text);
            }
            radialWindow.SetHover(RadialAction.Pin);
            Equal("选择操作", ((TextBlock)radialWindow.FindName("CenterTitle")).Text);

            var selected = new List<RadialAction>();
            controller.ActionSelected += selected.Add;
            controllerType.GetField("_isOpen", privateInstance)!.SetValue(controller, true);
            var selectAction = controllerType.GetMethod("SelectAction", privateInstance)!;
            selectAction.Invoke(controller, new object[] { RadialAction.Pin });
            Equal(0, selected.Count);
            True(controller.IsOpen, "selecting a disabled pin must neither fire an event nor close the radial menu");
            controller.PinAvailabilityProvider = () => true;
            object[] messageArguments = { radialWindow.Handle, 0x031D, IntPtr.Zero, IntPtr.Zero, false };
            controllerType.GetMethod("WndProc", privateInstance)!.Invoke(controller, messageArguments);
            True(pinItem!.IsEnabled, "WM_CLIPBOARDUPDATE should refresh pin availability while the menu is open");
            Equal(1.0, pinItem.Opacity, 0.001);
            True(controller.IsOpen && selected.Count == 0, "clipboard updates should not select or close the menu");
            selectAction.Invoke(controller, new object[] { RadialAction.Pin });
            True(selected.Count == 1 && selected[0] == RadialAction.Pin, "enabled pin selection should fire exactly one Pin event");
            True(!controller.IsOpen, "successful pin selection should close the radial menu");
            selectAction.Invoke(controller, new object[] { RadialAction.Pin });
            Equal(1, selected.Count);

            var shortcut = controllerType.GetMethod("TryGetShortcutAction", BindingFlags.Static | BindingFlags.NonPublic)!;
            foreach (var virtualKey in new[] { 0x34, 0x64 })
            {
                object[] arguments = { virtualKey, RadialAction.Screenshot };
                True(shortcut.Invoke(null, arguments) is true && (RadialAction)arguments[1] == RadialAction.Pin,
                    "digit 4 and numpad 4 should map to Pin without installing input hooks");
            }
        });
    }

    private static void CaptureOverlayPreviewKeepsMouseSurface()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var type = typeof(ScreenshotOverlayWindow);
        var showHover = type.GetMethod("ShowElementHover", flags)!;
        var clearHover = type.GetMethod("ClearElementHover", flags)!;
        var updateSelection = type.GetMethod("UpdateSelectionVisual", flags)!;
        var selectionField = type.GetField("_selectionPx", flags)!;
        var hasSelectionField = type.GetField("_hasSelection", flags)!;
        var scenarios = new[]
        {
            (CaptureSessionMode.Record, false),
            (CaptureSessionMode.Screenshot, false),
            (CaptureSessionMode.Ocr, false),
            (CaptureSessionMode.Screenshot, true)
        };

        foreach (var scale in new[] { 1.0, 1.5, 2.0 })
        {
            var pixelWidth = (int)(600 * scale);
            var pixelHeight = (int)(400 * scale);
            var monitor = new ScreenMonitor
            {
                Bounds = new PhysicalRect(-pixelWidth, -pixelHeight, pixelWidth, pixelHeight),
                WorkArea = new PhysicalRect(-pixelWidth, -pixelHeight, pixelWidth, pixelHeight),
                Scale = scale
            };

            foreach (var (mode, frozen) in scenarios)
            {
                using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
                    new object[] { mode, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
                System.Drawing.Bitmap? bitmap = null;
                if (frozen)
                {
                    bitmap = new System.Drawing.Bitmap(pixelWidth, pixelHeight);
                    using var graphics = System.Drawing.Graphics.FromImage(bitmap);
                    graphics.Clear(System.Drawing.Color.Red);
                }

                var overlay = new ScreenshotOverlayWindow(session, monitor, mode, bitmap);
                try
                {
                    var root = (Grid)overlay.FindName("Root");
                    root.Measure(new Size(600, 400));
                    root.Arrange(new Rect(0, 0, 600, 400));
                    type.GetMethod("RefreshMapping", flags)!.Invoke(overlay, null);
                    var hint = (FrameworkElement)overlay.FindName("HintChip");
                    hint.Visibility = Visibility.Collapsed;
                    True(!overlay.IsVisible && new System.Windows.Interop.WindowInteropHelper(overlay).Handle == IntPtr.Zero,
                        "overlay regression must not show a window or create a native overlay");

                    RenderSurface();
                    var candidate = new PhysicalRect((int)(80 * scale), (int)(80 * scale), (int)(380 * scale), (int)(260 * scale));
                    showHover.Invoke(overlay, new object[] { candidate });
                    var reference = RenderSurface();
                    var sampleOffset = (((int)(250 * scale) * pixelWidth) + (int)(300 * scale)) * 4;
                    Equal(frozen ? 255 : 1, reference[sampleOffset + 3]);
                    if (frozen)
                    {
                        Equal(255, reference[sampleOffset + 2]);
                        Equal(0, reference[sampleOffset]);
                    }

                    for (var iteration = 0; iteration < 8; iteration++)
                    {
                        showHover.Invoke(overlay, new object[] { candidate });
                        True(reference.AsSpan().SequenceEqual(RenderSurface()), "unchanged hover must render an identical frame");
                    }

                    showHover.Invoke(overlay, new object[] { candidate.Offset((int)(24 * scale), (int)(12 * scale)) });
                    RenderSurface();
                    clearHover.Invoke(overlay, null);
                    RenderSurface();
                    showHover.Invoke(overlay, new object[] { new PhysicalRect(0, 0, pixelWidth, pixelHeight) });
                    RenderSurface();
                    type.GetMethod("Root_MouseLeave", flags)!.Invoke(overlay, new object[] { root, new MouseEventArgs(Mouse.PrimaryDevice, 0) });
                    True(((FrameworkElement)overlay.FindName("DimFull")).Visibility == Visibility.Visible,
                        "a genuine leave should still restore the idle mask");
                    RenderSurface();
                    hasSelectionField.SetValue(overlay, true);
                    selectionField.SetValue(overlay, candidate);
                    updateSelection.Invoke(overlay, new object[] { false });
                    RenderSurface();
                    Equal(ScreenGeometry.ToVirtual(candidate, monitor.Bounds), overlay.VirtualSelection);
                    True(!overlay.IsVisible && new System.Windows.Interop.WindowInteropHelper(overlay).Handle == IntPtr.Zero,
                        "offscreen state transitions must not activate the desktop");

                    byte[] RenderSurface()
                    {
                        root.UpdateLayout();
                        var render = new RenderTargetBitmap(pixelWidth, pixelHeight, 96 * scale, 96 * scale, PixelFormats.Pbgra32);
                        render.Render(root);
                        var pixels = new byte[pixelWidth * pixelHeight * 4];
                        render.CopyPixels(pixels, pixelWidth * 4, 0);
                        for (var offset = 3; offset < pixels.Length; offset += 4)
                        {
                            if (pixels[offset] == 0)
                            {
                                throw new InvalidOperationException($"{mode} @{scale}x preview contains an alpha-zero mouse-through pixel");
                            }
                        }
                        return pixels;
                    }
                }
                finally
                {
                    overlay.Close();
                }
            }
        }
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
