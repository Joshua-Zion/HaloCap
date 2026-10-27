using System;
using System.Collections.Generic;
using System.Globalization;
using System.Windows;
using System.Windows.Media;
using HaloCap.Models;

namespace HaloCap.Services;

internal static class AnnotationHitTester
{
    private static readonly FontFamily TextFontFamily = new("Microsoft YaHei UI, Segoe UI");

    public static bool HitTest(AnnotationStroke stroke, Point point, double radius)
    {
        radius = Math.Max(0, radius);

        switch (stroke.Tool)
        {
            case AnnotationTool.Eraser:
                return false;

            case AnnotationTool.Step:
                return stroke.Points.Count > 0 &&
                       (stroke.Points[0] - point).Length <= AnnotationDefaults.StepRadius + radius;

            case AnnotationTool.Text:
                return HitText(stroke, point, radius);

            case AnnotationTool.Mosaic:
                return stroke.Points.Count >= 2 &&
                       Inflate(AnnotationGeometry.NormalizeRect(stroke.Points[0], stroke.Points[1]), radius)
                           .Contains(point);

            case AnnotationTool.Pen:
            case AnnotationTool.Highlighter:
                return NearPolyline(
                    stroke.Points,
                    point,
                    radius + (EffectiveThickness(stroke) / 2));

            case AnnotationTool.Arrow:
                return stroke.Points.Count >= 2 &&
                       NearSegment(
                           stroke.Points[0],
                           stroke.Points[stroke.Points.Count - 1],
                           point,
                           radius + (EffectiveThickness(stroke) / 2));

            case AnnotationTool.Rectangle:
            case AnnotationTool.Ellipse:
                return HitShape(stroke, point, radius);

            default:
                return false;
        }
    }

    private static bool HitText(AnnotationStroke stroke, Point point, double radius)
    {
        if (stroke.Points.Count == 0)
        {
            return false;
        }

        var typeface = new Typeface(
            TextFontFamily,
            stroke.Italic ? FontStyles.Italic : FontStyles.Normal,
            stroke.Bold ? FontWeights.Bold : FontWeights.Normal,
            FontStretches.Normal);

        var formatted = new FormattedText(
            string.IsNullOrEmpty(stroke.Text) ? " " : stroke.Text,
            CultureInfo.CurrentUICulture,
            FlowDirection.LeftToRight,
            typeface,
            stroke.TextSize,
            Brushes.Black,
            1.0);

        var origin = stroke.Points[0];
        var bounds = new Rect(
            origin.X,
            origin.Y,
            Math.Max(1, formatted.Width),
            Math.Max(1, formatted.Height));

        return Inflate(bounds, radius).Contains(point);
    }

    private static bool HitShape(AnnotationStroke stroke, Point point, double radius)
    {
        if (stroke.Points.Count < 2)
        {
            return false;
        }

        var start = stroke.Points[0];
        var end = stroke.Points[stroke.Points.Count - 1];
        var rect = new Rect(start, end);

        if (rect.Width < 0.5 || rect.Height < 0.5)
        {
            return false;
        }

        Geometry geometry = stroke.Tool == AnnotationTool.Rectangle
            ? new RectangleGeometry(rect)
            : new EllipseGeometry(
                new Point(rect.Left + (rect.Width / 2), rect.Top + (rect.Height / 2)),
                rect.Width / 2,
                rect.Height / 2);

        var pen = new Pen(Brushes.Black, Math.Max(1, stroke.Thickness + (2 * radius)));

        if (geometry.StrokeContains(pen, point))
        {
            return true;
        }

        return stroke.Filled && geometry.FillContains(point);
    }

    private static double EffectiveThickness(AnnotationStroke stroke)
    {
        var thickness = stroke.Tool == AnnotationTool.Highlighter
            ? Math.Max(2.0, stroke.Thickness)
            : stroke.Thickness;

        return Math.Max(0.5, thickness);
    }

    private static bool NearPolyline(IReadOnlyList<Point> points, Point point, double threshold)
    {
        if (points.Count == 0)
        {
            return false;
        }

        if (points.Count == 1)
        {
            return (points[0] - point).Length <= threshold;
        }

        for (var i = 1; i < points.Count; i++)
        {
            if (NearSegment(points[i - 1], points[i], point, threshold))
            {
                return true;
            }
        }

        return false;
    }

    private static bool NearSegment(Point start, Point end, Point point, double threshold)
    {
        var dx = end.X - start.X;
        var dy = end.Y - start.Y;
        var lengthSquared = (dx * dx) + (dy * dy);

        if (lengthSquared < 0.0001)
        {
            return (point - start).Length <= threshold;
        }

        var t = Math.Clamp(
            (((point.X - start.X) * dx) + ((point.Y - start.Y) * dy)) / lengthSquared,
            0,
            1);

        var projection = new Point(start.X + (t * dx), start.Y + (t * dy));
        return (point - projection).Length <= threshold;
    }

    private static Rect Inflate(Rect rect, double radius) => new(
        rect.X - radius,
        rect.Y - radius,
        rect.Width + (2 * radius),
        rect.Height + (2 * radius));
}
