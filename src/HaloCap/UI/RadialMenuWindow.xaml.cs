using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using HaloCap.Interop;
using HaloCap.Models;

namespace HaloCap.UI;

public partial class RadialMenuWindow : Window
{
    public const double MenuSize = 380;
    public const double DiskRadius = 170;
    public const double DeadZoneRadius = 52;

    /// <summary>
    /// 窗口在缩放后的设计画布之外额外保留的透明边距（DIP），
    /// 用于容纳投影，避免小尺寸时光圈与阴影被窗口边缘裁切。
    /// </summary>
    public const double MenuPadding = 20;

    private const double CenterPoint = MenuSize / 2;
    private const double NodeRadius = 110;
    private const double SectorInnerRadius = 56;
    private const double SectorOuterRadius = 168;
    private const double SectorGapDegrees = 3.6;

    private readonly List<SectorVisual> _sectors = new();
    private readonly Dictionary<RadialAction, RadialItemView> _items = new();
    private int _hoveredIndex = -1;
    private int _animationVersion;
    private double _sizeScale = 1;

    public RadialMenuWindow()
    {
        InitializeComponent();

        SourceInitialized += (_, _) => ApplyWindowStyles();
        BuildSectorVisuals();
        BuildActionItems();

        new WindowInteropHelper(this).EnsureHandle();
    }

    public IntPtr Handle => new WindowInteropHelper(this).Handle;

    public static double GetWindowSizeDip(RadialMenuSize size) => size switch
    {
        RadialMenuSize.Small => 360,
        RadialMenuSize.Medium => 420,
        RadialMenuSize.Large => 480,
        _ => MenuSize + (2 * MenuPadding)
    };

    public static double GetContentScale(RadialMenuSize size) =>
        (GetWindowSizeDip(size) - (2 * MenuPadding)) / MenuSize;

    public void ApplySize(RadialMenuSize size)
    {
        var windowSize = GetWindowSizeDip(size);
        Width = windowSize;
        Height = windowSize;
        _sizeScale = GetContentScale(size);

        if (!IsVisible)
        {
            RootScale.ScaleX = _sizeScale;
            RootScale.ScaleY = _sizeScale;
        }
    }

    public void EnsureShownInvisible()
    {
        _animationVersion++;
        BeginAnimation(OpacityProperty, null);
        RootScale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
        RootScale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
        RootScale.ScaleX = 0.93 * _sizeScale;
        RootScale.ScaleY = 0.93 * _sizeScale;
        Opacity = 0;
        ResetHover();

        if (!IsVisible)
        {
            Show();
        }
    }

    public void PlayShowAnimation()
    {
        _animationVersion++;
        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };

        BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(150))
            {
                EasingFunction = ease
            });

        RootScale.BeginAnimation(
            ScaleTransform.ScaleXProperty,
            new DoubleAnimation(0.93 * _sizeScale, _sizeScale, TimeSpan.FromMilliseconds(160))
            {
                EasingFunction = ease
            });

        RootScale.BeginAnimation(
            ScaleTransform.ScaleYProperty,
            new DoubleAnimation(0.93 * _sizeScale, _sizeScale, TimeSpan.FromMilliseconds(160))
            {
                EasingFunction = ease
            });
    }

    public void PlayHideAnimation()
    {
        if (!IsVisible)
        {
            return;
        }

        var version = ++_animationVersion;
        var ease = new CubicEase { EasingMode = EasingMode.EaseIn };
        var fade = new DoubleAnimation(Opacity, 0, TimeSpan.FromMilliseconds(120))
        {
            EasingFunction = ease
        };

        fade.Completed += (_, _) =>
        {
            if (version != _animationVersion)
            {
                return;
            }

            Hide();
            BeginAnimation(OpacityProperty, null);
            RootScale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            RootScale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            Opacity = 0;
            RootScale.ScaleX = _sizeScale;
            RootScale.ScaleY = _sizeScale;
        };

        BeginAnimation(OpacityProperty, fade);
        RootScale.BeginAnimation(
            ScaleTransform.ScaleXProperty,
            new DoubleAnimation(RootScale.ScaleX, 0.96 * _sizeScale, TimeSpan.FromMilliseconds(120))
            {
                EasingFunction = ease
            });
        RootScale.BeginAnimation(
            ScaleTransform.ScaleYProperty,
            new DoubleAnimation(RootScale.ScaleY, 0.96 * _sizeScale, TimeSpan.FromMilliseconds(120))
            {
                EasingFunction = ease
            });
    }

    public void SetActionEnabled(RadialAction action, bool enabled)
    {
        if (!_items.TryGetValue(action, out var item))
        {
            return;
        }

        item.SetEnabled(enabled);
        if (!enabled && _hoveredIndex == (int)action)
        {
            SetHover(null);
        }
    }

    public void SetHover(RadialAction? action)
    {
        if (action.HasValue &&
            (!_items.TryGetValue(action.Value, out var item) || !item.IsEnabled))
        {
            action = null;
        }

        var index = action.HasValue ? (int)action.Value : -1;
        if (index == _hoveredIndex)
        {
            return;
        }

        _hoveredIndex = index;

        for (var i = 0; i < _sectors.Count; i++)
        {
            _sectors[i].SetHover(i == index);
        }

        foreach (var pair in _items)
        {
            pair.Value.SetHover(pair.Key == action);
        }

        UpdateCenter(action);
    }

    private void ResetHover()
    {
        _hoveredIndex = -1;

        foreach (var sector in _sectors)
        {
            sector.SetHover(false, animate: false);
        }

        foreach (var item in _items.Values)
        {
            item.SetHover(false, animate: false);
        }

        UpdateCenter(null);
    }

    private void UpdateCenter(RadialAction? action)
    {
        if (action is null)
        {
            CenterTitle.Text = "光圈";
            CenterTitle.Foreground = UiColors.Brush("#FF1D1D1F");
            CenterDot.Fill = UiColors.Brush("#FFF5B963");
            return;
        }

        var info = RadialActionInfo.Get(action.Value);
        CenterTitle.Text = info.Title;
        CenterDot.Fill = new SolidColorBrush(UiColors.Parse(info.AccentHex));
    }

    private void ApplyWindowStyles()
    {
        var handle = new WindowInteropHelper(this).Handle;
        var exStyle = NativeMethods.GetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE).ToInt64();
        exStyle |= NativeMethods.WS_EX_NOACTIVATE |
                   NativeMethods.WS_EX_TOOLWINDOW;
        // 不再设置 WS_EX_TRANSPARENT：圆盘需要自己接收鼠标，
        // 否则悬停与点击会落到下方窗口，表现为鼠标穿透。
        exStyle &= ~NativeMethods.WS_EX_TRANSPARENT;

        NativeMethods.SetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE, new IntPtr(exStyle));
        NativeMethods.SetWindowPos(
            handle,
            IntPtr.Zero,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE |
            NativeMethods.SWP_NOSIZE |
            NativeMethods.SWP_NOZORDER |
            NativeMethods.SWP_NOACTIVATE |
            NativeMethods.SWP_FRAMECHANGED);
    }

    private void BuildSectorVisuals()
    {
        var halfSweep = 45 - (SectorGapDegrees / 2);

        for (var i = 0; i < RadialActionInfo.All.Count; i++)
        {
            var info = RadialActionInfo.All[i];
            var accent = UiColors.Parse(info.AccentHex);
            var centerAngle = i * 90;
            var start = centerAngle - halfSweep;
            var end = centerAngle + halfSweep;
            var geometry = CreateSectorGeometry(start, end, SectorInnerRadius, SectorOuterRadius);

            var baseFill = new Path
            {
                Data = geometry,
                Fill = new SolidColorBrush(Color.FromArgb(0x07, 0x00, 0x00, 0x00)),
                IsHitTestVisible = false
            };

            var fill = new Path
            {
                Data = geometry,
                Fill = UiColors.WithAlpha(accent, 0x26),
                Opacity = 0,
                IsHitTestVisible = false
            };

            var arc = new Path
            {
                Data = CreateArcGeometry(start, end, SectorOuterRadius - 0.8),
                Stroke = new SolidColorBrush(accent),
                StrokeThickness = 2.2,
                StrokeStartLineCap = PenLineCap.Round,
                StrokeEndLineCap = PenLineCap.Round,
                Fill = null,
                Opacity = 0,
                IsHitTestVisible = false
            };

            SectorLayer.Children.Add(baseFill);
            SectorLayer.Children.Add(fill);
            SectorLayer.Children.Add(arc);
            _sectors.Add(new SectorVisual(fill, arc));
        }
    }

    private void BuildActionItems()
    {
        foreach (var info in RadialActionInfo.All)
        {
            var angle = (int)info.Action * 90.0;
            var center = PointOnCircle(angle, NodeRadius);
            var view = new RadialItemView(info, angle);

            Canvas.SetLeft(view, center.X - (RadialItemView.ViewWidth / 2));
            Canvas.SetTop(view, center.Y - (RadialItemView.ViewHeight / 2));
            ItemLayer.Children.Add(view);
            _items.Add(info.Action, view);
        }
    }

    private static Point PointOnCircle(double angleDegrees, double radius)
    {
        var radians = (angleDegrees - 90.0) * Math.PI / 180.0;
        return new Point(
            CenterPoint + (radius * Math.Cos(radians)),
            CenterPoint + (radius * Math.Sin(radians)));
    }

    private static Geometry CreateSectorGeometry(
        double startAngle,
        double endAngle,
        double innerRadius,
        double outerRadius)
    {
        var outerStart = PointOnCircle(startAngle, outerRadius);
        var outerEnd = PointOnCircle(endAngle, outerRadius);
        var innerEnd = PointOnCircle(endAngle, innerRadius);
        var innerStart = PointOnCircle(startAngle, innerRadius);

        var figure = new PathFigure
        {
            StartPoint = outerStart,
            IsClosed = true,
            IsFilled = true
        };

        figure.Segments.Add(new ArcSegment
        {
            Point = outerEnd,
            Size = new Size(outerRadius, outerRadius),
            IsLargeArc = false,
            SweepDirection = SweepDirection.Clockwise
        });
        figure.Segments.Add(new LineSegment(innerEnd, true));
        figure.Segments.Add(new ArcSegment
        {
            Point = innerStart,
            Size = new Size(innerRadius, innerRadius),
            IsLargeArc = false,
            SweepDirection = SweepDirection.Counterclockwise
        });
        figure.Segments.Add(new LineSegment(outerStart, true));

        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        geometry.Freeze();
        return geometry;
    }

    private static Geometry CreateArcGeometry(double startAngle, double endAngle, double radius)
    {
        var figure = new PathFigure
        {
            StartPoint = PointOnCircle(startAngle, radius),
            IsClosed = false,
            IsFilled = false
        };

        figure.Segments.Add(new ArcSegment
        {
            Point = PointOnCircle(endAngle, radius),
            Size = new Size(radius, radius),
            IsLargeArc = false,
            SweepDirection = SweepDirection.Clockwise
        });

        var geometry = new PathGeometry();
        geometry.Figures.Add(figure);
        geometry.Freeze();
        return geometry;
    }

    private sealed class SectorVisual
    {
        private readonly Path _fill;
        private readonly Path _arc;

        public SectorVisual(Path fill, Path arc)
        {
            _fill = fill;
            _arc = arc;
        }

        public void SetHover(bool hovered, bool animate = true)
        {
            var fillTarget = hovered ? 1.0 : 0.0;
            var arcTarget = hovered ? 1.0 : 0.0;

            if (!animate)
            {
                _fill.Opacity = fillTarget;
                _arc.Opacity = arcTarget;
                return;
            }

            var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
            var duration = TimeSpan.FromMilliseconds(125);

            _fill.BeginAnimation(
                UIElement.OpacityProperty,
                new DoubleAnimation(fillTarget, duration) { EasingFunction = ease });
            _arc.BeginAnimation(
                UIElement.OpacityProperty,
                new DoubleAnimation(arcTarget, duration) { EasingFunction = ease });
        }
    }
}
