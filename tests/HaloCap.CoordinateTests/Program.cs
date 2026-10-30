using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
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
using HaloCap.Interop;
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
            ("Typing single keys are classified", TypingSingleKeysAreClassified),
            ("Capture typed key hook classifies input keys", CaptureTypedKeyHookClassifiesKeys),
            ("Color clipboard formatter matches hex and rgb", ColorClipboardFormatterMatches),
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
            ("OCR settings default to the native engine", OcrSettingsDefaultAndNormalize),
            ("OCR native image prepare resizes and composites", OcrImagePrepareResizesAndComposites),
            ("OCR result window exposes engines and search", OcrResultWindowExposesControls),
            ("OCR result window searches and switches views", OcrResultWindowSearchesAndSwitchesViews),
            ("OCR settings window binds and saves OCR values", OcrSettingsWindowBindsAndSaves),
            ("OCR native engine recognizes a rendered text image", OcrNativeEngineRecognizesRenderedText),
            ("OCR model engine recognizes a rendered text image", OcrModelEngineRecognizesRenderedText),
            ("OCR model engine handles a dark background", OcrModelEngineHandlesDarkBackground),
            ("WPF chrome, clipboard actions and capture hover render correctly", ToolbarVectorIconsPreserveBoundsAndColors)
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

    private static void TypingSingleKeysAreClassified()
    {
        True(new HotkeyDefinition(ModifierKeys.None, Key.C).IsTypingSingleKey,
            "letter single keys must go through the low-level hook");
        True(new HotkeyDefinition(ModifierKeys.None, Key.D9).IsTypingSingleKey,
            "digit single keys must go through the low-level hook");
        True(new HotkeyDefinition(ModifierKeys.None, Key.NumPad9).IsTypingSingleKey,
            "numpad single keys must go through the low-level hook");
        True(!new HotkeyDefinition(ModifierKeys.None, Key.F1).IsTypingSingleKey,
            "function keys should stay on RegisterHotKey");
        True(!new HotkeyDefinition(ModifierKeys.Control, Key.C).IsTypingSingleKey,
            "modified shortcuts are not typing keys");
    }

    private static void ColorClipboardFormatterMatches()
    {
        var color = Color.FromRgb(0x12, 0xAB, 0xFF);
        Equal("#12ABFF", ColorClipboardFormatter.Format(color, false));
        Equal("rgb(18, 171, 255)", ColorClipboardFormatter.Format(color, true));
    }

    private static void CaptureTypedKeyHookClassifiesKeys()
    {
        True(CaptureTypedKeyHook.IsTypingKey(0x41), "A should be intercepted for the overlay");
        True(CaptureTypedKeyHook.IsTypingKey(0x43), "C should be intercepted for color copy");
        True(CaptureTypedKeyHook.IsTypingKey(0x20), "Space should be intercepted for annotation mode");
        True(CaptureTypedKeyHook.IsTypingKey(0xDB), "[ should be intercepted for brush size");
        True(!CaptureTypedKeyHook.IsTypingKey(0x1B), "Esc must pass through to the overlay");
        True(!CaptureTypedKeyHook.IsTypingKey(0x0D), "Enter must pass through to the overlay");
        True(!CaptureTypedKeyHook.IsTypingKey(0x70), "F1 must pass through to the global hotkey");
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
            CaptureSelectionHintsStayLightAndConcise();
            CaptureSelectionFallsBackToFullScreen();
            CaptureStartupKeepsHoverCandidateAndClearsDots();
            CaptureOverlayReleasesFrozenBuffers();
            OverlaySamplesFrozenPixel();
            ScreenCaptureConversionPreservesPixelsAndDpi();
            RecordSelectionDimsLikeOtherModes();
            CrossScreenSelectionSharesStateAndComposesCapture();
            SettingsAndTrayChromeUseCleanLayout(app);
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
            True(!(bool)typeof(PinWindow).GetField("_textBold", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(pin)!,
                "pin text tool should default to regular weight");
            var pinShell = (FrameworkElement)pin.FindName("PinShell");
            var root = (FrameworkElement)pin.FindName("Root");
            typeof(PinWindow).GetMethod("SetAnnotationMode", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(pin, new object[] { true });
            ((Button)pin.FindName("ToolShapeButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            ArrangeShell();
            var mainBar = (Border)pin.FindName("AnnotationBar");
            var optionBar = (Border)pin.FindName("PinOptionBar");
            var host = (FrameworkElement)pin.FindName("AnnotationBarHost");
            True(((Viewbox)host).Stretch == Stretch.None,
                "the pin annotation toolbar should keep its natural size instead of scaling with the pin");
            var mainOrigin = mainBar.TransformToAncestor(host).Transform(new Point());
            var optionOrigin = optionBar.TransformToAncestor(host).Transform(new Point());
            True(optionOrigin.Y >= mainOrigin.Y + mainBar.ActualHeight + 5, "pin options should expand on a separate row");
            var hostOrigin = host.TransformToAncestor(pinShell).Transform(new Point());
            var rootOrigin = root.TransformToAncestor(pinShell).Transform(new Point());
            True(hostOrigin.Y >= rootOrigin.Y + root.ActualHeight - 0.5,
                "the pin annotation toolbar should sit below the image, outside its bounds");
            var tools = (StackPanel)mainBar.Child;
            var done = (Button)tools.Children[tools.Children.Count - 1];
            True(done.Content is Viewbox, "pin completion should contain only the checkmark icon");
            var closeEdit = (Button)pin.FindName("CloseEditButton");
            Equal("action.close", AutomationProperties.GetAutomationId(closeEdit));
            True(tools.Children[tools.Children.Count - 2] == closeEdit, "the close button should sit next to the completion button");
            var editSeparator = (Border)pin.FindName("EditSeparator");
            True(tools.Children[tools.Children.Count - 3] == editSeparator,
                "a separator should divide the pin close and completion buttons from the tools");
            True(editSeparator.Width == 1 &&
                 editSeparator.Background is SolidColorBrush { Color: var separatorColor } &&
                 separatorColor == Color.FromArgb(0x1A, 0, 0, 0),
                "the pin toolbar separator should match the screenshot toolbar divider");
            closeEdit.RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            ArrangeShell();
            True(((FrameworkElement)pin.FindName("AnnotationBarHost")).Visibility == Visibility.Collapsed,
                "closing from the pin toolbar should exit annotation mode instead of closing the pin");
            typeof(PinWindow).GetMethod("SetAnnotationMode", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!.Invoke(pin, new object[] { true });
            ArrangeShell();
            var workArea = new PhysicalRect(0, 0, 1920, 1040);
            True(PinWindow.PreferToolbarAbove(new PhysicalRect(100, 800, 600, 400), 96, workArea, 4),
                "a pin crossing the screen bottom should move its toolbar above the image");
            True(!PinWindow.PreferToolbarAbove(new PhysicalRect(100, 100, 600, 400), 96, workArea, 4),
                "a pin with room below should keep the toolbar under the image");
            True(PinWindow.PreferToolbarAbove(new PhysicalRect(100, 900, 600, 600), 96, workArea, 4),
                "a pin taller than the work area should use the side with more room");
            var narrow = PinWindow.FitImageWindow(88, 460);
            Equal(460, narrow.WindowWidth);
            Equal(186, narrow.SideInset);
            var wide = PinWindow.FitImageWindow(1208, 460);
            Equal(1208, wide.WindowWidth);
            Equal(0, wide.SideInset);
            var plain = PinWindow.FitImageWindow(88, 0);
            Equal(88, plain.WindowWidth);
            Equal(0, plain.SideInset);
            var toolbarStack = (StackPanel)pin.FindName("ToolbarStack");
            var placement = typeof(PinWindow).GetMethod("ApplyToolbarPlacement", System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic)!;
            placement.Invoke(pin, new object[] { true });
            ArrangeShell();
            True(Grid.GetRow(host) == 0 && toolbarStack.Children[0] == optionBar,
                "the flipped toolbar should sit above the image with its option bar on top");
            placement.Invoke(pin, new object[] { false });
            ArrangeShell();
            True(Grid.GetRow(host) == 2 && toolbarStack.Children[toolbarStack.Children.Count - 1] == optionBar,
                "the toolbar should return below the image when there is room");
            foreach (var name in new[] { "ToolShapeButton", "ToolArrowButton", "ToolPenButton", "ToolHighlighterButton", "ToolEraserButton", "ToolTextButton", "ToolMosaicButton", "ToolStepButton" })
            {
                ((Button)pin.FindName(name)).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                ArrangeShell();
                True(optionBar.ActualWidth <= mainBar.ActualWidth + 0.1, $"{name} options should not exceed the main toolbar width");
            }
            ((Button)pin.FindName("ToolMosaicButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            True(((FrameworkElement)pin.FindName("PinColorGroup")).Visibility == Visibility.Collapsed, "mosaic should hide color options");
            True(((FrameworkElement)pin.FindName("PinColorSeparator")).Visibility == Visibility.Collapsed, "mosaic should hide the color separator");

            ToolbarHoverTintsPreserveActiveBackgrounds();
            PinOutlineAndLightMenuBehaveConsistently(pin);
            PinOcrUsesOriginalImage();

            void ArrangeShell()
            {
                pinShell.Measure(new Size(1100, 900));
                pinShell.Arrange(new Rect(0, 0, 1100, 900));
                pinShell.UpdateLayout();
            }

            // Reuse this Application and STA without showing windows or pumping startup callbacks.
            foreach (var initialConversion in new[] { new AppSettings().ConvertFilePathsToImages, false })
            {
                var appliedValues = new List<bool>();
                var settings = new SettingsWindow(HotkeyDefinition.Default, DefaultActionHotkeys(), false, ScreenshotImageFormat.Png,
                    RadialMenuSize.Medium, string.Empty, initialConversion,
                    (_, _) => null, _ => null, _ => null, _ => null, _ => null,
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
            var nativeType = typeof(RadialMenuWindow).Assembly.GetType("HaloCap.Interop.NativeMethods")!;
            var exStyle = ((IntPtr)nativeType.GetMethod("GetWindowLongPtr", BindingFlags.Static | BindingFlags.Public)!
                .Invoke(null, new object[] { radialWindow.Handle, -20 })!).ToInt64();
            True((exStyle & 0x00000020) == 0,
                "the aperture must not be click-through (WS_EX_TRANSPARENT)");
            True((exStyle & 0x08000000) != 0,
                "the aperture must remain a no-activate window");
            var radialRoot = (Grid)radialWindow.FindName("Root");
            True(((SolidColorBrush)radialRoot.Background).Color.A == 1,
                "the aperture needs a low-alpha hit surface so mouse input cannot fall through");
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
            var centerTitle = (TextBlock)radialWindow.FindName("CenterTitle");
            var centerDot = (System.Windows.Shapes.Ellipse)radialWindow.FindName("CenterDot");
            Equal("HaloCap", centerTitle.Text);
            True(centerDot.Visibility == Visibility.Collapsed,
                "the idle radial center should hide the accent dot");
            True(radialWindow.FindName("CenterSubtitle") is null,
                "the radial center should no longer show the multi-word hint");
            var outerDisk = (System.Windows.Shapes.Ellipse)radialWindow.FindName("OuterDisk");
            var diskGradient = (RadialGradientBrush)outerDisk.Fill;
            True(diskGradient.GradientStops.All(stop => stop.Color.R >= 0xEF && stop.Color.G >= 0xEF && stop.Color.B >= 0xEF),
                "the radial disk should use a light surface");
            True(((SolidColorBrush)((System.Windows.Shapes.Ellipse)radialWindow.FindName("CenterDisk")).Fill).Color == Colors.White,
                "the radial center disk should be white");
            True(((SolidColorBrush)centerTitle.Foreground).Color == Color.FromRgb(0x1D, 0x1D, 0x1F),
                "the radial center label should use dark text on the light surface");
            foreach (var action in new[] { RadialAction.Screenshot, RadialAction.Ocr, RadialAction.Record })
            {
                radialWindow.SetHover(action);
                var info = RadialActionInfo.Get(action);
                Equal(info.Title, centerTitle.Text);
                True(centerDot.Visibility == Visibility.Visible,
                    $"{action} should show its accent dot in the radial center");
                True(((SolidColorBrush)centerDot.Fill).Color == UiColors.Parse(info.AccentHex),
                    $"{action} should color the center dot with its accent");
            }
            radialWindow.SetHover(RadialAction.Pin);
            Equal("HaloCap", centerTitle.Text);
            True(centerDot.Visibility == Visibility.Collapsed,
                "the idle radial center should hide the accent dot again");

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
                    var dimFull = (FrameworkElement)overlay.FindName("DimFull");
                    True(dimFull.Visibility == Visibility.Visible,
                        "every capture mode should start with the full-screen dim");
                    Equal(92, ((SolidColorBrush)((System.Windows.Shapes.Rectangle)dimFull).Fill).Color.A);
                    True(!(bool)type.GetField("_textBold", flags)!.GetValue(overlay)!,
                        "capture text tool should default to regular weight");
                    var dimTop = (FrameworkElement)overlay.FindName("DimTop");
                    var doneButton = (Button)overlay.FindName("DoneButton");
                    var cancelButton = (Button)overlay.FindName("CancelButton");
                    var toolbarPanel = (StackPanel)((Border)overlay.FindName("Toolbar")).Child;
                    True(doneButton.Visibility == (mode == CaptureSessionMode.Screenshot
                            ? Visibility.Visible
                            : Visibility.Collapsed),
                        "only screenshot mode should show the completion check");
                    True(toolbarPanel.Children.IndexOf(doneButton) == toolbarPanel.Children.IndexOf(cancelButton) + 1,
                        "the completion check should sit immediately to the right of close");
                    Equal("Done", doneButton.Tag as string ?? string.Empty);
                    True(overlay.Opacity == 0,
                        "the overlay must stay transparent until its first frame has rendered");
                    type.GetMethod("PrepareReveal", flags)!.Invoke(overlay, null);
                    Equal(0.01, overlay.Opacity, 0.0001);
                    type.GetMethod("CommitReveal", flags)!.Invoke(overlay, null);
                    Equal(1.0, overlay.Opacity, 0.0001);
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
                    Equal(92, ((SolidColorBrush)((System.Windows.Shapes.Rectangle)dimTop).Fill).Color.A);
                    True(dimTop.Visibility == Visibility.Visible, "a hovered window should show the outside dim");
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
                    var idleMask = ((FrameworkElement)overlay.FindName("DimFull")).Visibility;
                    True(idleMask == Visibility.Visible,
                        "a genuine leave should restore the idle mask");
                    True(((FrameworkElement)overlay.FindName("ElementBorder")).Visibility == Visibility.Collapsed,
                        "leaving the overlay should clear the hovered-window border");
                    var idle = RenderSurface();
                    var cornerOffset = (((int)(8 * scale) * pixelWidth) + (int)(8 * scale)) * 4;
                    Equal(
                        frozen ? 255 : 92,
                        idle[cornerOffset + 3],
                        1);
                    hasSelectionField.SetValue(overlay, true);
                    selectionField.SetValue(overlay, candidate);
                    updateSelection.Invoke(overlay, new object[] { false });
                    RenderSurface();
                    Equal(ScreenGeometry.ToVirtual(candidate, monitor.Bounds), overlay.VirtualSelection);
                    Equal(140, ((SolidColorBrush)((System.Windows.Shapes.Rectangle)dimTop).Fill).Color.A);
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

    private static void CaptureSelectionHintsStayLightAndConcise()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        foreach (var mode in new[] { CaptureSessionMode.Screenshot, CaptureSessionMode.Ocr, CaptureSessionMode.Record })
        {
            using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
                new object[] { mode, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
            var monitor = new ScreenMonitor
            {
                Bounds = new PhysicalRect(0, 0, 600, 400),
                WorkArea = new PhysicalRect(0, 0, 600, 400),
                Scale = 1.0
            };
            var overlay = new ScreenshotOverlayWindow(session, monitor, mode, null);
            try
            {
                var chip = (Border)overlay.FindName("HintChip");
                var background = ((SolidColorBrush)chip.Background).Color;
                True(background.A >= 0xE0 && background.R >= 0xF0 && background.G >= 0xF0 && background.B >= 0xF0,
                    $"{mode} hint chip should use a light surface");

                var text = (TextBlock)overlay.FindName("HintText");
                var foreground = ((SolidColorBrush)text.Foreground).Color;
                True(foreground.R < 0x60 && foreground.G < 0x60 && foreground.B < 0x60,
                    $"{mode} hint text should be dark on the light chip");
                True(text.Text.Length <= 32 && text.Text.Contains("Esc 取消") && !text.Text.Contains("工具"),
                    $"{mode} hint should stay short and actionable");
            }
            finally
            {
                overlay.Close();
            }
        }
    }

    private static void CaptureSelectionFallsBackToFullScreen()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 600, 400),
            WorkArea = new PhysicalRect(0, 0, 600, 400),
            Scale = 1.0
        };
        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Screenshot, null);
        try
        {
            var root = (Grid)overlay.FindName("Root");
            root.Measure(new Size(600, 400));
            root.Arrange(new Rect(0, 0, 600, 400));
            typeof(ScreenshotOverlayWindow).GetMethod("RefreshMapping", flags)!.Invoke(overlay, null);

            typeof(ScreenshotOverlayWindow).GetMethod("ShowElementHoverOrFullScreen", flags)!
                .Invoke(overlay, new object?[] { null });

            var border = (FrameworkElement)overlay.FindName("ElementBorder");
            True(border.Visibility == Visibility.Visible, "the fallback should show the element border");
            Equal(600.0, border.Width, 0.01);
            Equal(400.0, border.Height, 0.01);
            Equal(
                new PhysicalRect(0, 0, 600, 400),
                (PhysicalRect)typeof(ScreenshotOverlayWindow).GetField("_hoverElementPx", flags)!.GetValue(overlay)!);

            var type = typeof(ScreenshotOverlayWindow);
            var dragKindType = type.GetNestedType("DragKind", BindingFlags.NonPublic)!;
            type.GetField("_clickElementPx", flags)!.SetValue(overlay, default(PhysicalRect));
            type.GetField("_dragging", flags)!.SetValue(overlay, true);
            type.GetField("_dragKind", flags)!.SetValue(overlay, Enum.Parse(dragKindType, "Create"));
            session.BeginSelection(overlay, new PhysicalRect(100, 100, 5, 5));
            type.GetMethod("EndDrag", flags)!.Invoke(overlay, null);
            Equal(new PhysicalRect(0, 0, 600, 400), session.SelectionVirtual);
        }
        finally
        {
            overlay.Close();
        }
    }

    private static void CaptureStartupKeepsHoverCandidateAndClearsDots()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        True(WindowProbe.IsCandidateSize(new PhysicalRect(0, 0, 32, 32)),
            "a 32px window should remain a valid hover candidate");
        True(!WindowProbe.IsCandidateSize(new PhysicalRect(0, 0, 31, 32)),
            "tiny helper windows should not become a dot-like hover");

        NativeMethods.GetCursorPos(out var cursor);
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(cursor.X - 1000, cursor.Y - 1000, 2000, 2000),
            WorkArea = new PhysicalRect(cursor.X - 1000, cursor.Y - 1000, 2000, 2000),
            Scale = 1.0
        };

        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Screenshot, null);
        try
        {
            var root = (Grid)overlay.FindName("Root");
            root.Measure(new Size(600, 400));
            root.Arrange(new Rect(0, 0, 600, 400));
            typeof(ScreenshotOverlayWindow).GetMethod("RefreshMapping", flags)!.Invoke(overlay, null);

            var border = (FrameworkElement)overlay.FindName("ElementBorder");
            typeof(ScreenshotOverlayWindow).GetMethod("ShowElementHover", flags)!
                .Invoke(overlay, new object[] { new PhysicalRect(10, 10, 200, 150) });
            True(border.Visibility == Visibility.Visible, "startup should expose the hovered window");

            typeof(ScreenshotOverlayWindow).GetMethod("Root_MouseLeave", flags)!
                .Invoke(overlay, new object[] { root, new MouseEventArgs(Mouse.PrimaryDevice, 0) });
            True(border.Visibility == Visibility.Visible,
                "a synthetic mouse leave must keep the hover while the cursor is still on the monitor");

            session.BeginSelection(
                overlay,
                ScreenGeometry.ToVirtual(new PhysicalRect(20, 20, 4, 4), monitor.Bounds));
            True(!session.IsSelectionComplete, "the tiny selection should still be pending");
            True(overlay.DiscardIncompleteTinySelection(),
                "a stray point-sized selection should be discarded");
            True(!session.HasSelection, "discarding the dot should return the session to idle");
            True(!overlay.HasSelection, "the overlay should leave the dot selection state");
            True(((FrameworkElement)overlay.FindName("SelectionBorder")).Visibility == Visibility.Collapsed,
                "the dot selection chrome should be gone");
            True(((FrameworkElement)overlay.FindName("DimFull")).Visibility == Visibility.Visible ||
                 ((FrameworkElement)overlay.FindName("ElementBorder")).Visibility == Visibility.Visible,
                "the overlay should return to idle or hover, never a bare dot");

            typeof(ScreenshotOverlayWindow).GetField("_lastElementProbeTick", flags)!
                .SetValue(overlay, Environment.TickCount64);
            typeof(ScreenshotOverlayWindow).GetMethod("ResetToIdle", flags)!.Invoke(overlay, null);
            True(((FrameworkElement)overlay.FindName("ElementBorder")).Visibility == Visibility.Visible,
                "reset must force a fresh probe so the first frame already has the window candidate");

            session.BeginSelection(
                overlay,
                ScreenGeometry.ToVirtual(new PhysicalRect(20, 20, 100, 80), monitor.Bounds));
            True(!overlay.DiscardIncompleteTinySelection(),
                "a pending selection with a real area must not be discarded");

            overlay.ActivateForSelection();
            True((bool)typeof(ScreenshotOverlayWindow).GetField("_activationPending", flags)!.GetValue(overlay)!,
                "focus should wait until the overlay is actually on screen");
            typeof(ScreenshotOverlayWindow).GetMethod("PrepareReveal", flags)!.Invoke(overlay, null);
            Equal(0.01, overlay.Opacity, 0.0001);
            typeof(ScreenshotOverlayWindow).GetMethod("CommitReveal", flags)!.Invoke(overlay, null);
            Equal(1.0, overlay.Opacity, 0.0001);
            True((bool)typeof(ScreenshotOverlayWindow).GetField("_revealCompleted", flags)!.GetValue(overlay)!,
                "the overlay should be fully visible before focus is taken");
        }
        finally
        {
            overlay.Close();
        }
    }

    private static void CaptureOverlayReleasesFrozenBuffers()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var type = typeof(ScreenshotOverlayWindow);
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 120, 80),
            WorkArea = new PhysicalRect(0, 0, 120, 80),
            Scale = 1.0
        };

        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var bitmap = CreateDrawingBitmap(120, 80, System.Drawing.Color.Red);
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Screenshot, bitmap);

        True(type.GetField("_frozenSource", flags)!.GetValue(overlay) is not null,
            "the overlay should keep a frozen source while alive");
        True(type.GetField("_frozenBitmap", flags)!.GetValue(overlay) is not null,
            "the overlay should keep a frozen bitmap while alive");
        var image = (Image)overlay.FindName("FrozenImage");
        True(image.Source is not null, "the frozen image should be shown");

        overlay.CloseOverlay();

        True(type.GetField("_frozenSource", flags)!.GetValue(overlay) is null,
            "closing the overlay must drop the frozen source");
        True(type.GetField("_frozenBitmap", flags)!.GetValue(overlay) is null,
            "closing the overlay must drop the frozen bitmap");
        True(image.Source is null, "closing the overlay must clear the image visual");
    }

    private static void OverlaySamplesFrozenPixel()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 20, 20),
            WorkArea = new PhysicalRect(0, 0, 20, 20),
            Scale = 1.0
        };

        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var bitmap = CreateDrawingBitmap(20, 20, System.Drawing.Color.FromArgb(255, 12, 34, 56));
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Screenshot, bitmap);
        try
        {
            True(overlay.TryGetScreenPixel(7, 9, out var color), "sampling a frozen pixel should succeed");
            True(color.R == 12 && color.G == 34 && color.B == 56,
                "the sampled color should come from the frozen pixel");
            True(!overlay.TryGetScreenPixel(20, 0, out _), "out-of-bitmap pixels must be rejected");
        }
        finally
        {
            overlay.CloseOverlay();
        }
    }

    private static void ScreenCaptureConversionPreservesPixelsAndDpi()
    {
        using var bitmap = CreateDrawingBitmap(10, 8, System.Drawing.Color.FromArgb(255, 18, 52, 86));
        var source = ScreenCaptureService.ToBitmapSource(bitmap);
        Equal(10, source.PixelWidth);
        Equal(8, source.PixelHeight);
        Equal(96.0, source.DpiX, 0.01);
        var pixel = ReadPixel(source, 5, 4);
        True(pixel.R == 18 && pixel.G == 52 && pixel.B == 86, "conversion must preserve pixels");

        var scaled = ScreenCaptureService.ToBitmapSource(bitmap, 144);
        Equal(144.0, scaled.DpiX, 0.01);
        Equal(144.0, scaled.DpiY, 0.01);
        var scaledPixel = ReadPixel(scaled, 1, 1);
        True(scaledPixel.R == 18 && scaledPixel.G == 52 && scaledPixel.B == 86,
            "scaled conversion must preserve pixels");
        True(source.IsFrozen && scaled.IsFrozen, "capture bitmaps should be frozen for cross-thread use");
    }

    private static void RecordSelectionDimsLikeOtherModes()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var type = typeof(ScreenshotOverlayWindow);
        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Record, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 600, 400),
            WorkArea = new PhysicalRect(0, 0, 600, 400),
            Scale = 1.0
        };
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Record, null);
        try
        {
            var root = (FrameworkElement)overlay.Content;
            root.Measure(new Size(600, 400));
            root.Arrange(new Rect(0, 0, 600, 400));
            root.UpdateLayout();
            type.GetMethod("RefreshMapping", flags)!.Invoke(overlay, null);

            var updateSelection = type.GetMethod("UpdateSelectionVisual", flags)!;
            var selectionField = type.GetField("_selectionPx", flags)!;
            type.GetField("_hasSelection", flags)!.SetValue(overlay, false);
            var dragKindType = type.GetNestedType("DragKind", BindingFlags.NonPublic)!;
            type.GetField("_dragKind", flags)!.SetValue(overlay, Enum.Parse(dragKindType, "Create"));

            selectionField.SetValue(overlay, new PhysicalRect(80, 80, 0, 0));
            updateSelection.Invoke(overlay, new object[] { true });
            True(((FrameworkElement)overlay.FindName("DimFull")).Visibility == Visibility.Visible,
                "a zero-size press should keep the full-screen mask instead of leaving a lone dot");

            selectionField.SetValue(overlay, new PhysicalRect(80, 80, 2, 2));
            updateSelection.Invoke(overlay, new object[] { true });
            True(((FrameworkElement)overlay.FindName("DimTop")).Visibility == Visibility.Visible,
                "record mode should dim outside the selection like screenshot and OCR");

            selectionField.SetValue(overlay, new PhysicalRect(80, 80, 300, 200));
            updateSelection.Invoke(overlay, new object[] { true });
            var dimTop = (FrameworkElement)overlay.FindName("DimTop");
            True(dimTop.Visibility == Visibility.Visible && Math.Abs(dimTop.Height - 80) < 0.01,
                "the outside dim should follow the selection edges in record mode");
        }
        finally
        {
            overlay.Close();
        }
    }

    private static void CrossScreenSelectionSharesStateAndComposesCapture()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var monitorA = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 600, 400),
            WorkArea = new PhysicalRect(0, 0, 600, 400),
            Scale = 1.0,
            IsPrimary = true
        };
        var monitorB = new ScreenMonitor
        {
            Bounds = new PhysicalRect(600, 0, 600, 400),
            WorkArea = new PhysicalRect(600, 0, 600, 400),
            Scale = 1.0
        };

        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var red = CreateDrawingBitmap(600, 400, System.Drawing.Color.Red);
        var blue = CreateDrawingBitmap(600, 400, System.Drawing.Color.RoyalBlue);
        var overlayA = new ScreenshotOverlayWindow(session, monitorA, CaptureSessionMode.Screenshot, red);
        var overlayB = new ScreenshotOverlayWindow(session, monitorB, CaptureSessionMode.Screenshot, blue);
        try
        {
            Arrange(overlayA, 600, 400);
            Arrange(overlayB, 600, 400);

            Equal(1200, session.DesktopBounds.Width);
            session.BeginSelection(overlayA, new PhysicalRect(450, 100, 300, 200));
            True(!session.CanPinSelection,
                "the pin hotkey should wait until the screenshot selection is complete");
            session.CompleteSelection();
            True(session.CanPinSelection,
                "a completed screenshot selection should accept the pin hotkey");

            var mirrorDimTop = (FrameworkElement)overlayB.FindName("DimTop");
            var mirrorDimBottom = (FrameworkElement)overlayB.FindName("DimBottom");
            var mirrorDimLeft = (FrameworkElement)overlayB.FindName("DimLeft");
            var mirrorDimRight = (FrameworkElement)overlayB.FindName("DimRight");
            Equal(100.0, mirrorDimTop.Height, 0.01);
            Equal(300.0, Canvas.GetTop(mirrorDimBottom), 0.01);
            Equal(100.0, mirrorDimBottom.Height, 0.01);
            True(mirrorDimLeft.Visibility == Visibility.Collapsed,
                "the dim mask should leave the mirrored selection edge open");
            Equal(150.0, Canvas.GetLeft(mirrorDimRight), 0.01);
            Equal(450.0, mirrorDimRight.Width, 0.01);

            var hostDimRight = (FrameworkElement)overlayA.FindName("DimRight");
            var hostDimLeft = (FrameworkElement)overlayA.FindName("DimLeft");
            Equal(450.0, hostDimLeft.Width, 0.01);
            True(hostDimRight.Visibility == Visibility.Collapsed,
                "the dim mask hole should line up with the selection on the host monitor");

            True(session.IsCrossMonitorSelection, "selection spanning two monitors should be cross-screen");
            Equal(150, overlayA.LocalSelection.Width);
            Equal(450, overlayA.LocalSelection.Left);
            Equal(0, overlayB.LocalSelection.Left);
            Equal(150, overlayB.LocalSelection.Width);
            True(overlayA.HasSelection && overlayB.HasSelection, "both monitors should expose the shared selection");
            True(((FrameworkElement)overlayA.FindName("Toolbar")).Visibility == Visibility.Visible,
                "the monitor that starts the selection should host the toolbar");
            True(((FrameworkElement)overlayB.FindName("Toolbar")).Visibility == Visibility.Collapsed,
                "mirror monitors should not show a second toolbar");

            var composed = session.CreateSelectionBitmap(false);
            True(composed is not null, "cross-screen capture should compose a bitmap");
            Equal(300, composed!.PixelWidth);
            Equal(200, composed.PixelHeight);
            True(ReadPixel(composed, 10, 10) == Colors.Red, "left part should come from the first monitor");
            True(ReadPixel(composed, 160, 10) == Colors.RoyalBlue, "composition should split at the physical monitor boundary");
            True(ReadPixel(composed, 290, 10) == Colors.RoyalBlue, "right part should come from the second monitor");

            session.UpdateSelection(new PhysicalRect(700, 100, 300, 200));
            True(ReferenceEquals(session.SelectionHost, overlayB),
                "the toolbar host should follow a selection moved to another monitor");
            True(((FrameworkElement)overlayB.FindName("Toolbar")).Visibility == Visibility.Visible,
                "the new host should show the toolbar");
            True(((FrameworkElement)overlayA.FindName("Toolbar")).Visibility == Visibility.Collapsed,
                "the previous host should hide the toolbar");
            True(overlayA.LocalSelection.IsEmpty, "a selection outside a monitor should not render chrome there");

            var second = session.CreateSelectionBitmap(false);
            True(second is not null &&
                 ReadPixel(second!, 10, 10) == Colors.RoyalBlue &&
                 ReadPixel(second!, 290, 10) == Colors.RoyalBlue,
                "a selection moved onto one monitor should capture only that monitor");

            var inside = (bool)typeof(ScreenshotOverlayWindow).GetMethod("IsInsideSelection", flags)!
                .Invoke(overlayB, new object[] { new Point(400, 200) })!;
            True(inside, "a mirror monitor should hit-test the shared selection in its local coordinates");
            var handle = (int)typeof(ScreenshotOverlayWindow).GetMethod("HitTestSelectionHandle", flags)!
                .Invoke(overlayB, new object[] { new Point(400, 200) })!;
            True((handle & 2) != 0, "the right-edge handle should be hittable on the monitor that owns that edge");

            var moveVirtual = typeof(ScreenshotOverlayWindow).GetMethod("MoveVirtualSelection", flags)!;
            var movedSelection = (PhysicalRect)moveVirtual.Invoke(overlayB,
                new object[] { new PhysicalRect(700, 100, 300, 200), -100, 0 })!;
            Equal(600, movedSelection.Left);

            var resizeVirtual = typeof(ScreenshotOverlayWindow).GetMethod("ResizeVirtualSelection", flags)!;
            var resizedSelection = (PhysicalRect)resizeVirtual.Invoke(overlayB,
                new object[] { new PhysicalRect(700, 100, 300, 200), 2, new PhysicalRect(1100, 300, 0, 0) })!;
            Equal(400, resizedSelection.Width);

            var normalize = typeof(ScreenshotOverlayWindow).GetMethod("NormalizeVirtualSelection", flags)!;
            var screenshotSpan = (PhysicalRect)normalize.Invoke(overlayA,
                new object[] { new PhysicalRect(500, 100, 0, 0), new PhysicalRect(700, 150, 0, 0) })!;
            True(screenshotSpan.Right == 700, "screenshot selections should be allowed to cross monitors");

            using var recordSession = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
                new object[] { CaptureSessionMode.Record, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
            var recordOverlay = new ScreenshotOverlayWindow(recordSession, monitorA, CaptureSessionMode.Record, null);
            try
            {
                Arrange(recordOverlay, 600, 400);
                var recordSpan = (PhysicalRect)normalize.Invoke(recordOverlay,
                    new object[] { new PhysicalRect(500, 100, 0, 0), new PhysicalRect(700, 150, 0, 0) })!;
                True(recordSpan.Right <= 600, "record selections must stay on a single monitor");
                recordSession.BeginSelection(recordOverlay, new PhysicalRect(100, 100, 200, 100));
                recordSession.CompleteSelection();
                True(!recordSession.CanPinSelection,
                    "only screenshot selections should react to the pin hotkey");
            }
            finally
            {
                recordOverlay.Close();
            }

            var dpiMonitorA = new ScreenMonitor
            {
                Bounds = new PhysicalRect(0, 0, 600, 400),
                WorkArea = new PhysicalRect(0, 0, 600, 400),
                Scale = 1.5,
                IsPrimary = true
            };
            var dpiMonitorB = new ScreenMonitor
            {
                Bounds = new PhysicalRect(600, 0, 600, 400),
                WorkArea = new PhysicalRect(600, 0, 600, 400),
                Scale = 1.5
            };
            using var dpiSession = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
                new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
            var dpiA = new ScreenshotOverlayWindow(dpiSession, dpiMonitorA, CaptureSessionMode.Screenshot,
                CreateDrawingBitmap(600, 400, System.Drawing.Color.Red));
            var dpiB = new ScreenshotOverlayWindow(dpiSession, dpiMonitorB, CaptureSessionMode.Screenshot,
                CreateDrawingBitmap(600, 400, System.Drawing.Color.RoyalBlue));
            try
            {
                Arrange(dpiA, 600, 400);
                Arrange(dpiB, 600, 400);
                dpiSession.BeginSelection(dpiA, new PhysicalRect(450, 100, 300, 200));
                dpiSession.CompleteSelection();
                var dpiComposed = dpiSession.CreateSelectionBitmap(false);
                True(dpiComposed is not null, "scaled cross-screen capture should compose a bitmap");
                Equal(300, dpiComposed!.PixelWidth);
                Equal(200, dpiComposed.PixelHeight);
                True(ReadPixel(dpiComposed, 10, 10) == Colors.Red, "high-DPI composition should keep the left monitor pixels");
                True(ReadPixel(dpiComposed, 160, 10) == Colors.RoyalBlue, "high-DPI composition should not stretch monitor portions");
                True(ReadPixel(dpiComposed, 290, 10) == Colors.RoyalBlue, "high-DPI composition should keep the right monitor pixels");
            }
            finally
            {
                dpiA.Close();
                dpiB.Close();
            }

            var offsetMonitor = new ScreenMonitor
            {
                Bounds = new PhysicalRect(600, -150, 900, 600),
                WorkArea = new PhysicalRect(600, -150, 900, 600),
                Scale = 1.5
            };
            using var offsetSession = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
                new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
            var offsetOverlay = new ScreenshotOverlayWindow(offsetSession, offsetMonitor, CaptureSessionMode.Screenshot,
                CreateDrawingBitmap(900, 600, System.Drawing.Color.Green));
            try
            {
                Arrange(offsetOverlay, 600, 400);
                var virtualPoint = (PhysicalRect)typeof(ScreenshotOverlayWindow)
                    .GetMethod("ToVirtualPoint", flags)!
                    .Invoke(offsetOverlay, new object[] { new Point(100, 100) })!;
                Equal(750, virtualPoint.Left);
                Equal(0, virtualPoint.Top);

                offsetSession.BeginSelection(offsetOverlay, new PhysicalRect(700, -50, 300, 150));
                offsetSession.CompleteSelection();
                var root = (FrameworkElement)offsetOverlay.Content;
                root.UpdateLayout();
                var border = (FrameworkElement)offsetOverlay.FindName("SelectionBorder");
                Equal(66.6667, Canvas.GetLeft(border), 0.01);
                Equal(66.6667, Canvas.GetTop(border), 0.01);
                Equal(200.0, border.Width, 0.01);
                Equal(100.0, border.Height, 0.01);

                var offsetDimTop = (FrameworkElement)offsetOverlay.FindName("DimTop");
                var offsetDimBottom = (FrameworkElement)offsetOverlay.FindName("DimBottom");
                var offsetDimLeft = (FrameworkElement)offsetOverlay.FindName("DimLeft");
                var offsetDimRight = (FrameworkElement)offsetOverlay.FindName("DimRight");
                Equal(66.6667, offsetDimTop.Height, 0.01);
                Equal(66.6667, offsetDimLeft.Width, 0.01);
                Equal(66.6667, Canvas.GetTop(offsetDimLeft), 0.01);
                Equal(100.0, offsetDimLeft.Height, 0.01);
                Equal(266.6667, Canvas.GetLeft(offsetDimRight), 0.01);
                Equal(333.3333, offsetDimRight.Width, 0.01);
                Equal(166.6667, Canvas.GetTop(offsetDimBottom), 0.01);
                Equal(233.3333, offsetDimBottom.Height, 0.01);

                var annotation = (FrameworkElement)offsetOverlay.FindName("AnnotationView");
                Equal(66.6667,
                    (double)typeof(AnnotationCanvas).GetField("_originX", flags)!.GetValue(annotation)!,
                    0.01);
                Equal(66.6667,
                    (double)typeof(AnnotationCanvas).GetField("_originY", flags)!.GetValue(annotation)!,
                    0.01);

                var toolbar = (FrameworkElement)offsetOverlay.FindName("Toolbar");
                True(toolbar.Visibility == Visibility.Visible, "the host should show the toolbar");
                Equal(178.6667, toolbar.Margin.Top, 0.6);

                var strokes = (List<AnnotationStroke>)typeof(ScreenshotOverlayWindow)
                    .GetField("_strokes", flags)!.GetValue(offsetOverlay)!;
                var stroke = new AnnotationStroke
                {
                    Tool = AnnotationTool.Rectangle,
                    Color = Colors.Red,
                    Thickness = 4
                };
                stroke.Points.Add(new Point(20, 20));
                stroke.Points.Add(new Point(80, 60));
                strokes.Add(stroke);
                typeof(ScreenshotOverlayWindow).GetMethod("UpdateAnnotationFrame", flags)!
                    .Invoke(offsetOverlay, null);
                root.UpdateLayout();

                var screen = new RenderTargetBitmap(900, 600, 144, 144, PixelFormats.Pbgra32);
                screen.Render(root);
                var hasRedOnScreen = false;
                for (var y = 120; y <= 140; y++)
                {
                    for (var x = 120; x <= 140; x++)
                    {
                        var pixel = ReadPixel(screen, x, y);
                        hasRedOnScreen |= pixel.R > 200 && pixel.G < 90 && pixel.B < 90;
                    }
                }
                True(hasRedOnScreen, "annotations should render at the selection coordinate on scaled monitors");

                var annotatedCapture = offsetSession.CreateSelectionBitmap(true);
                True(annotatedCapture is not null, "annotated scaled capture should compose");
                var hasRedInCapture = false;
                for (var y = 20; y <= 40; y++)
                {
                    for (var x = 20; x <= 40; x++)
                    {
                        var pixel = ReadPixel(annotatedCapture!, x, y);
                        hasRedInCapture |= pixel.R > 200 && pixel.G < 90 && pixel.B < 90;
                    }
                }
                True(hasRedInCapture, "annotations should export at the selection coordinate on scaled monitors");
            }
            finally
            {
                offsetOverlay.Close();
            }
        }
        finally
        {
            overlayA.Close();
            overlayB.Close();
        }

        static void Arrange(ScreenshotOverlayWindow overlay, double width, double height)
        {
            var root = (FrameworkElement)overlay.Content;
            root.Measure(new Size(width, height));
            root.Arrange(new Rect(0, 0, width, height));
            root.UpdateLayout();
            typeof(ScreenshotOverlayWindow).GetMethod("RefreshMapping", flags)!.Invoke(overlay, null);
        }
    }

    private static System.Drawing.Bitmap CreateDrawingBitmap(int width, int height, System.Drawing.Color color)
    {
        var bitmap = new System.Drawing.Bitmap(width, height);
        using var graphics = System.Drawing.Graphics.FromImage(bitmap);
        graphics.Clear(color);
        return bitmap;
    }

    private static void ToolbarHoverTintsPreserveActiveBackgrounds()
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var monitor = new ScreenMonitor
        {
            Bounds = new PhysicalRect(0, 0, 600, 400),
            WorkArea = new PhysicalRect(0, 0, 600, 400),
            Scale = 1.0
        };
        using var session = (ScreenshotSession)Activator.CreateInstance(typeof(ScreenshotSession), flags, null,
            new object[] { CaptureSessionMode.Screenshot, (Action<CaptureResult>)(_ => { }) }, CultureInfo.InvariantCulture)!;
        var overlay = new ScreenshotOverlayWindow(session, monitor, CaptureSessionMode.Screenshot, null);
        System.Windows.Media.Geometry? overlayCheckGeometry = null;
        try
        {
            var toolStyle = (Style)overlay.Resources["AnnotationToolButtonStyle"];
            AssertHoverTint(toolStyle, "screenshot tool");
            AssertHoverTint((Style)overlay.Resources["OverlayButtonStyle"], "screenshot action");
            AssertHoverTint((Style)overlay.Resources["OptionStepButtonStyle"], "screenshot step");
            AssertHoverTint((Style)overlay.Resources["ColorSwatchStyle"], "screenshot color swatch", "Ring");
            True(ReferenceEquals(((Style)overlay.Resources["AnnotationOptionButtonStyle"]).BasedOn, toolStyle),
                "screenshot option buttons should reuse the tool hover");
            var closeStyle = (Style)overlay.Resources["CloseButtonStyle"];
            True(ReferenceEquals(closeStyle.BasedOn, toolStyle),
                "the close button should reuse the same tool hover");
            True(closeStyle.Setters.OfType<Setter>().All(setter => setter.Property != Control.TemplateProperty),
                "the close button should not override the shared hover template");
            var overlayDone = (Button)overlay.FindName("DoneButton");
            True(overlayDone.ReadLocalValue(Control.ForegroundProperty) == DependencyProperty.UnsetValue &&
                 overlayDone.ReadLocalValue(Control.BackgroundProperty) == DependencyProperty.UnsetValue,
                "the screenshot check should use the neutral tool colors instead of a special accent");
            True(overlay.FocusVisualStyle is null &&
                 ((FrameworkElement)overlay.FindName("Root")).FocusVisualStyle is null,
                "the overlay and its root must not draw dashed focus rectangles");
            overlayCheckGeometry = ((System.Windows.Shapes.Path)((Viewbox)overlayDone.Content).Child).Data;
        }
        finally
        {
            overlay.Close();
        }

        var pin = new PinWindow(CreateSolidBitmap(80, 60, Colors.White),
            new PhysicalRect(0, 0, 80, 60), 1, ScreenshotImageFormat.Png);
        try
        {
            var pinToolStyle = (Style)pin.Resources["PinToolButtonStyle"];
            AssertHoverTint(pinToolStyle, "pin tool");
            AssertHoverTint((Style)pin.Resources["PinBarButtonStyle"], "pin bar");
            AssertHoverTint((Style)pin.Resources["PinColorSwatchStyle"], "pin color swatch", "Ring");
            True(ReferenceEquals(((Style)pin.Resources["PinOptionButtonStyle"]).BasedOn, pinToolStyle),
                "pin option buttons should reuse the tool hover");
            var pinDone = (Button)pin.FindName("DoneEditButton");
            True(pinDone.ReadLocalValue(Control.ForegroundProperty) == DependencyProperty.UnsetValue &&
                 pinDone.ReadLocalValue(Control.BackgroundProperty) == DependencyProperty.UnsetValue,
                "the pin completion check should use the same neutral tool colors");
            True(pinDone.Foreground is SolidColorBrush { Color: var doneColor } &&
                 doneColor == Color.FromRgb(0x3C, 0x42, 0x50),
                "the pin completion check should keep the standard tool icon color");
            var pinCheckGeometry = ((System.Windows.Shapes.Path)((Viewbox)pinDone.Content).Child).Data;
            True(ReferenceEquals(pinCheckGeometry, overlayCheckGeometry),
                "the screenshot and pin completion checks should use the same icon");
            True(pin.FocusVisualStyle is null &&
                 ((FrameworkElement)pin.FindName("Root")).FocusVisualStyle is null,
                "the pin and its root must not draw dashed focus rectangles");
        }
        finally
        {
            pin.Close();
        }
    }

    private static void AssertHoverTint(Style style, string name, string target = "Hover")
    {
        var template = style.Setters.OfType<Setter>()
            .Where(setter => setter.Property == Control.TemplateProperty)
            .Select(setter => setter.Value)
            .OfType<ControlTemplate>()
            .FirstOrDefault();
        True(template is not null, $"{name} should define a control template");
        var hover = template!.Triggers.OfType<Trigger>()
            .FirstOrDefault(trigger => trigger.Property == UIElement.IsMouseOverProperty);
        True(hover is not null, $"{name} should define a hover trigger");
        var background = hover!.Setters.OfType<Setter>()
            .FirstOrDefault(setter => setter.Property == Border.BackgroundProperty);
        True(background?.TargetName == target, $"{name} hover should tint {target}");
        True(background!.Value is SolidColorBrush brush &&
             brush.Color == Color.FromArgb(0x14, 0, 0, 0),
            $"{name} hover should use the shared translucent black tint");
        True(hover.Setters.OfType<Setter>().All(setter =>
                setter.Property != Control.ForegroundProperty && setter.TargetName != "Host"),
            $"{name} hover should not replace the active fill or icon color");
    }

    private static void PinOutlineAndLightMenuBehaveConsistently(PinWindow pin)
    {
        const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
        var outline = (FrameworkElement)pin.FindName("PinOutline");
        True(outline.Visibility == Visibility.Visible, "pins should show the outer outline by default");
        True(new AppSettings().PinGlowEnabled, "the pin outline setting should default to enabled");

        var menu = pin.ContextMenu;
        True(menu is not null, "pins should keep a context menu");
        var menuStyle = (Style)pin.Resources["PinMenuStyle"];
        var menuTemplate = menuStyle.Setters.OfType<Setter>()
            .Where(setter => setter.Property == Control.TemplateProperty)
            .Select(setter => setter.Value)
            .OfType<ControlTemplate>()
            .First();
        var menuRoot = (Border)menuTemplate.LoadContent();
        var menuBackground = (SolidColorBrush)menuRoot.Background;
        True(menuBackground.Color.A >= 0xF0 &&
             menuBackground.Color.R >= 0xF5 &&
             menuBackground.Color.G >= 0xF5 &&
             menuBackground.Color.B >= 0xF5,
            "the pin menu should use the light HaloCap surface");

        var outlineItem = menu!.Items.OfType<MenuItem>()
            .First(item => item.Header as string == "外描边");
        True(outlineItem.IsChecked, "the outline menu item should start checked");
        var foreground = (SolidColorBrush)outlineItem.Foreground;
        True(foreground.Color.R < 0x40 && foreground.Color.G < 0x40 && foreground.Color.B < 0x40,
            "pin menu text should be dark on the light surface");

        outlineItem.ApplyTemplate();
        outlineItem.Measure(new Size(220, 34));
        outlineItem.Arrange(new Rect(0, 0, 220, 34));
        outlineItem.UpdateLayout();
        var check = (System.Windows.Shapes.Path?)outlineItem.Template.FindName("CheckGlyph", outlineItem);
        True(check is { Visibility: Visibility.Visible },
            "the enabled outline state should show a check glyph");

        var fired = new List<bool>();
        pin.OutlineChanged += fired.Add;
        outlineItem.IsChecked = false;
        typeof(PinWindow).GetMethod("OutlineMenuItem_Click", flags)!
            .Invoke(pin, new object[] { outlineItem, new RoutedEventArgs() });
        True(outline.Visibility == Visibility.Collapsed, "turning off the outline should hide it");
        True(fired.Count == 1 && !fired[0], "the outline toggle should notify the app to persist it");
        outlineItem.UpdateLayout();
        True(check is not null && check.Visibility == Visibility.Collapsed,
            "the unchecked outline item should hide the check glyph");

        var sample = new PinWindow(CreateSolidBitmap(120, 80, Colors.White),
            new PhysicalRect(0, 0, 120, 80), 1, ScreenshotImageFormat.Png);
        try
        {
            var shell = (FrameworkElement)sample.FindName("PinShell");
            shell.Measure(new Size(128, 88));
            shell.Arrange(new Rect(0, 0, 128, 88));
            shell.UpdateLayout();
            var frame = RenderBitmap(shell, 128, 88);
            var stroke = ReadPixel(frame, 1, 44);
            True(stroke.A >= 0x80 && stroke.B > stroke.R + 10,
                "the enabled outline should draw a visible light blue stroke outside the image");
            True(stroke.B > 240 && stroke.R is >= 90 and <= 200,
                "the outer stroke should stay a soft blue rather than a saturated line");
            var imagePixel = ReadPixel(frame, 10, 44);
            True(imagePixel.R > 240 && imagePixel.G > 240 && imagePixel.B > 240,
                "the outer stroke must not paint over the image");

            PinWindow.SetOutlineForAll(false);
            shell.UpdateLayout();
            var plain = RenderBitmap(shell, 128, 88);
            True(ReadPixel(plain, 1, 44).A == 0, "turning the outline off should leave the padding transparent");
            True(ReadPixel(plain, 10, 44).R > 240, "the image should stay untouched when the outline is off");
        }
        finally
        {
            PinWindow.SetOutlineForAll(true);
            sample.Close();
        }

        static BitmapSource RenderBitmap(FrameworkElement element, int width, int height)
        {
            var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
            bitmap.Render(element);
            return bitmap;
        }
    }

    private static void SettingsAndTrayChromeUseCleanLayout(HaloCap.App app)
    {
        foreach (var style in new[] { "PrimaryButtonStyle", "SecondaryButtonStyle", "CloseButtonStyle", "ToggleSwitchStyle" })
        {
            var resource = (Style)app.FindResource(style);
            True(resource.Setters.OfType<Setter>().Any(setter =>
                    setter.Property == FrameworkElement.FocusVisualStyleProperty && setter.Value is null),
                $"{style} should suppress the dashed focus rectangle");
        }

        var settings = new SettingsWindow(HotkeyDefinition.Default, DefaultActionHotkeys(), false, ScreenshotImageFormat.Png,
            RadialMenuSize.Medium, string.Empty, true,
            (_, _) => null, _ => null, _ => null, _ => null, _ => null, _ => null);
        try
        {
            True(settings.FocusVisualStyle is null,
                "the settings window must not draw a dashed focus rectangle around its frame");
            var root = (FrameworkElement)settings.Content;
            root.Measure(new Size(820, 620));
            root.Arrange(new Rect(0, 0, 820, 620));
            root.UpdateLayout();

            var titleBar = (Grid)settings.FindName("WindowTitleBar");
            var close = (Button)settings.FindName("WindowCloseButton");
            var band = (Border)settings.FindName("TitleBarBand");
            True(titleBar.ActualWidth > 700, "settings title bar should span the full window width");
            Equal(50, titleBar.ActualHeight, 0.1);
            True(band.ActualWidth >= titleBar.ActualWidth - 0.1,
                "the title bar background should cover the full width without a left/right split");
            True(((SolidColorBrush)band.Background).Color == Color.FromRgb(0xED, 0xED, 0xF1),
                "the unified title bar should use the sidebar surface color");
            True(band.CornerRadius == new CornerRadius(17, 17, 0, 0),
                "the title bar should keep the rounded top corners");
            True(HasText(root, "光圈"), "the title bar should label the app as 光圈");
            True(!HasText(root, "设置"), "the title bar should no longer show the 设置 label");
            True(HasText(root, "开机自启"), "settings should label auto-start as 开机自启");
            True(!HasText(root, "登录时启动"), "settings should no longer use the old auto-start wording");
            Equal("开机自启", AutomationProperties.GetName((DependencyObject)settings.FindName("AutoStartToggle")!));
            var closePoint = close.TransformToAncestor(titleBar).Transform(new Point());
            True(closePoint.X + close.ActualWidth >= titleBar.ActualWidth - 20,
                "the close button should sit at the top-right edge");
            True(!HasText(root, "本地处理 · 无网络请求"), "settings should no longer show the bottom-left description");
            True(!HasText(root, "✕"), "settings should use the drawn close icon rather than the old text glyph");
            var general = (FrameworkElement)settings.FindName("GeneralPane");
            True(general.TransformToAncestor(root).Transform(new Point()).Y >= 50,
                "settings content should start below the unified title bar");

            Equal("Ctrl + Alt + D", ((TextBlock)settings.FindName("RadialHotkeyValueText")).Text);
            Equal("Ctrl + Alt + A", ((TextBlock)settings.FindName("ScreenshotHotkeyValueText")).Text);
            Equal("Ctrl + Alt + O", ((TextBlock)settings.FindName("OcrHotkeyValueText")).Text);
            Equal("Ctrl + Alt + R", ((TextBlock)settings.FindName("RecordHotkeyValueText")).Text);
            Equal("Ctrl + Alt + P", ((TextBlock)settings.FindName("PinHotkeyValueText")).Text);
            Equal("settings.hotkey.radial", AutomationProperties.GetAutomationId((Button)settings.FindName("RadialHotkeyButton")));
            Equal("settings.hotkey.screenshot", AutomationProperties.GetAutomationId((Button)settings.FindName("ScreenshotHotkeyButton")));
            Equal("settings.hotkey.ocr", AutomationProperties.GetAutomationId((Button)settings.FindName("OcrHotkeyButton")));
            Equal("settings.hotkey.record", AutomationProperties.GetAutomationId((Button)settings.FindName("RecordHotkeyButton")));
            Equal("settings.hotkey.pin", AutomationProperties.GetAutomationId((Button)settings.FindName("PinHotkeyButton")));

            var defaults = new AppSettings();
            Equal("Ctrl + Alt + A", defaults.GetActionHotkey(RadialAction.Screenshot).DisplayText);
            Equal("Ctrl + Alt + O", defaults.GetActionHotkey(RadialAction.Ocr).DisplayText);
            Equal("Ctrl + Alt + R", defaults.GetActionHotkey(RadialAction.Record).DisplayText);
            Equal("Ctrl + Alt + P", defaults.GetActionHotkey(RadialAction.Pin).DisplayText);

            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
            var settingsField = typeof(HaloCap.App).GetField("_settingsWindow", flags)!;
            var previousWindow = settingsField.GetValue(app);
            settingsField.SetValue(app, settings);
            try
            {
                True((bool)typeof(HaloCap.App).GetMethod("CanShowTrayMenu", flags)!.Invoke(app, null)!,
                    "opening settings should not block the tray menu");
                True((bool)typeof(HaloCap.App).GetMethod("CanToggleRadial", flags)!.Invoke(app, null)!,
                    "opening settings should not block summoning the aperture");
                typeof(SettingsWindow).GetMethod("BeginHotkeyCapture", flags)!.Invoke(settings, null);
                True(settings.IsCapturingHotkey, "settings should expose hotkey capture state to the app");
                True(!(bool)typeof(HaloCap.App).GetMethod("CanToggleRadial", flags)!.Invoke(app, null)!,
                    "hotkey capture should suppress the global hotkey briefly");
                True((bool)typeof(HaloCap.App).GetMethod("CanShowTrayMenu", flags)!.Invoke(app, null)!,
                    "hotkey capture should not block the tray menu");
                typeof(SettingsWindow).GetMethod("CancelHotkeyCapture", flags)!.Invoke(settings, null);
                True(!settings.IsCapturingHotkey, "cancelling capture should clear the app guard state");
            }
            finally
            {
                settingsField.SetValue(app, previousWindow);
            }

            var closedCount = 0;
            settings.Closed += (_, _) => closedCount++;
            ((Button)settings.FindName("SaveButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
            Equal(1, closedCount);
        }
        finally
        {
            settings.Close();
        }

        var menu = new TrayMenu("v1.2.4", "Ctrl + Alt + D", DefaultActionHotkeys(),
            autoStart: true, recording: false, pinAvailable: true);
        True(!menu.StaysOpen, "the tray menu should close automatically when clicking outside");
        menu.ApplyTemplate();
        var actionItems = menu.Items.OfType<MenuItem>()
            .Where(item => AutomationProperties.GetAutomationId(item).StartsWith("tray.action.", StringComparison.Ordinal))
            .ToList();
        Equal(4, actionItems.Count);
        Equal("截图", actionItems[0].Header as string);
        Equal("文字识别", actionItems[1].Header as string);
        Equal("录屏", actionItems[2].Header as string);
        Equal("贴图", actionItems[3].Header as string);
        Equal("Ctrl + Alt + A", actionItems[0].InputGestureText);
        Equal("Ctrl + Alt + O", actionItems[1].InputGestureText);
        Equal("Ctrl + Alt + R", actionItems[2].InputGestureText);
        Equal("Ctrl + Alt + P", actionItems[3].InputGestureText);
        True(actionItems.All(item => item.IsEnabled), "all four tray actions should be enabled by default");
        Equal("开机自启", menu.Items.OfType<MenuItem>()
            .First(item => AutomationProperties.GetAutomationId(item) == "tray.item.autoStart").Header as string);

        var selected = new List<RadialAction>();
        menu.ActionRequested += selected.Add;
        actionItems[1].RaiseEvent(new RoutedEventArgs(MenuItem.ClickEvent));
        True(selected.Count == 1 && selected[0] == RadialAction.Ocr,
            "choosing a tray action should raise exactly one action request");
        True(!menu.IsOpen, "choosing an action should close the tray menu");

        var recordingMenu = new TrayMenu("v1.2.4", "Ctrl + Alt + D", DefaultActionHotkeys(),
            autoStart: true, recording: true, pinAvailable: true);
        recordingMenu.ApplyTemplate();
        var recordingItems = recordingMenu.Items.OfType<MenuItem>()
            .Where(item => AutomationProperties.GetAutomationId(item).StartsWith("tray.action.", StringComparison.Ordinal))
            .ToList();
        Equal("停止录制", recordingItems[2].Header as string);
        True(recordingItems[2].IsEnabled, "the record row should stop recording while it is running");
        True(!recordingItems[0].IsEnabled && !recordingItems[1].IsEnabled && !recordingItems[3].IsEnabled,
            "capture actions and pin should stay disabled while recording");
        True(!recordingMenu.Items.OfType<MenuItem>()
                .First(item => AutomationProperties.GetAutomationId(item) == "tray.item.autoStart").IsEnabled,
            "auto-start should stay disabled while recording");

        var emptyClipboardMenu = new TrayMenu("v1.2.4", "Ctrl + Alt + D", DefaultActionHotkeys(),
            autoStart: false, recording: false, pinAvailable: false);
        emptyClipboardMenu.ApplyTemplate();
        var pinRow = emptyClipboardMenu.Items.OfType<MenuItem>()
            .First(item => AutomationProperties.GetAutomationId(item) == "tray.action.pin");
        True(!pinRow.IsEnabled, "pin should be disabled while the clipboard has no pinnable content");
        True(emptyClipboardMenu.Items.OfType<MenuItem>()
                .First(item => AutomationProperties.GetAutomationId(item) == "tray.action.screenshot").IsEnabled,
            "an empty clipboard should not disable screenshots");

        var autoStart = menu.Items.OfType<MenuItem>()
            .First(item => AutomationProperties.GetAutomationId(item) == "tray.item.autoStart");
        True(autoStart.IsChecked, "the tray auto-start item should retain its checked state");
        autoStart.ApplyTemplate();
        autoStart.Measure(new Size(240, 36));
        autoStart.Arrange(new Rect(0, 0, 240, 36));
        autoStart.UpdateLayout();
        var check = FindVisualChildren<System.Windows.Shapes.Path>(autoStart)
            .First(path => AutomationProperties.GetAutomationId(path) == "tray.check");
        True(check.Visibility == Visibility.Visible, "a checked auto-start item should show the tick");
        var checkPoint = check.TransformToAncestor(autoStart).Transform(new Point());
        True(checkPoint.X > autoStart.ActualWidth / 2, "the tick should be placed on the right side");
        var iconPresenter = autoStart.Template.FindName("IconPresenter", autoStart) as ContentPresenter;
        True(iconPresenter is { Visibility: Visibility.Visible },
            "the auto-start icon should remain visible when checked");
        True(!HasText(menu, "本地处理 · 无网络请求"), "tray menu should no longer show the bottom-left description");
        True(HasText(menu, "Esc 关闭"), "tray menu should keep the escape hint");
    }

    private static void OcrSettingsDefaultAndNormalize()
    {
        var defaults = new AppSettings().Ocr;
        True(defaults.Engine == OcrEngineKind.Native, "OCR should default to the native engine");
        True(!defaults.AutoCopy, "OCR should not auto copy by default");
        True(defaults.PinOcrEnabled, "pin OCR should be enabled by default");

        var settings = new OcrSettings { Engine = (OcrEngineKind)99 };
        settings.Normalize();
        True(settings.Engine == OcrEngineKind.Native, "invalid engines should fall back to native");

        var clone = settings.Clone();
        True(clone.ValueEquals(settings), "a clone should compare equal");
        clone.AutoCopy = true;
        True(!settings.ValueEquals(clone), "value equality should include toggles");
    }

    private static void OcrImagePrepareResizesAndComposites()
    {
        RunSta(() =>
        {
            var translucent = new byte[] { 0, 0, 255, 128 };
            OcrImageProcessor.CompositeOverWhite(translucent);
            Equal(127, translucent[0]);
            Equal(127, translucent[1]);
            Equal(255, translucent[2]);
            Equal(255, translucent[3]);

            var source = CreateSolidBitmap(100, 50, Colors.White);
            var prepared = OcrImageProcessor.Prepare(source, 40);
            Equal(40, prepared.Width);
            Equal(20, prepared.Height);
            Equal(40 * 20 * 4, prepared.Pixels.Length);

            var dark = new byte[] { 10, 10, 10, 255 };
            True(OcrImageProcessor.InvertIfDark(dark), "dark images should be inverted for the model engine");
            Equal(245, dark[0]);
            Equal(245, dark[1]);
            Equal(245, dark[2]);
            Equal(255, dark[3]);

            var light = new byte[] { 240, 240, 240, 255 };
            True(!OcrImageProcessor.InvertIfDark(light), "light images should stay unchanged");
            Equal(240, light[0]);

            var pixels = new byte[2 * 2 * 4];
            SetBgra(pixels, 0, 0, 0, 255, 255);
            SetBgra(pixels, 4, 255, 0, 0, 255);
            SetBgra(pixels, 8, 0, 255, 0, 255);
            SetBgra(pixels, 12, 255, 255, 255, 255);

            var resized = OcrImageProcessor.ResizeBilinear(pixels, 2, 2, 4, 4);
            Equal(4 * 4 * 4, resized.Length);
            Equal(0, resized[0]);
            Equal(0, resized[1]);
            Equal(255, resized[2]);
            Equal(255, resized[12]);
            Equal(0, resized[13]);
            Equal(0, resized[14]);
        });
    }

    private static void OcrResultWindowExposesControls()
    {
        RunSta(() =>
        {
            var monitor = new ScreenMonitor
            {
                Bounds = new PhysicalRect(0, 0, 1920, 1080),
                WorkArea = new PhysicalRect(0, 0, 1920, 1040),
                Scale = 1.0
            };
            var window = new OcrResultWindow(
                CreateSolidBitmap(40, 30, Colors.White),
                new PhysicalRect(20, 20, 40, 30),
                monitor,
                new OcrService(),
                new OcrSettings { Engine = OcrEngineKind.Model },
                OcrImageSource.Selection);

            try
            {
                var textBox = (TextBox)window.FindName("ResultTextBox");
                True(textBox.IsReadOnly, "OCR result text should be selectable but read-only");
                True(textBox.AcceptsReturn, "OCR result text should preserve line breaks");
                True(textBox.IsInactiveSelectionHighlightEnabled,
                    "search matches should stay visible while the search box has focus");
                True(((RadioButton)window.FindName("ModelEngineRadio")).IsChecked == true,
                    "the result window should reflect the configured OCR engine");
                True(((FrameworkElement)window.FindName("SearchBar")).Visibility == Visibility.Collapsed,
                    "search should start collapsed");
                True(((FrameworkElement)window.FindName("ThumbnailCard")).Visibility == Visibility.Collapsed,
                    "the thumbnail should start collapsed");
                True(((Button)window.FindName("RetryButton")).IsEnabled == false,
                    "retry should stay disabled before the first result");
            }
            finally
            {
                window.Close();
            }
        });
    }

    private static void OcrResultWindowSearchesAndSwitchesViews()
    {
        RunSta(() =>
        {
            const BindingFlags flags = BindingFlags.Instance | BindingFlags.NonPublic;
            var monitor = new ScreenMonitor
            {
                Bounds = new PhysicalRect(0, 0, 1920, 1080),
                WorkArea = new PhysicalRect(0, 0, 1920, 1040),
                Scale = 1.0
            };
            var window = new OcrResultWindow(
                CreateSolidBitmap(60, 40, Colors.White),
                new PhysicalRect(20, 20, 60, 40),
                monitor,
                new OcrService(),
                new OcrSettings { Engine = OcrEngineKind.Native },
                OcrImageSource.Selection);

            try
            {
                var lines = new[] { "Alpha Beta", "Gamma" };
                var outcome = new OcrOutcome
                {
                    Status = OcrStatus.Success,
                    Lines = lines,
                    Text = string.Join(Environment.NewLine, lines),
                    EngineLabel = "PP-OCRv6 tiny",
                    Engine = OcrEngineKind.Model,
                    ElapsedMilliseconds = 42
                };

                typeof(OcrResultWindow).GetMethod("ApplyOutcome", flags)!
                    .Invoke(window, new object[] { outcome });

                True(((TextBox)window.FindName("ResultTextBox")).Text == outcome.Text,
                    "the result window should show the recognized lines");
                True(((TextBlock)window.FindName("EngineText")).Text == "PP-OCRv6 tiny",
                    "the result window should show the engine that produced the result");
                True(((Button)window.FindName("CopyAllButton")).IsEnabled,
                    "copy all should become available after a successful result");

                typeof(OcrResultWindow).GetMethod("OpenSearch", flags)!.Invoke(window, null);
                var searchBox = (TextBox)window.FindName("SearchBox");
                searchBox.Text = "a";
                Equal("1/5", ((TextBlock)window.FindName("SearchStatusText")).Text);
                typeof(OcrResultWindow).GetMethod("NavigateSearch", flags)!
                    .Invoke(window, new object[] { true });
                Equal("2/5", ((TextBlock)window.FindName("SearchStatusText")).Text);

                for (var index = 0; index < 4; index++)
                {
                    typeof(OcrResultWindow).GetMethod("NavigateSearch", flags)!
                        .Invoke(window, new object[] { true });
                }

                Equal("1/5", ((TextBlock)window.FindName("SearchStatusText")).Text);

                ((Button)window.FindName("ViewToggleButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));
                True(((FrameworkElement)window.FindName("LinesCard")).Visibility == Visibility.Visible,
                    "the line view should become visible");
                Equal(2, ((StackPanel)window.FindName("LinesPanel")).Children.Count);
                True(((Button)window.FindName("ViewToggleButton")).Content as string == "文本",
                    "the toggle should offer switching back to text");

                var thumbnail = (ToggleButton)window.FindName("ThumbnailButton");
                thumbnail.IsChecked = true;
                True(((FrameworkElement)window.FindName("ThumbnailCard")).Visibility == Visibility.Visible,
                    "the thumbnail card should follow its toggle");

                var root = (Grid)window.FindName("Root");
                root.Measure(new Size(560, double.PositiveInfinity));
                True(root.DesiredSize.Height <= 660,
                    $"the full result window should fit within its max height, got {root.DesiredSize.Height:0.#}");

                var failedOutcome = new OcrOutcome
                {
                    Status = OcrStatus.Failed,
                    Message = "test failure",
                    Engine = OcrEngineKind.Native,
                    EngineLabel = "Windows 原生"
                };
                typeof(OcrResultWindow).GetMethod("ApplyOutcome", flags)!
                    .Invoke(window, new object[] { failedOutcome });
                True(((FrameworkElement)window.FindName("EmptyState")).Visibility == Visibility.Visible,
                    "failed OCR should show the empty state");
                True(((Button)window.FindName("EmptyRetryButton")).Visibility == Visibility.Visible,
                    "failed OCR should offer switching to the other engine");
                True(((Button)window.FindName("RetryButton")).IsEnabled,
                    "retry should be available after a failed result");

                root.Measure(new Size(560, double.PositiveInfinity));
                True(root.DesiredSize.Height <= 660,
                    $"the failed result window should fit within its max height, got {root.DesiredSize.Height:0.#}");
            }
            finally
            {
                window.Close();
            }
        });
    }

    private static void OcrSettingsWindowBindsAndSaves()
    {
        RunSta(() =>
        {
            OcrSettings? applied = null;
            var initial = new OcrSettings
            {
                Engine = OcrEngineKind.Model,
                AutoCopy = true,
                PinOcrEnabled = false
            };

            var window = new SettingsWindow(
                HotkeyDefinition.Default,
                DefaultActionHotkeys(),
                autoStartEnabled: false,
                ScreenshotImageFormat.Png,
                RadialMenuSize.Medium,
                string.Empty,
                convertFilePathsToImages: true,
                (_, _) => null,
                _ => null,
                _ => null,
                _ => null,
                _ => null,
                _ => null,
                initial,
                settings =>
                {
                    applied = settings;
                    return null;
                });

            try
            {
                True(((RadioButton)window.FindName("OcrModelRadio")).IsChecked == true,
                    "the settings window should show the current OCR engine");
                True(((ToggleButton)window.FindName("OcrAutoCopyToggle")).IsChecked == true,
                    "the settings window should show auto copy");
                True(((ToggleButton)window.FindName("PinOcrToggle")).IsChecked == false,
                    "the settings window should show pin OCR availability");

                ((RadioButton)window.FindName("NavOcr")).IsChecked = true;
                True(((FrameworkElement)window.FindName("OcrPane")).Visibility == Visibility.Visible,
                    "selecting the OCR navigation item should show the OCR pane");

                ((RadioButton)window.FindName("OcrNativeRadio")).IsChecked = true;
                ((ToggleButton)window.FindName("PinOcrToggle")).IsChecked = true;
                ((Button)window.FindName("SaveButton")).RaiseEvent(new RoutedEventArgs(Button.ClickEvent));

                True(applied is not null, "saving settings should hand the OCR values back to the app");
                True(applied!.Engine == OcrEngineKind.Native, "the new engine should be saved");
                True(applied.PinOcrEnabled, "the new pin OCR flag should be saved");
                True(applied.AutoCopy, "OCR toggles should round trip");
            }
            finally
            {
                window.Close();
            }
        });
    }

    private static void PinOcrUsesOriginalImage()
    {
        {
            var image = CreateSolidBitmap(80, 60, Colors.White);
            var pin = new PinWindow(
                image,
                new PhysicalRect(20, 30, 80, 60),
                1.0,
                ScreenshotImageFormat.Png,
                outlineEnabled: true,
                ocrEnabled: true);
            var disabled = new PinWindow(
                CreateSolidBitmap(40, 30, Colors.White),
                new PhysicalRect(0, 0, 40, 30),
                1.0,
                ScreenshotImageFormat.Png,
                outlineEnabled: true,
                ocrEnabled: false);

            try
            {
                True(((FrameworkElement)pin.FindName("OcrMenuItem")).Visibility == Visibility.Visible,
                    "pin OCR should be visible when enabled");
                var fired = 0;
                pin.OcrRequested += _ => fired++;
                typeof(PinWindow)
                    .GetMethod("RequestOcr", BindingFlags.Instance | BindingFlags.NonPublic)!
                    .Invoke(pin, null);

                Equal(1, fired);
                True(ReferenceEquals(image, pin.OcrImage),
                    "pin OCR should use the original image instead of a composited annotation image");

                True(((FrameworkElement)disabled.FindName("OcrMenuItem")).Visibility == Visibility.Collapsed,
                    "pin OCR should hide when disabled");
                var disabledFired = 0;
                disabled.OcrRequested += _ => disabledFired++;
                typeof(PinWindow)
                    .GetMethod("RequestOcr", BindingFlags.Instance | BindingFlags.NonPublic)!
                    .Invoke(disabled, null);
                Equal(0, disabledFired);
            }
            finally
            {
                pin.Close();
                disabled.Close();
            }
        }
    }

    private static void OcrNativeEngineRecognizesRenderedText()
    {
        RunSta(() =>
        {
            var image = RenderOcrText("HaloCap 2026");
            using var service = new OcrService();
            var outcome = service.RecognizeAsync(
                    image,
                    new OcrSettings { Engine = OcrEngineKind.Native },
                    OcrEngineKind.Native)
                .GetAwaiter()
                .GetResult();

            if (outcome.Status == OcrStatus.NoLanguage)
            {
                Console.WriteLine("      skipped: no Windows OCR language installed");
                return;
            }

            True(outcome.Status != OcrStatus.Failed,
                "native OCR failed: " + outcome.Message);
            True(outcome.Engine == OcrEngineKind.Native && outcome.EngineLabel == "Windows 原生",
                "native OCR should report the native engine");
            True(outcome.Status == OcrStatus.Success,
                "native OCR should recognize rendered text, status=" + outcome.Status + " message=" + outcome.Message);
            True(outcome.Text.Contains("2026", StringComparison.Ordinal),
                "native OCR should contain the rendered digits, got: " +
                outcome.Text.Replace(Environment.NewLine, " / "));
            Console.WriteLine($"      native OCR in {outcome.ElapsedMilliseconds} ms");
        });
    }

    private static void OcrModelEngineRecognizesRenderedText()
    {
        RunSta(() =>
        {
            var image = RenderOcrText("HaloCap 2026 你好世界");
            using var service = new OcrService();
            var outcome = service.RecognizeAsync(
                    image,
                    new OcrSettings { Engine = OcrEngineKind.Model },
                    OcrEngineKind.Model)
                .GetAwaiter()
                .GetResult();

            True(outcome.Status != OcrStatus.Failed,
                "model OCR failed: " + outcome.Message);
            True(outcome.Engine == OcrEngineKind.Model &&
                 outcome.EngineLabel.StartsWith("PP-OCRv6", StringComparison.Ordinal),
                "model OCR should report the bundled model");
            True(outcome.Status == OcrStatus.Success &&
                 outcome.Lines.Count > 0,
                "model OCR should recognize rendered text, status=" + outcome.Status + " message=" + outcome.Message);
            Console.WriteLine($"      model OCR in {outcome.ElapsedMilliseconds} ms: " +
                              outcome.Text.Replace(Environment.NewLine, " / "));
            True(outcome.Text.Contains("2026", StringComparison.Ordinal) ||
                 outcome.Text.Contains("Halo", StringComparison.OrdinalIgnoreCase),
                "model OCR should contain the rendered text, got: " +
                outcome.Text.Replace(Environment.NewLine, " / "));
        });
    }

    private static void OcrModelEngineHandlesDarkBackground()
    {
        RunSta(() =>
        {
            var background = new SolidColorBrush(Color.FromRgb(0x1E, 0x1E, 0x1E));
            background.Freeze();
            var image = RenderOcrText("HaloCap 2026 你好世界", background, Brushes.White);
            using var service = new OcrService();
            var outcome = service.RecognizeAsync(
                    image,
                    new OcrSettings { Engine = OcrEngineKind.Model },
                    OcrEngineKind.Model)
                .GetAwaiter()
                .GetResult();

            True(outcome.Status == OcrStatus.Success,
                "model OCR should handle dark backgrounds, status=" + outcome.Status + " message=" + outcome.Message);
            True(outcome.Text.Contains("2026", StringComparison.Ordinal) ||
                 outcome.Text.Contains("Halo", StringComparison.OrdinalIgnoreCase),
                "dark background OCR should contain the rendered text, got: " +
                outcome.Text.Replace(Environment.NewLine, " / "));
        });
    }

    private static void SetBgra(byte[] pixels, int offset, byte blue, byte green, byte red, byte alpha)
    {
        pixels[offset] = blue;
        pixels[offset + 1] = green;
        pixels[offset + 2] = red;
        pixels[offset + 3] = alpha;
    }

    private static BitmapSource RenderOcrText(
        string text,
        Brush? background = null,
        Brush? foreground = null)
    {
        var formatted = new FormattedText(
            text,
            CultureInfo.InvariantCulture,
            FlowDirection.LeftToRight,
            new Typeface("Segoe UI"),
            48,
            foreground ?? Brushes.Black,
            1.0);
        var width = (int)Math.Ceiling(formatted.WidthIncludingTrailingWhitespace) + 60;
        var height = 120;
        var visual = new DrawingVisual();

        using (var context = visual.RenderOpen())
        {
            context.DrawRectangle(background ?? Brushes.White, null, new Rect(0, 0, width, height));
            context.DrawText(formatted, new Point(30, 28));
        }

        var bitmap = new RenderTargetBitmap(width, height, 96, 96, PixelFormats.Pbgra32);
        bitmap.Render(visual);
        bitmap.Freeze();
        return bitmap;
    }

    private static Dictionary<RadialAction, HotkeyDefinition> DefaultActionHotkeys()
    {
        var settings = new AppSettings();
        return new[] { RadialAction.Screenshot, RadialAction.Ocr, RadialAction.Record, RadialAction.Pin }
            .ToDictionary(action => action, action => settings.GetActionHotkey(action).Clone());
    }

    private static bool HasText(DependencyObject root, string text) =>
        FindVisualChildren<TextBlock>(root).Any(block => block.Text == text);

    private static IEnumerable<T> FindVisualChildren<T>(DependencyObject root) where T : DependencyObject
    {
        for (var index = 0; index < VisualTreeHelper.GetChildrenCount(root); index++)
        {
            var child = VisualTreeHelper.GetChild(root, index);
            if (child is T match)
            {
                yield return match;
            }

            foreach (var descendant in FindVisualChildren<T>(child))
            {
                yield return descendant;
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
