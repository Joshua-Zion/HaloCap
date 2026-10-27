using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using HaloCap.Models;

namespace HaloCap.Services;

internal static class AnnotationRenderer
{
    private const double ArrowHeadAngle = Math.PI / 7.0;

    private static readonly Typeface TextTypeface = new(
        new FontFamily("Microsoft YaHei UI, Segoe UI"),
        FontStyles.Normal,
        FontWeights.SemiBold,
        FontStretches.Normal);

    /// <summary>
    /// 在给定坐标空间中绘制标注；坐标以图片 DIP 为单位，scale 用于屏幕预览时缩放。
    /// </summary>
    public static void Draw(
        DrawingContext drawingContext,
        IReadOnlyList<AnnotationStroke> strokes,
        double scale)
    {
        if (scale <= 0.0001)
        {
            return;
        }

        foreach (var stroke in strokes)
        {
            DrawStroke(drawingContext, stroke, scale);
        }
    }

    public static BitmapSource Compose(BitmapSource source, IReadOnlyList<AnnotationStroke> strokes)
    {
        if (strokes.Count == 0)
        {
            return source;
        }

        var dpiX = source.DpiX > 0.1 ? source.DpiX : 96.0;
        var dpiY = source.DpiY > 0.1 ? source.DpiY : 96.0;
        var width = source.PixelWidth * 96.0 / dpiX;
        var height = source.PixelHeight * 96.0 / dpiY;

        var visual = new DrawingVisual();
        RenderOptions.SetBitmapScalingMode(visual, BitmapScalingMode.NearestNeighbor);

        using (var context = visual.RenderOpen())
        {
            context.DrawImage(source, new Rect(0, 0, width, height));
            Draw(context, strokes, 1.0);
        }

        var target = new RenderTargetBitmap(
            source.PixelWidth,
            source.PixelHeight,
            dpiX,
            dpiY,
            PixelFormats.Pbgra32);

        target.Render(visual);
        target.Freeze();
        return target;
    }

    private static void DrawStroke(DrawingContext context, AnnotationStroke stroke, double scale)
    {
        if (stroke.Points.Count == 0)
        {
            return;
        }

        var color = stroke.Color;

        if (stroke.Tool == AnnotationTool.Text)
        {
            if (string.IsNullOrWhiteSpace(stroke.Text))
            {
                return;
            }

            var textBrush = new SolidColorBrush(color);
            textBrush.Freeze();

            var formatted = new FormattedText(
                stroke.Text,
                CultureInfo.CurrentUICulture,
                FlowDirection.LeftToRight,
                TextTypeface,
                stroke.TextSize * scale,
                textBrush,
                1.0);

            context.DrawText(formatted, stroke.Points[0]);
            return;
        }

        if (stroke.Tool == AnnotationTool.Step)
        {
            DrawStep(context, stroke, scale);
            return;
        }

        if (stroke.Tool == AnnotationTool.Mosaic)
        {
            if (stroke.Mosaic is not null && stroke.Points.Count >= 2)
            {
                context.DrawImage(
                    stroke.Mosaic,
                    AnnotationGeometry.NormalizeRect(stroke.Points[0], stroke.Points[1]));
            }

            return;
        }

        if (stroke.Tool == AnnotationTool.Highlighter)
        {
            var fill = new SolidColorBrush(Color.FromArgb(0x66, color.R, color.G, color.B));
            fill.Freeze();
            var highlighter = new Pen(fill, Math.Max(2.0, AnnotationDefaults.HighlighterThickness * scale))
            {
                StartLineCap = PenLineCap.Round,
                EndLineCap = PenLineCap.Round,
                LineJoin = PenLineJoin.Round
            };

            DrawPolyline(context, stroke.Points, highlighter);
            return;
        }

        if (stroke.Tool == AnnotationTool.Pen)
        {
            DrawPolyline(context, stroke.Points, CreatePen(color, stroke.Thickness * scale));
            return;
        }

        if (stroke.Points.Count < 2)
        {
            return;
        }

        var start = stroke.Points[0];
        var end = stroke.Points[stroke.Points.Count - 1];
        var pen = CreatePen(color, stroke.Thickness * scale);

        switch (stroke.Tool)
        {
            case AnnotationTool.Rectangle:
                context.DrawRectangle(
                    stroke.Filled ? CreateFill(color) : null,
                    pen,
                    new Rect(start, end));
                break;

            case AnnotationTool.Ellipse:
            {
                var rect = new Rect(start, end);
                context.DrawEllipse(
                    stroke.Filled ? CreateFill(color) : null,
                    pen,
                    new Point(rect.Left + (rect.Width / 2), rect.Top + (rect.Height / 2)),
                    rect.Width / 2,
                    rect.Height / 2);
                break;
            }

            case AnnotationTool.Arrow:
            {
                var vector = end - start;
                if (vector.Length < 0.5)
                {
                    break;
                }

                context.DrawLine(pen, start, end);

                var head = Math.Max(8 * scale, stroke.Thickness * scale * 3.2);
                var angle = Math.Atan2(vector.Y, vector.X);

                for (var i = 1; i <= 2; i++)
                {
                    var wingAngle = angle + Math.PI - (i * ArrowHeadAngle);
                    var wing = new Point(
                        end.X + (head * Math.Cos(wingAngle)),
                        end.Y + (head * Math.Sin(wingAngle)));
                    context.DrawLine(pen, end, wing);
                }

                break;
            }
        }
    }

    private static void DrawStep(DrawingContext context, AnnotationStroke stroke, double scale)
    {
        if (stroke.Points.Count == 0)
        {
            return;
        }

        var center = stroke.Points[0];
        var radius = AnnotationDefaults.StepRadius * scale;
        var inkColor = AnnotationGeometry.IsLightColor(stroke.Color)
            ? Color.FromRgb(0x1B, 0x1F, 0x24)
            : Colors.White;

        var fill = new SolidColorBrush(stroke.Color);
        fill.Freeze();
        var ink = new SolidColorBrush(inkColor);
        ink.Freeze();
        var outline = new Pen(ink, Math.Max(1.0, 1.2 * scale));

        context.DrawEllipse(fill, outline, center, radius, radius);

        var formatted = new FormattedText(
            stroke.Text ?? "?",
            CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight,
            TextTypeface,
            13.0 * scale,
            ink,
            1.0);

        context.DrawText(
            formatted,
            new Point(center.X - (formatted.Width / 2), center.Y - (formatted.Height / 2)));
    }

    private static void DrawPolyline(
        DrawingContext context,
        IReadOnlyList<Point> points,
        Pen pen)
    {
        if (points.Count == 1)
        {
            var radius = Math.Max(0.6, pen.Thickness / 2);
            context.DrawEllipse(pen.Brush, null, points[0], radius, radius);
            return;
        }

        for (var i = 1; i < points.Count; i++)
        {
            context.DrawLine(pen, points[i - 1], points[i]);
        }
    }

    private static SolidColorBrush CreateFill(Color color)
    {
        var fill = new SolidColorBrush(color);
        fill.Freeze();
        return fill;
    }

    private static Pen CreatePen(Color color, double thickness)
    {
        var brush = new SolidColorBrush(color);
        brush.Freeze();

        var pen = new Pen(brush, Math.Max(0.4, thickness))
        {
            StartLineCap = PenLineCap.Round,
            EndLineCap = PenLineCap.Round,
            LineJoin = PenLineJoin.Round
        };

        return pen;
    }
}
