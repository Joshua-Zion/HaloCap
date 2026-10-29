using System;
using System.Windows;
using System.Windows.Automation;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using HaloCap.Models;

namespace HaloCap.UI;

internal sealed class RadialItemView : Grid
{
    public const double ViewWidth = 68;
    public const double ViewHeight = 66;

    private readonly Border _iconBorder;
    private readonly Path _iconPath;
    private readonly TextBlock _badgeText;
    private readonly Border _badge;
    private readonly TextBlock _label;
    private readonly ScaleTransform _iconScale;
    private readonly TranslateTransform _translate;
    private readonly Vector _outward;

    private readonly SolidColorBrush _neutralIconBackground = UiColors.Brush("#FFFFFFFF");
    private readonly SolidColorBrush _neutralIconBorder = UiColors.Brush("#1A000000");
    private readonly SolidColorBrush _neutralIconStroke = UiColors.Brush("#FF3C3C43");
    private readonly SolidColorBrush _neutralLabel = UiColors.Brush("#FF515156");
    private readonly SolidColorBrush _neutralBadgeBackground = UiColors.Brush("#FFEDEEF2");
    private readonly SolidColorBrush _neutralBadgeText = UiColors.Brush("#FF85858B");
    private readonly SolidColorBrush _hoverIconBackground;
    private readonly SolidColorBrush _hoverBorder;
    private readonly SolidColorBrush _hoverLabel = UiColors.Brush("#FF1D1D1F");
    private readonly SolidColorBrush _hoverBadgeText = UiColors.Brush("#FF1D1D1F");

    public RadialItemView(RadialActionInfo info, double angleDegrees)
    {
        Width = ViewWidth;
        Height = ViewHeight;
        AutomationProperties.SetName(this, info.Title);
        AutomationProperties.SetAutomationId(this, info.Action switch
        {
            RadialAction.Screenshot => "radial.screenshot",
            RadialAction.Ocr => "radial.ocr",
            RadialAction.Record => "radial.record",
            RadialAction.Pin => "radial.pin",
            _ => $"radial.{info.Action.ToString().ToLowerInvariant()}"
        });

        var accent = UiColors.Parse(info.AccentHex);
        _hoverIconBackground = UiColors.WithAlpha(accent, 0x26);
        _hoverBorder = new SolidColorBrush(accent);
        _hoverBorder.Freeze();

        var radians = (angleDegrees - 90.0) * Math.PI / 180.0;
        _outward = new Vector(Math.Cos(radians), Math.Sin(radians));

        _translate = new TranslateTransform();
        RenderTransform = _translate;

        var stack = new StackPanel
        {
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center
        };

        var iconHost = new Grid
        {
            Width = 40,
            Height = 40,
            HorizontalAlignment = HorizontalAlignment.Center
        };

        _iconBorder = new Border
        {
            Width = 40,
            Height = 40,
            CornerRadius = new CornerRadius(13),
            Background = _neutralIconBackground,
            BorderBrush = _neutralIconBorder,
            BorderThickness = new Thickness(1),
            RenderTransformOrigin = new Point(0.5, 0.5)
        };

        _iconScale = new ScaleTransform(1, 1);
        _iconBorder.RenderTransform = _iconScale;

        _iconPath = new Path
        {
            Data = Geometry.Parse(info.IconGeometry),
            Stroke = _neutralIconStroke,
            StrokeThickness = 1.55,
            StrokeStartLineCap = PenLineCap.Round,
            StrokeEndLineCap = PenLineCap.Round,
            StrokeLineJoin = PenLineJoin.Round,
            Fill = null
        };

        var iconViewbox = new Viewbox
        {
            Width = 21,
            Height = 21,
            Child = _iconPath,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center
        };

        _iconBorder.Child = iconViewbox;

        _badgeText = new TextBlock
        {
            Text = info.Shortcut,
            FontSize = 9.5,
            FontWeight = FontWeights.SemiBold,
            Foreground = _neutralBadgeText,
            HorizontalAlignment = HorizontalAlignment.Center,
            VerticalAlignment = VerticalAlignment.Center,
            TextAlignment = TextAlignment.Center
        };

        _badge = new Border
        {
            Width = 17,
            Height = 17,
            CornerRadius = new CornerRadius(8.5),
            Background = _neutralBadgeBackground,
            HorizontalAlignment = HorizontalAlignment.Right,
            VerticalAlignment = VerticalAlignment.Top,
            Margin = new Thickness(0, -3, -4, 0),
            Child = _badgeText
        };

        iconHost.Children.Add(_iconBorder);
        iconHost.Children.Add(_badge);

        _label = new TextBlock
        {
            Text = info.Title,
            FontSize = 12.5,
            FontWeight = FontWeights.Medium,
            Foreground = _neutralLabel,
            HorizontalAlignment = HorizontalAlignment.Center,
            TextAlignment = TextAlignment.Center,
            Margin = new Thickness(0, 7, 0, 0)
        };

        stack.Children.Add(iconHost);
        stack.Children.Add(_label);
        Children.Add(stack);
    }

    public void SetEnabled(bool enabled)
    {
        IsEnabled = enabled;
        Opacity = enabled ? 1.0 : 0.35;
        if (!enabled)
        {
            SetHover(false, animate: false);
        }
    }

    public void SetHover(bool hovered, bool animate = true)
    {
        hovered = hovered && IsEnabled;
        _iconBorder.Background = hovered ? _hoverIconBackground : _neutralIconBackground;
        _iconBorder.BorderBrush = hovered ? _hoverBorder : _neutralIconBorder;
        _iconPath.Stroke = hovered ? _hoverBorder : _neutralIconStroke;
        _label.Foreground = hovered ? _hoverLabel : _neutralLabel;
        _badge.Background = hovered ? _hoverBorder : _neutralBadgeBackground;
        _badgeText.Foreground = hovered ? _hoverBadgeText : _neutralBadgeText;

        if (!animate)
        {
            _translate.BeginAnimation(TranslateTransform.XProperty, null);
            _translate.BeginAnimation(TranslateTransform.YProperty, null);
            _iconScale.BeginAnimation(ScaleTransform.ScaleXProperty, null);
            _iconScale.BeginAnimation(ScaleTransform.ScaleYProperty, null);
            _translate.X = hovered ? _outward.X * 4 : 0;
            _translate.Y = hovered ? _outward.Y * 4 : 0;
            _iconScale.ScaleX = hovered ? 1.06 : 1.0;
            _iconScale.ScaleY = hovered ? 1.06 : 1.0;
            return;
        }

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        var duration = TimeSpan.FromMilliseconds(130);

        _translate.BeginAnimation(
            TranslateTransform.XProperty,
            new DoubleAnimation(hovered ? _outward.X * 4 : 0, duration) { EasingFunction = ease });
        _translate.BeginAnimation(
            TranslateTransform.YProperty,
            new DoubleAnimation(hovered ? _outward.Y * 4 : 0, duration) { EasingFunction = ease });
        _iconScale.BeginAnimation(
            ScaleTransform.ScaleXProperty,
            new DoubleAnimation(hovered ? 1.06 : 1.0, duration) { EasingFunction = ease });
        _iconScale.BeginAnimation(
            ScaleTransform.ScaleYProperty,
            new DoubleAnimation(hovered ? 1.06 : 1.0, duration) { EasingFunction = ease });
    }
}
