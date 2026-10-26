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

        AnnotationRenderer.Draw(drawingContext, _strokes, _scale);

        if (_live is not null)
        {
            AnnotationRenderer.Draw(drawingContext, new[] { _live }, _scale);
        }
    }
}
