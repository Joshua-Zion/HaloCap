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

    private static readonly FontFamily TextFontFamily = new("Microsoft YaHei UI, Segoe UI");

    private static readonly Typeface StepTypeface = new(
        TextFontFamily,
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

        if (stroke.Tool == AnnotationTool.Eraser)
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

            var typeface = new Typeface(
                TextFontFamily,
                stroke.Italic ? FontStyles.Italic : FontStyles.Normal,
                stroke.Bold ? FontWeights.Bold : FontWeights.Normal,
                FontStretches.Normal);

            var formatted = new FormattedText(
                stroke.Text,
                CultureInfo.CurrentUICulture,
                FlowDirection.LeftToRight,
                typeface,
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
            // 整条几何一次描边并放在不透明组内统一调透明度，
            // 避免分段半透明叠加导致快速滑动时出现深色圆点。
            var group = new DrawingGroup { Opacity = 0.42 };
            using (var groupContext = group.Open())
            {
                var highlighter = CreatePen(
                    color,
                    Math.Max(2.0, stroke.Thickness * scale));
                DrawPolyline(groupContext, stroke.Points, highlighter);
            }

            context.DrawDrawing(group);
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
                var angle = Math.Atan2(vector.Y, vector.X);

                if (stroke.ArrowHead is ArrowHeadMode.End or ArrowHeadMode.Both)
                {
                    DrawArrowHead(context, pen, end, angle, stroke, scale);
                }

                if (stroke.ArrowHead == ArrowHeadMode.Both)
                {
                    DrawArrowHead(context, pen, start, angle + Math.PI, stroke, scale);
                }

                break;
            }
        }
    }

    private static void DrawArrowHead(
        DrawingContext context,
        Pen pen,
        Point tip,
        double angle,
        AnnotationStroke stroke,
        double scale)
    {
        var head = Math.Max(8 * scale, stroke.Thickness * scale * 3.2);

        for (var direction = -1; direction <= 1; direction += 2)
        {
            var wingAngle = angle + Math.PI + (direction * ArrowHeadAngle);
            var wing = new Point(
                tip.X + (head * Math.Cos(wingAngle)),
                tip.Y + (head * Math.Sin(wingAngle)));
            context.DrawLine(pen, tip, wing);
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
            StepTypeface,
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
        if (points.Count == 0)
        {
            return;
        }

        if (points.Count == 1)
        {
            var radius = Math.Max(0.6, pen.Thickness / 2);
            context.DrawEllipse(pen.Brush, null, points[0], radius, radius);
            return;
        }

        var geometry = new StreamGeometry();

        using (var geometryContext = geometry.Open())
        {
            geometryContext.BeginFigure(points[0], false, false);

            var rest = new Point[points.Count - 1];

            for (var i = 1; i < points.Count; i++)
            {
                rest[i - 1] = points[i];
            }

            geometryContext.PolyLineTo(rest, true, true);
        }

        geometry.Freeze();
        context.DrawGeometry(null, pen, geometry);
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
