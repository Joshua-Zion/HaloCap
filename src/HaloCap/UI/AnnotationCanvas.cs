using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Media;
using HaloCap.Models;
using HaloCap.Services;

namespace HaloCap.UI;

/// <summary>
/// 按图片 DIP 坐标绘制标注的轻量画布，屏幕预览时通过 scale 放大或缩小。
/// </summary>
public sealed class AnnotationCanvas : FrameworkElement
{
    private IReadOnlyList<AnnotationStroke> _strokes = Array.Empty<AnnotationStroke>();
    private AnnotationStroke? _live;
    private double _scale = 1.0;
    private double _originX;
    private double _originY;

    public AnnotationCanvas()
    {
        RenderOptions.SetBitmapScalingMode(this, BitmapScalingMode.NearestNeighbor);
    }

    internal void SetOrigin(double x, double y)
    {
        if (Math.Abs(_originX - x) < 0.01 && Math.Abs(_originY - y) < 0.01)
        {
            return;
        }

        _originX = x;
        _originY = y;
        InvalidateVisual();
    }

    internal void SetContent(
        IReadOnlyList<AnnotationStroke> strokes,
        AnnotationStroke? live,
        double scale)
    {
        _strokes = strokes;
        _live = live;
        _scale = scale;
        InvalidateVisual();
    }

    protected override void OnRender(DrawingContext drawingContext)
    {
        base.OnRender(drawingContext);

        var hasOrigin = _originX != 0 || _originY != 0;

        if (hasOrigin)
        {
            drawingContext.PushTransform(new TranslateTransform(_originX, _originY));
        }

        AnnotationRenderer.Draw(drawingContext, _strokes, _scale);

        if (_live is not null)
        {
            AnnotationRenderer.Draw(drawingContext, new[] { _live }, _scale);
        }

        if (hasOrigin)
        {
            drawingContext.Pop();
        }
    }
}
