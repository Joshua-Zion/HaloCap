using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Interop;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using HaloCap.Diagnostics;
using HaloCap.Interop;
using HaloCap.Models;
using HaloCap.Services;
using DrawingBitmap = System.Drawing.Bitmap;

namespace HaloCap.UI;

public partial class ScreenshotOverlayWindow : Window
{
    private const double HandleHitRadius = 9;
    private const int MinSelectionSizePx = 8;
    private const int MaxHistory = 40;
    private const int MagnifierPixels = 17;
    private const int ElementProbeIntervalMs = 24;
    private const long PhysicalPressWindowMs = 600;
    private const int VkLeftButton = 0x01;

    // 未框选时保持轻遮罩，进入选区后再压暗外围，启动时不会整屏像被盖住。
    private static readonly Brush HoverDimBrush = CreateDimBrush(0x5C);
    private static readonly Brush SelectionDimBrush = CreateDimBrush(0x8C);

    private enum DragKind
    {
        None,
        Create,
        Move,
        Resize
    }

    private readonly ScreenshotSession _session;
    private readonly ScreenMonitor _monitor;
    private readonly CaptureSessionMode _mode;
    private readonly DrawingBitmap? _frozenBitmap;
    private readonly BitmapSource? _frozenSource;
    private readonly List<AnnotationStroke> _strokes = new();
    private readonly List<List<AnnotationStroke>> _undoStack = new();
    private readonly List<List<AnnotationStroke>> _redoStack = new();

    private IntPtr _handle;
    private bool _closed;
    private bool _revealed;
    private bool _dragging;
    private bool _hasSelection;
    private DragKind _dragKind;
    private int _resizeMask;
    private PhysicalRect _dragOriginVirtual;
    private PhysicalRect _dragStartVirtual;
    private PhysicalRect _startVirtual;
    private PhysicalRect _selectionPx;
    private PhysicalRect _hoverElementPx;
    private PhysicalRect _clickElementPx;
    private long _lastElementProbeTick;
    private WriteableBitmap? _magnifierBitmap;
    private FormatConvertedBitmap? _magnifierSource;
    private byte[] _magnifierSample = Array.Empty<byte>();
    private byte[] _magnifierFrame = Array.Empty<byte>();
    private int _magnifierBlock = 8;
    private int _lastProbeX;
    private int _lastProbeY;
    private double _mapScaleX = 1.0;
    private double _mapScaleY = 1.0;
    private TranslateTransform? _toolbarOffset;
    private HwndSource? _source;
    private long _lastPhysicalPressTick = -1_000_000;
    private bool _syntheticPressLogged;
    private bool _activeDim;
    private bool _initialHoverLogged;
    private readonly DispatcherTimer _idleHoverTimer;
    private readonly DispatcherTimer _revealSettleTimer;
    private bool _revealCompleted;
    private bool _activationPending;

    private bool _annotating;
    private AnnotationTool _annotationTool = AnnotationTool.Pen;
    private AnnotationTool _shapeTool = AnnotationTool.Rectangle;
    private Color _annotationColor = AnnotationDefaults.Palette[0];
    private double _strokeThickness = AnnotationDefaults.Thickness;
    private double _highlighterThickness = AnnotationDefaults.HighlighterThickness;
    private double _textSize = AnnotationDefaults.TextSize;
    private int _mosaicBlockDip = AnnotationDefaults.MosaicBlockDip;
    private double _eraserSize = AnnotationDefaults.EraserSize;
    private bool _fillShapes;
    private bool _textBold = false;
    private bool _textItalic;
    private ArrowHeadMode _arrowHead = ArrowHeadMode.End;
    private bool _eraseHistoryPushed;
    private Point _mosaicStart;
    private Int32Rect _mosaicLastRect;
    private AnnotationStroke? _liveStroke;
    private bool _drawingAnnotation;
    private TextBox? _textEditor;
    private Point _textPoint;
    private readonly DispatcherTimer _toolHudTimer;

    internal ScreenshotOverlayWindow(
        ScreenshotSession session,
        ScreenMonitor monitor,
        CaptureSessionMode mode,
        DrawingBitmap? frozenBitmap)
    {
        InitializeComponent();

        // 先在完全透明状态下完成首帧渲染，再一次性显示；分层窗口若带着未合成的首帧出现，
        // 会先透出未变暗的实时桌面再变暗，看起来就是一次闪屏。
        Opacity = 0;

        _session = session;
        _monitor = monitor;
        _mode = mode;
        _frozenBitmap = frozenBitmap;

        if (frozenBitmap is not null)
        {
            _frozenSource = ScreenCaptureService.ToBitmapSource(frozenBitmap);
            FrozenImage.Source = _frozenSource;
        }
        else
        {
            FrozenImage.Visibility = Visibility.Collapsed;
            // 分层窗口的全透明像素会穿透鼠标，导致悬停挖洞与 MouseLeave 恢复遮罩反复交替。
            Root.Background = UiColors.Brush("#01000000");
        }

        ConfigureMode();
        ConfigureWindowBounds();
        RefreshMapping();
        UpdateAnnotationToolVisuals();
        UpdateAnnotationColorVisuals();

        _toolHudTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(1100) };
        _toolHudTimer.Tick += (_, _) =>
        {
            _toolHudTimer.Stop();
            FadeToolHudOut();
        };

        // 启动时 WPF 可能不会补发鼠标移动，窗口激活时也可能收到一次假的离开事件；
        // 定时校正未框选状态，保证候选窗口一直可见，并兜底清理未成形的点选区。
        _idleHoverTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(150) };
        _idleHoverTimer.Tick += (_, _) => IdleHoverTick();
        _idleHoverTimer.Start();

        // 遮罩上屏后再抢焦点，否则透明或半透明阶段的前台窗口会先失活，屏幕像是闪了一下。
        _revealSettleTimer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(40) };
        _revealSettleTimer.Tick += (_, _) => OnRevealSettled();

        SourceInitialized += (_, _) =>
        {
            ApplyOverlayWindowStyle();
            _source = HwndSource.FromHwnd(new WindowInteropHelper(this).Handle);
            _source?.AddHook(OverlayWndProc);
        };
        DpiChanged += (_, _) =>
        {
            ApplyPhysicalBounds();
            RefreshMapping();
        };

        Loaded += (_, _) =>
        {
            RefreshMapping();
            if (_session.HasSelection)
            {
                ApplySharedSelection();
            }
            else
            {
                ResetToIdle();
            }
        };
        SizeChanged += (_, _) =>
        {
            RefreshMapping();
            UpdateAnnotationFrame();
            PositionToolbar();
        };
        Closed += (_, _) =>
        {
            _toolHudTimer.Stop();
            _idleHoverTimer.Stop();
            _revealSettleTimer.Stop();

            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }

            _source?.RemoveHook(OverlayWndProc);
            _source = null;
            _frozenBitmap?.Dispose();
            FrozenImage.Source = null;
            _session.UnregisterOverlay(this);
        };

        _session.RegisterOverlay(this);
    }

    internal CaptureSessionMode Mode => _mode;

    internal ScreenMonitor Monitor => _monitor;

    internal bool HasSelection => _hasSelection;

    internal bool IsSelectionHost => ReferenceEquals(_session.SelectionHost, this);

    internal PhysicalRect LocalSelection =>
        ScreenGeometry.Intersect(_selectionPx, MonitorLocalBounds);

    internal PhysicalRect VirtualSelection => _session.HasSelection
        ? _session.SelectionVirtual
        : ScreenGeometry.ToVirtual(_selectionPx, _monitor.Bounds);

    private PhysicalRect MonitorLocalBounds => new(0, 0, _monitor.Bounds.Width, _monitor.Bounds.Height);

    private PhysicalRect SelectionContainerVirtual =>
        _mode == CaptureSessionMode.Record || _session.DesktopBounds.IsEmpty
            ? _monitor.Bounds
            : _session.DesktopBounds;

    internal void ApplySharedSelection()
    {
        if (_closed)
        {
            return;
        }

        if (!_session.HasSelection)
        {
            if (_hasSelection || _dragging || Toolbar.Visibility == Visibility.Visible)
            {
                ResetToIdle();
            }

            return;
        }

        _selectionPx = ScreenGeometry.ToLocal(_session.SelectionVirtual, _monitor.Bounds);
        var local = ScreenGeometry.Intersect(_selectionPx, MonitorLocalBounds);
        _hasSelection = !local.IsEmpty || _dragging;

        HintChip.Visibility = Visibility.Collapsed;
        DimFull.Visibility = Visibility.Collapsed;

        if (local.IsEmpty)
        {
            SetActiveDim(false);
            SetFullDim(!_dragging);
            DimFull.Visibility = Visibility.Visible;
            DimTop.Visibility = Visibility.Collapsed;
            DimBottom.Visibility = Visibility.Collapsed;
            DimLeft.Visibility = Visibility.Collapsed;
            DimRight.Visibility = Visibility.Collapsed;
            SelectionBorder.Visibility = Visibility.Collapsed;
            SetCornersVisible(false);
            SizeBadge.Visibility = Visibility.Collapsed;
            HideToolbar();
            HideMagnifier();
            HideCrosshair();
            AnnotationLayer.Visibility = Visibility.Collapsed;
            return;
        }

        if (!_dragging)
        {
            SizeBadge.Visibility = Visibility.Collapsed;
        }

        UpdateSelectionVisual(_dragging);

        var isHost = IsSelectionHost;
        if (!isHost && _annotating)
        {
            SetDrawingTool(null);
        }

        if (isHost && _hasSelection)
        {
            UpdateAnnotationFrame();

            if (_session.IsSelectionComplete)
            {
                if (Toolbar.Visibility != Visibility.Visible)
                {
                    ShowToolbar();
                }
                else
                {
                    PositionToolbar();
                }
            }
            else
            {
                HideToolbar();
            }
        }
        else
        {
            HideToolbar();
            AnnotationLayer.Visibility = Visibility.Collapsed;
        }
    }

    private PhysicalRect ToVirtualPoint(Point dipPoint) => new(
        _monitor.Bounds.Left + (int)Math.Round(dipPoint.X * _mapScaleX),
        _monitor.Bounds.Top + (int)Math.Round(dipPoint.Y * _mapScaleY),
        0,
        0);

    private PhysicalRect NormalizeVirtualSelection(PhysicalRect start, PhysicalRect current)
    {
        var container = SelectionContainerVirtual;
        var startLocal = start.Offset(-container.Left, -container.Top);
        var currentLocal = current.Offset(-container.Left, -container.Top);
        return ScreenGeometry.NormalizeSelection(
                startLocal.Left,
                startLocal.Top,
                currentLocal.Left,
                currentLocal.Top,
                container.Width,
                container.Height)
            .Offset(container.Left, container.Top);
    }

    private PhysicalRect MoveVirtualSelection(PhysicalRect origin, int dx, int dy)
    {
        var container = SelectionContainerVirtual;
        return ScreenGeometry.MoveWithin(
                origin.Offset(-container.Left, -container.Top),
                dx,
                dy,
                container.Width,
                container.Height)
            .Offset(container.Left, container.Top);
    }

    private PhysicalRect ResizeVirtualSelection(PhysicalRect origin, int mask, PhysicalRect cursor)
    {
        var container = SelectionContainerVirtual;
        return ScreenGeometry.ResizeEdges(
                origin.Offset(-container.Left, -container.Top),
                mask,
                cursor.Left - container.Left,
                cursor.Top - container.Top,
                container.Width,
                container.Height,
                MinSelectionSizePx)
            .Offset(container.Left, container.Top);
    }

    internal void PrepareAndShow()
    {
        if (_closed)
        {
            return;
        }

        var helper = new WindowInteropHelper(this);
        _handle = helper.EnsureHandle();

        Show();
        ApplyPhysicalBounds();

        Dispatcher.BeginInvoke(
            DispatcherPriority.Loaded,
            new Action(() =>
            {
                if (_closed)
                {
                    return;
                }

                ApplyPhysicalBounds();
                RefreshMapping();
                if (_session.HasSelection)
                {
                    ApplySharedSelection();
                }
                else
                {
                    ResetToIdle();
                }
                PositionCrosshairFromCursor();
            }));
    }

    internal void RevealOverlay()
    {
        if (_revealed || _closed)
        {
            return;
        }

        // DPI/窗口位置可能在首帧后仍有一次调整，显示前再对齐一次显示器边界。
        ApplyPhysicalBounds();
        RefreshMapping();
        Root.UpdateLayout();
        _revealed = true;
        _revealCompleted = true;

        // 先把候选窗口和遮罩开洞算好，再直接显示，避免第一帧之后再改变整屏明暗。
        if (!_session.HasSelection)
        {
            PositionCrosshairFromCursor();
        }

        // 选区要即时出现：轻遮罩直接上屏，不再做淡入，既没有跨帧亮度变化也不拖慢启动。
        BeginAnimation(OpacityProperty, null);
        Opacity = 1;

        _revealSettleTimer.Stop();
        _revealSettleTimer.Start();
    }

    private void OnRevealSettled()
    {
        _revealSettleTimer.Stop();

        if (_closed)
        {
            return;
        }

        // 首帧时布局尚未就绪、没能算出候选时，上屏后补一次。
        if (!_session.HasSelection &&
            !_hasSelection &&
            !_dragging &&
            !_annotating &&
            ElementBorder.Visibility != Visibility.Visible)
        {
            PositionCrosshairFromCursor();
        }

        TryActivateForInput();
    }

    internal void ActivateForSelection()
    {
        if (_closed)
        {
            return;
        }

        _activationPending = true;
        PositionCrosshairFromCursor();

        if (_revealCompleted)
        {
            TryActivateForInput();
        }
    }

    private void TryActivateForInput()
    {
        if (_closed || !_activationPending)
        {
            return;
        }

        _activationPending = false;

        // 用户已经在另一块屏上开始操作时，不要再把焦点抢回启动时的遮罩。
        if (!ReferenceEquals(_session.ActiveOverlay, this))
        {
            return;
        }

        try
        {
            Activate();
            MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
            Keyboard.Focus(Root);
        }
        catch
        {
            // 窗口可能已被其他显示器事件关闭。
        }
    }

    internal BitmapSource? CreateLocalSelectionBitmap(bool includeAnnotations)
    {
        var local = LocalSelection;

        if (_frozenBitmap is null || local.IsEmpty)
        {
            return null;
        }

        var scale = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        var cropped = ScreenCaptureService.CropToBitmapSource(
            _frozenBitmap,
            local,
            96.0 * scale);

        if (!includeAnnotations || _strokes.Count == 0)
        {
            return cropped;
        }

        RefreshMosaicTiles();
        return AnnotationRenderer.Compose(cropped, _strokes);
    }

    internal BitmapSource? CreateSelectionBitmap(bool includeAnnotations) =>
        _hasSelection ? CreateLocalSelectionBitmap(includeAnnotations) : null;

    internal BitmapSource ComposeAnnotations(BitmapSource image)
    {
        if (_strokes.Count == 0)
        {
            return image;
        }

        RefreshMosaicTiles();
        return AnnotationRenderer.Compose(image, _strokes);
    }

    internal void ClearSelectionFromSession()
    {
        if (_closed || _dragging)
        {
            return;
        }

        ResetToIdle();
    }

    internal void CloseOverlay()
    {
        if (_closed)
        {
            return;
        }

        _closed = true;
        CancelTextEditor();

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放鼠标捕获时的异常。
        }

        Close();
    }

    private void ConfigureWindowBounds()
    {
        var scale = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        WindowStartupLocation = WindowStartupLocation.Manual;
        Left = _monitor.Bounds.Left / scale;
        Top = _monitor.Bounds.Top / scale;
        Width = Math.Max(1, _monitor.Bounds.Width / scale);
        Height = Math.Max(1, _monitor.Bounds.Height / scale);

        DimFull.Width = Math.Max(0, Width);
        DimFull.Height = Math.Max(0, Height);
        Canvas.SetLeft(DimFull, 0);
        Canvas.SetTop(DimFull, 0);
    }

    private void ApplyOverlayWindowStyle()
    {
        var handle = new WindowInteropHelper(this).Handle;
        if (handle == IntPtr.Zero)
        {
            return;
        }

        var exStyle = NativeMethods.GetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE).ToInt64();
        exStyle |= NativeMethods.WS_EX_TOOLWINDOW;
        NativeMethods.SetWindowLongPtr(handle, NativeMethods.GWL_EXSTYLE, new IntPtr(exStyle));

        NativeMethods.SetWindowPos(
            handle,
            NativeMethods.HWND_TOPMOST,
            0,
            0,
            0,
            0,
            NativeMethods.SWP_NOMOVE |
            NativeMethods.SWP_NOSIZE |
            NativeMethods.SWP_NOACTIVATE |
            NativeMethods.SWP_FRAMECHANGED);
    }

    private IntPtr OverlayWndProc(IntPtr hwnd, int msg, IntPtr wParam, IntPtr lParam, ref bool handled)
    {
        if (msg == NativeMethods.WM_LBUTTONDOWN)
        {
            _lastPhysicalPressTick = Environment.TickCount64;
        }

        return IntPtr.Zero;
    }

    private static bool IsLeftButtonPressed() =>
        (NativeMethods.GetAsyncKeyState(VkLeftButton) & 0x8000) != 0;

    private void ApplyPhysicalBounds()
    {
        if (_handle == IntPtr.Zero)
        {
            _handle = new WindowInteropHelper(this).Handle;
        }

        if (_handle == IntPtr.Zero)
        {
            return;
        }

        NativeMethods.SetWindowPos(
            _handle,
            NativeMethods.HWND_TOPMOST,
            _monitor.Bounds.Left,
            _monitor.Bounds.Top,
            _monitor.Bounds.Width,
            _monitor.Bounds.Height,
            NativeMethods.SWP_NOACTIVATE | NativeMethods.SWP_SHOWWINDOW);
    }

    private void ConfigureMode()
    {
        Color accent;

        switch (_mode)
        {
            case CaptureSessionMode.Record:
                accent = Color.FromRgb(0xFF, 0x7A, 0x7A);
                HintText.Text = "拖动框选（单屏） · Enter 开始录制 · Esc 取消";
                ToolGroup.Visibility = Visibility.Collapsed;
                HistorySeparator.Visibility = Visibility.Collapsed;
                HistoryGroup.Visibility = Visibility.Collapsed;
                PinButton.Visibility = Visibility.Collapsed;
                SaveButton.Visibility = Visibility.Collapsed;
                OcrButton.Visibility = Visibility.Collapsed;
                StartRecordButton.Visibility = Visibility.Visible;
                DoneButton.Visibility = Visibility.Collapsed;
                break;

            case CaptureSessionMode.Ocr:
                accent = Color.FromRgb(0xB0, 0x8C, 0xFF);
                HintText.Text = "拖动框选或单击窗口 · 松开即识别 · Esc 取消";
                DoneButton.Visibility = Visibility.Collapsed;
                break;

            case CaptureSessionMode.Pin:
                accent = Color.FromRgb(0xF5, 0xB9, 0x63);
                HintText.Text = "拖动框选或单击窗口 · 松开即贴图 · Esc 取消";
                DoneButton.Visibility = Visibility.Collapsed;
                break;

            default:
                accent = Color.FromRgb(0x7C, 0xA8, 0xFF);
                HintText.Text = "拖动框选或单击窗口 · Enter 复制 · Esc 取消";
                ToolGroup.Visibility = Visibility.Visible;
                HistorySeparator.Visibility = Visibility.Visible;
                HistoryGroup.Visibility = Visibility.Visible;
                PinButton.Visibility = Visibility.Visible;
                SaveButton.Visibility = Visibility.Visible;
                OcrButton.Visibility = Visibility.Visible;
                StartRecordButton.Visibility = Visibility.Collapsed;
                DoneButton.Visibility = Visibility.Visible;
                break;
        }

        var brush = new SolidColorBrush(accent);
        brush.Freeze();
        SelectionBorder.BorderBrush = brush;
        ElementBorder.Stroke = brush;
        MagnifierPixelRing.Stroke = brush;
        ToolHudDot.Fill = brush;
        CornerTopLeft.Fill = brush;        CornerTopRight.Fill = brush;
        CornerBottomLeft.Fill = brush;
        CornerBottomRight.Fill = brush;
        EdgeTop.Fill = brush;
        EdgeBottom.Fill = brush;
        EdgeLeft.Fill = brush;
        EdgeRight.Fill = brush;
        HintDot.Fill = brush;
    }

    private void RefreshMapping()
    {
        if (Root.ActualWidth > 0.5)
        {
            _mapScaleX = _monitor.Bounds.Width / Root.ActualWidth;
        }
        else
        {
            _mapScaleX = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        }

        if (Root.ActualHeight > 0.5)
        {
            _mapScaleY = _monitor.Bounds.Height / Root.ActualHeight;
        }
        else
        {
            _mapScaleY = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        }

        DimFull.Width = Math.Max(0, Root.ActualWidth);
        DimFull.Height = Math.Max(0, Root.ActualHeight);
        Canvas.SetLeft(DimFull, 0);
        Canvas.SetTop(DimFull, 0);
    }

    private void Root_PreviewMouseLeftButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (_closed || e.ChangedButton != MouseButton.Left)
        {
            return;
        }

        // 从托盘或光圈启动时，系统可能在遮罩刚出现时补发一次按下（没有对应的窗口鼠标消息），
        // 放行会立刻在原地完成一个点选区；只接受本窗口真实收到的 WM_LBUTTONDOWN。
        var pressAge = Environment.TickCount64 - _lastPhysicalPressTick;
        if (e.StylusDevice is null && pressAge > PhysicalPressWindowMs)
        {
            e.Handled = true;

            if (!_syntheticPressLogged)
            {
                _syntheticPressLogged = true;
                Log.Info($"忽略启动时补发的鼠标按下（{pressAge}ms）");
            }

            return;
        }

        if (IsPointerOverToolbars())
        {
            return;
        }

        if (IsWithinTextEditor(e.OriginalSource))
        {
            return;
        }

        if (_textEditor is not null)
        {
            CommitTextEditor();
        }

        e.Handled = true;
        Activate();
        MonitorService.ActivateWindow(new WindowInteropHelper(this).Handle);
        Keyboard.Focus(Root);
        _activationPending = false;

        var point = e.GetPosition(Root);

        if (_annotating && _hasSelection && IsSelectionHost)
        {
            BeginAnnotationStroke(point);
            return;
        }

        if (e.ClickCount == 2 &&
            _session.HasSelection &&
            _mode == CaptureSessionMode.Screenshot &&
            IsInsideSelection(point))
        {
            _session.RequestAction(this, CaptureCommand.Copy);
            return;
        }

        if (_hasSelection && !_dragging)
        {
            var mask = HitTestSelectionHandle(point);

            if (mask != 0)
            {
                _dragKind = DragKind.Resize;
                _resizeMask = mask;
                _dragOriginVirtual = _session.SelectionVirtual;
                _dragStartVirtual = ToVirtualPoint(point);
                _dragging = true;
                Mouse.Capture(Root);
                return;
            }

            if (IsInsideSelection(point))
            {
                _dragKind = DragKind.Move;
                _dragOriginVirtual = _session.SelectionVirtual;
                _dragStartVirtual = ToVirtualPoint(point);
                _dragging = true;
                Mouse.Capture(Root);
                return;
            }
        }

        ClearAnnotations();
        SetDrawingTool(null);

        _clickElementPx = _hoverElementPx;
        ClearElementHover();
        HideMagnifier();

        _dragKind = DragKind.Create;
        _dragging = true;
        _startVirtual = ToVirtualPoint(point);

        HideToolbar();
        SizeBadge.Visibility = Visibility.Collapsed;
        HintChip.Visibility = Visibility.Collapsed;
        HideCrosshair();
        DimFull.Visibility = Visibility.Collapsed;
        _session.BeginSelection(this, new PhysicalRect(_startVirtual.Left, _startVirtual.Top, 0, 0));

        Mouse.Capture(Root);
    }

    private void Root_PreviewMouseMove(object sender, MouseEventArgs e)
    {
        if (_closed)
        {
            return;
        }

        var point = e.GetPosition(Root);

        if (_drawingAnnotation)
        {
            UpdateAnnotationStroke(point);
            e.Handled = true;
            return;
        }

        if (_dragging)
        {
            if (!IsLeftButtonPressed())
            {
                // 松开消息丢失时按松开结算，否则选区会永远停在按下时的那个点。
                EndDrag();
                return;
            }

            var cursor = ToVirtualPoint(point);

            switch (_dragKind)
            {
                case DragKind.Create:
                    _session.UpdateSelection(NormalizeVirtualSelection(_startVirtual, cursor));
                    break;

                case DragKind.Move:
                    _session.UpdateSelection(MoveVirtualSelection(
                        _dragOriginVirtual,
                        cursor.Left - _dragStartVirtual.Left,
                        cursor.Top - _dragStartVirtual.Top));
                    break;

                case DragKind.Resize:
                    _session.UpdateSelection(ResizeVirtualSelection(
                        _dragOriginVirtual,
                        _resizeMask,
                        cursor));
                    break;
            }

            if (_dragKind is DragKind.Create or DragKind.Resize)
            {
                UpdateMagnifier(point);
            }
            else
            {
                HideMagnifier();
            }

            e.Handled = true;
            return;
        }

        if (_hasSelection)
        {
            if (!_annotating &&
                !IsPointerOverToolbars())
            {
                UpdateSelectionCursor(point);
            }

            HideCrosshair();
            HideMagnifier();
            return;
        }

        if (_session.HasSelection)
        {
            HideCrosshair();
            HideMagnifier();
            return;
        }

        if (IsPointerOverToolbars())
        {
            HideCrosshair();
            HideMagnifier();
            ClearElementHover();
            return;
        }

        UpdateCrosshair(point);
        UpdateMagnifier(point);
        UpdateElementHover(point);
    }

    private void Root_PreviewMouseLeftButtonUp(object sender, MouseButtonEventArgs e)
    {
        if (_closed)
        {
            return;
        }

        if (_drawingAnnotation)
        {
            e.Handled = true;
            FinishAnnotationStroke();
            return;
        }

        if (!_dragging)
        {
            return;
        }

        e.Handled = true;
        EndDrag();
    }

    private void Root_LostMouseCapture(object sender, MouseEventArgs e)
    {
        if (_closed || _drawingAnnotation || !_dragging)
        {
            return;
        }

        if (IsLeftButtonPressed())
        {
            // 拖拽中途被其它窗口抢走捕获，之后就收不到松开消息，放弃这次选区以免停在按下时的点。
            _dragging = false;
            _dragKind = DragKind.None;
            _clickElementPx = default;
            _session.ClearSelection();
            return;
        }

        EndDrag();
    }

    private void EndDrag()
    {
        _dragging = false;

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放捕获异常。
        }

        var kind = _dragKind;
        _dragKind = DragKind.None;

        if (kind != DragKind.Create)
        {
            HideMagnifier();
            RefreshMosaicTiles();
            _session.CompleteSelection();
            return;
        }

        if (!ScreenGeometry.IsUsableSelection(_session.SelectionVirtual, MinSelectionSizePx))
        {
            // 单击或轻微抖动都按单击处理：选中光标下的窗口，没有窗口就整屏，绝不留下几像素的点选区。
            var element = ScreenGeometry.IsUsableSelection(_clickElementPx, MinSelectionSizePx)
                ? _clickElementPx
                : MonitorLocalBounds;
            _session.UpdateSelection(ScreenGeometry.ToVirtual(element, _monitor.Bounds));
        }

        _clickElementPx = default;
        CompleteNewSelection();
    }

    private void CompleteNewSelection()
    {
        HideMagnifier();
        ClearElementHover();
        HintChip.Visibility = Visibility.Collapsed;
        _session.CompleteSelection();

        if (_handle != IntPtr.Zero && NativeMethods.GetWindowRect(_handle, out var windowRect))
        {
            Log.Info(
                $"选区完成：monitor={_monitor.Bounds} window={windowRect.Left},{windowRect.Top} {windowRect.Right - windowRect.Left}x{windowRect.Bottom - windowRect.Top} root={Root.ActualWidth:F1}x{Root.ActualHeight:F1} selection={_session.SelectionVirtual}");
        }

        if (_mode == CaptureSessionMode.Ocr)
        {
            _session.RequestAction(this, CaptureCommand.Ocr);
            return;
        }

        if (_mode == CaptureSessionMode.Pin)
        {
            _session.RequestAction(this, CaptureCommand.Pin);
            return;
        }

        Keyboard.Focus(Root);
    }

    private void Root_MouseLeave(object sender, MouseEventArgs e)
    {
        if (_closed || _dragging || _drawingAnnotation)
        {
            return;
        }

        // 窗口刚显示或激活时 WPF 可能补发一次离开事件，实际上光标仍在遮罩内；
        // 误清掉候选会让整屏只剩遮罩，看起来像一个点，需要等光标真的离开再处理。
        if (IsCursorInsideMonitor())
        {
            return;
        }

        HideCrosshair();
        HideMagnifier();

        if (!_hasSelection && !_annotating)
        {
            ClearElementHover();
        }
    }

    private void Root_PreviewMouseRightButtonDown(object sender, MouseButtonEventArgs e)
    {
        if (_closed || IsPointerOverToolbars())
        {
            return;
        }

        e.Handled = true;

        if (_annotating)
        {
            SetDrawingTool(null);
            return;
        }

        if (_session.HasSelection)
        {
            _session.ClearSelection();
            return;
        }

        _session.CancelAll();
    }

    private void Window_KeyDown(object sender, KeyEventArgs e)
    {
        if (_closed || _textEditor is not null)
        {
            return;
        }

        if (e.Key == Key.Escape)
        {
            e.Handled = true;
            _session.CancelAll();
            return;
        }

        if (_hasSelection && _mode == CaptureSessionMode.Screenshot && IsSelectionHost)
        {
            if (e.Key == Key.Space)
            {
                e.Handled = true;
                SetDrawingTool(_annotating ? null : _annotationTool);
                return;
            }

            if (Keyboard.Modifiers == ModifierKeys.Shift && e.Key is Key.R or Key.E)
            {
                e.Handled = true;
                _shapeTool = e.Key == Key.R
                    ? AnnotationTool.Rectangle
                    : AnnotationTool.Ellipse;
                _fillShapes = true;
                SetDrawingTool(_shapeTool);
                return;
            }

            if (Keyboard.Modifiers == ModifierKeys.None)
            {
                if (e.Key == Key.V)
                {
                    e.Handled = true;
                    SetDrawingTool(null);
                    return;
                }

                if (GetToolForKey(e.Key) is { } tool)
                {
                    e.Handled = true;

                    if (tool is AnnotationTool.Rectangle or AnnotationTool.Ellipse)
                    {
                        _shapeTool = tool;
                        _fillShapes = false;
                    }

                    SetDrawingTool(tool);
                    return;
                }

                if (_annotating && e.Key == Key.F)
                {
                    e.Handled = true;
                    ToggleFill();
                    return;
                }

                if (_annotating && e.Key == Key.OemOpenBrackets)
                {
                    e.Handled = true;
                    AdjustBrush(-1);
                    return;
                }

                if (_annotating && e.Key == Key.OemCloseBrackets)
                {
                    e.Handled = true;
                    AdjustBrush(1);
                    return;
                }
            }

            if (_annotating &&
                _annotationTool == AnnotationTool.Text &&
                Keyboard.Modifiers == ModifierKeys.Control &&
                (e.Key == Key.B || e.Key == Key.I))
            {
                e.Handled = true;

                if (e.Key == Key.B)
                {
                    _textBold = !_textBold;
                }
                else
                {
                    _textItalic = !_textItalic;
                }

                UpdateToolOptionVisuals();
                ShowToolHud(e.Key == Key.B
                    ? (_textBold ? "加粗" : "取消加粗")
                    : (_textItalic ? "斜体" : "取消斜体"));
                return;
            }

            if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control) &&
                (e.Key == Key.Z || e.Key == Key.Y))
            {
                e.Handled = true;

                if (e.Key == Key.Y || Keyboard.Modifiers.HasFlag(ModifierKeys.Shift))
                {
                    RedoAnnotation();
                }
                else
                {
                    UndoAnnotation();
                }

                return;
            }

            if (Keyboard.Modifiers.HasFlag(ModifierKeys.Control) && e.Key == Key.T)
            {
                e.Handled = true;
                CommitTextEditor();
                _session.RequestAction(this, CaptureCommand.Pin);
                return;
            }
        }

        if (_hasSelection &&
            !_annotating &&
            e.Key is Key.Left or Key.Right or Key.Up or Key.Down)
        {
            e.Handled = true;
            NudgeSelection(e.Key);
            return;
        }

        if (!_hasSelection)
        {
            return;
        }

        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            RequestDefaultAction();
            return;
        }

        if (e.Key == Key.C && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            if (_mode != CaptureSessionMode.Record)
            {
                _session.RequestAction(this, CaptureCommand.Copy);
            }
            return;
        }

        if (e.Key == Key.S && Keyboard.Modifiers.HasFlag(ModifierKeys.Control))
        {
            e.Handled = true;
            if (_mode != CaptureSessionMode.Record)
            {
                _session.RequestAction(this, CaptureCommand.Save);
            }
        }
    }

    private void Window_MouseWheel(object sender, MouseWheelEventArgs e)
    {
        if (_closed || !_annotating || !_hasSelection || IsPointerOverToolbars())
        {
            return;
        }

        e.Handled = true;
        AdjustBrush(e.Delta > 0 ? 1 : -1);
    }

    private static AnnotationTool? GetToolForKey(Key key) => key switch
    {
        Key.R => AnnotationTool.Rectangle,
        Key.E => AnnotationTool.Ellipse,
        Key.A => AnnotationTool.Arrow,
        Key.P => AnnotationTool.Pen,
        Key.T => AnnotationTool.Text,
        Key.H => AnnotationTool.Highlighter,
        Key.M => AnnotationTool.Mosaic,
        Key.N => AnnotationTool.Step,
        Key.X => AnnotationTool.Eraser,
        _ => null
    };

    private void NudgeSelection(Key key)
    {
        if (!_session.HasSelection)
        {
            return;
        }

        var step = Keyboard.Modifiers.HasFlag(ModifierKeys.Shift) ? 10 : 1;
        var dx = key == Key.Left ? -step : key == Key.Right ? step : 0;
        var dy = key == Key.Up ? -step : key == Key.Down ? step : 0;

        _session.UpdateSelection(MoveVirtualSelection(_session.SelectionVirtual, dx, dy));
        RefreshMosaicTiles();
    }

    private void ActionButton_Click(object sender, RoutedEventArgs e)
    {
        if (_closed || sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        var command = tag switch
        {
            "Save" => CaptureCommand.Save,
            "Pin" => CaptureCommand.Pin,
            "Ocr" => CaptureCommand.Ocr,
            "StartRecording" => CaptureCommand.StartRecording,
            "Done" => CaptureCommand.Copy,
            _ => CaptureCommand.Cancel
        };

        if (command == CaptureCommand.Cancel)
        {
            _session.CancelAll();
            return;
        }

        if (!_hasSelection)
        {
            return;
        }

        CommitTextEditor();
        _session.RequestAction(this, command);
    }

    internal void RequestExternalAction(CaptureCommand command)
    {
        if (_closed || !_hasSelection || !IsSelectionHost)
        {
            return;
        }

        CommitTextEditor();
        _session.RequestAction(this, command);
    }

    private void RequestDefaultAction()
    {
        var command = _mode switch
        {
            CaptureSessionMode.Record => CaptureCommand.StartRecording,
            CaptureSessionMode.Ocr => CaptureCommand.Ocr,
            CaptureSessionMode.Pin => CaptureCommand.Pin,
            _ => CaptureCommand.Copy
        };

        CommitTextEditor();
        _session.RequestAction(this, command);
    }

    private void ResetToIdle()
    {
        if (_closed)
        {
            return;
        }

        _dragging = false;
        _dragKind = DragKind.None;
        _hasSelection = false;
        _selectionPx = default;

        ClearAnnotations();
        SetDrawingTool(null);

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放捕获异常。
        }

        HideToolbar();
        HideCrosshair();
        HideMagnifier();
        SizeBadge.Visibility = Visibility.Collapsed;
        SetActiveDim(false);
        SetFullDim(false);
        SelectionBorder.Visibility = Visibility.Collapsed;
        SetCornersVisible(false);
        AnnotationLayer.Visibility = Visibility.Collapsed;
        EditorLayer.Children.Clear();
        DimFull.Visibility = Visibility.Visible;
        DimTop.Visibility = Visibility.Collapsed;
        DimBottom.Visibility = Visibility.Collapsed;
        DimLeft.Visibility = Visibility.Collapsed;
        DimRight.Visibility = Visibility.Collapsed;
        ElementBorder.Visibility = Visibility.Collapsed;
        _hoverElementPx = default;
        _clickElementPx = default;
        HintChip.Visibility = Visibility.Visible;

        PositionCrosshairFromCursor();
    }

    private void UpdateSelectionVisual(bool showSize)
    {
        var width = Root.ActualWidth;
        var height = Root.ActualHeight;

        if (width <= 0.5 || height <= 0.5)
        {
            return;
        }

        var virtualPx = _session.HasSelection
            ? _session.SelectionVirtual
            : ScreenGeometry.ToVirtual(_selectionPx, _monitor.Bounds);

        var virtualLeft = (virtualPx.Left - _monitor.Bounds.Left) / _mapScaleX;
        var virtualTop = (virtualPx.Top - _monitor.Bounds.Top) / _mapScaleY;
        var virtualWidth = Math.Max(0, virtualPx.Width / _mapScaleX);
        var virtualHeight = Math.Max(0, virtualPx.Height / _mapScaleY);
        var virtualRight = virtualLeft + virtualWidth;
        var virtualBottom = virtualTop + virtualHeight;

        // 虚拟桌面坐标不能直接和本显示器的本地边界求交，否则带偏移的多屏会把遮罩开洞放到别处。
        var localPx = ScreenGeometry.Intersect(_selectionPx, MonitorLocalBounds);
        var left = localPx.Left / _mapScaleX;
        var top = localPx.Top / _mapScaleY;
        var selectionWidth = Math.Max(0, localPx.Width / _mapScaleX);
        var selectionHeight = Math.Max(0, localPx.Height / _mapScaleY);
        var right = left + selectionWidth;
        var bottom = top + selectionHeight;
        var hasLocal = !localPx.IsEmpty;
        SetActiveDim(hasLocal);

        // 选区还没面积时保留整屏遮罩：否则按下的瞬间桌面会整体变亮，屏幕上只剩下一个点。
        DimFull.Visibility = hasLocal ? Visibility.Collapsed : Visibility.Visible;

        var showOutsideDim = hasLocal;

        if (showOutsideDim)
        {
            UpdateDimRect(DimTop, 0, 0, width, top);
            UpdateDimRect(DimBottom, 0, bottom, width, Math.Max(0, height - bottom));
            UpdateDimRect(DimLeft, 0, top, left, selectionHeight);
            UpdateDimRect(DimRight, right, top, Math.Max(0, width - right), selectionHeight);
        }
        else
        {
            DimTop.Visibility = Visibility.Collapsed;
            DimBottom.Visibility = Visibility.Collapsed;
            DimLeft.Visibility = Visibility.Collapsed;
            DimRight.Visibility = Visibility.Collapsed;
        }

        var showChrome = _hasSelection || _dragging;
        SelectionBorder.Visibility = showChrome ? Visibility.Visible : Visibility.Collapsed;

        if (showChrome)
        {
            SelectionBorder.Width = virtualWidth;
            SelectionBorder.Height = virtualHeight;
            Canvas.SetLeft(SelectionBorder, virtualLeft);
            Canvas.SetTop(SelectionBorder, virtualTop);

            SetCornerPosition(CornerTopLeft, virtualLeft, virtualTop);
            SetCornerPosition(CornerTopRight, virtualRight, virtualTop);
            SetCornerPosition(CornerBottomLeft, virtualLeft, virtualBottom);
            SetCornerPosition(CornerBottomRight, virtualRight, virtualBottom);
            SetCornerPosition(EdgeTop, virtualLeft + (virtualWidth / 2), virtualTop);
            SetCornerPosition(EdgeBottom, virtualLeft + (virtualWidth / 2), virtualBottom);
            SetCornerPosition(EdgeLeft, virtualLeft, virtualTop + (virtualHeight / 2));
            SetCornerPosition(EdgeRight, virtualRight, virtualTop + (virtualHeight / 2));
            SetCornersVisible(!_annotating && showChrome);
        }
        else
        {
            SetCornersVisible(false);
        }

        if (showSize && hasLocal)
        {
            UpdateSizeBadge(virtualPx, left, top);
        }
        else if (!_dragging)
        {
            SizeBadge.Visibility = Visibility.Collapsed;
        }
    }

    private static void UpdateDimRect(
        FrameworkElement rectangle,
        double left,
        double top,
        double width,
        double height)
    {
        var visible = width > 0.5 && height > 0.5;
        rectangle.Visibility = visible ? Visibility.Visible : Visibility.Collapsed;
        rectangle.Width = Math.Max(0, width);
        rectangle.Height = Math.Max(0, height);
        Canvas.SetLeft(rectangle, left);
        Canvas.SetTop(rectangle, top);
    }

    private static void SetCornerPosition(FrameworkElement corner, double x, double y)
    {
        Canvas.SetLeft(corner, x - (corner.Width / 2));
        Canvas.SetTop(corner, y - (corner.Height / 2));
    }

    private void SetCornersVisible(bool visible)
    {
        var state = visible ? Visibility.Visible : Visibility.Collapsed;
        CornerTopLeft.Visibility = state;
        CornerTopRight.Visibility = state;
        CornerBottomLeft.Visibility = state;
        CornerBottomRight.Visibility = state;
        EdgeTop.Visibility = state;
        EdgeBottom.Visibility = state;
        EdgeLeft.Visibility = state;
        EdgeRight.Visibility = state;
    }

    private static Size MeasureWithoutMargin(FrameworkElement element)
    {
        element.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        var desired = element.DesiredSize;
        var margin = element.Margin;

        return new Size(
            Math.Max(0, desired.Width - margin.Left - margin.Right),
            Math.Max(0, desired.Height - margin.Top - margin.Bottom));
    }

    private void UpdateSizeBadge(PhysicalRect rect, double left, double top)
    {
        SizeText.Text = $"{rect.Width} × {rect.Height}";
        SizeBadge.Visibility = Visibility.Visible;

        var badge = MeasureWithoutMargin(SizeBadge);
        var badgeTop = top - badge.Height - 8;
        if (badgeTop < 6)
        {
            badgeTop = top + 9;
        }

        var badgeLeft = Math.Clamp(left, 6, Math.Max(6, Root.ActualWidth - badge.Width - 6));
        badgeTop = Math.Clamp(badgeTop, 6, Math.Max(6, Root.ActualHeight - badge.Height - 6));
        SizeBadge.Margin = new Thickness(badgeLeft, badgeTop, 0, 0);
    }

    private int HitTestSelectionHandle(Point dip)
    {
        if (!_hasSelection)
        {
            return 0;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var right = left + (_selectionPx.Width / _mapScaleX);
        var bottom = top + (_selectionPx.Height / _mapScaleY);

        var nearLeft = Math.Abs(dip.X - left) <= HandleHitRadius &&
                       dip.Y >= top - HandleHitRadius &&
                       dip.Y <= bottom + HandleHitRadius;
        var nearRight = Math.Abs(dip.X - right) <= HandleHitRadius &&
                        dip.Y >= top - HandleHitRadius &&
                        dip.Y <= bottom + HandleHitRadius;
        var nearTop = Math.Abs(dip.Y - top) <= HandleHitRadius &&
                      dip.X >= left - HandleHitRadius &&
                      dip.X <= right + HandleHitRadius;
        var nearBottom = Math.Abs(dip.Y - bottom) <= HandleHitRadius &&
                         dip.X >= left - HandleHitRadius &&
                         dip.X <= right + HandleHitRadius;

        return (nearLeft ? 1 : 0) |
               (nearRight ? 2 : 0) |
               (nearTop ? 4 : 0) |
               (nearBottom ? 8 : 0);
    }

    private bool IsInsideSelection(Point dip)
    {
        if (!_hasSelection)
        {
            return false;
        }

        var left = _selectionPx.Left / _mapScaleX;
        var top = _selectionPx.Top / _mapScaleY;
        var right = left + (_selectionPx.Width / _mapScaleX);
        var bottom = top + (_selectionPx.Height / _mapScaleY);

        return dip.X >= left && dip.X <= right && dip.Y >= top && dip.Y <= bottom;
    }

    private void UpdateSelectionCursor(Point dip)
    {
        var mask = HitTestSelectionHandle(dip);

        Root.Cursor = mask switch
        {
            1 | 4 or 2 | 8 => Cursors.SizeNWSE,
            2 | 4 or 1 | 8 => Cursors.SizeNESW,
            1 or 2 => Cursors.SizeWE,
            4 or 8 => Cursors.SizeNS,
            _ => IsInsideSelection(dip) ? Cursors.SizeAll : Cursors.Cross
        };
    }

    private void ShowToolbar()
    {
        Toolbar.Visibility = Visibility.Visible;
        PositionToolbar();

        var offset = new TranslateTransform(0, 6);
        _toolbarOffset = offset;
        Toolbar.RenderTransform = offset;
        Toolbar.Opacity = 0;

        var ease = new CubicEase { EasingMode = EasingMode.EaseOut };
        Toolbar.BeginAnimation(
            OpacityProperty,
            new DoubleAnimation(0, 1, TimeSpan.FromMilliseconds(120)) { EasingFunction = ease });
        offset.BeginAnimation(
            TranslateTransform.YProperty,
            new DoubleAnimation(6, 0, TimeSpan.FromMilliseconds(140)) { EasingFunction = ease });
    }

    private void PositionToolbar()
    {
        if (Toolbar.Visibility != Visibility.Visible || !_hasSelection)
        {
            return;
        }

        var selection = LocalSelection;
        if (selection.IsEmpty)
        {
            HideToolbar();
            return;
        }

        var desired = MeasureWithoutMargin(Toolbar);
        var selectionLeft = selection.Left / _mapScaleX;
        var selectionTop = selection.Top / _mapScaleY;
        var selectionRight = (selection.Left + selection.Width) / _mapScaleX;
        var selectionBottom = (selection.Top + selection.Height) / _mapScaleY;
        var rootWidth = Root.ActualWidth;
        var rootHeight = Root.ActualHeight;

        var left = selectionRight - desired.Width;
        var top = selectionBottom + 12;

        if (top + desired.Height > rootHeight - 8)
        {
            top = selectionTop - desired.Height - 12;
        }

        if (top < 8)
        {
            top = Math.Clamp(
                selectionBottom - desired.Height - 10,
                8,
                Math.Max(8, rootHeight - desired.Height - 8));
        }

        left = Math.Clamp(left, 8, Math.Max(8, rootWidth - desired.Width - 8));
        top = Math.Clamp(top, 8, Math.Max(8, rootHeight - desired.Height - 8));
        Toolbar.Margin = new Thickness(left, top, 0, 0);

        PositionOptionBar();
    }

    private void HideToolbar()
    {
        Toolbar.BeginAnimation(OpacityProperty, null);
        if (_toolbarOffset is not null)
        {
            _toolbarOffset.BeginAnimation(TranslateTransform.YProperty, null);
            _toolbarOffset = null;
        }

        Toolbar.Opacity = 1;
        Toolbar.Visibility = Visibility.Collapsed;
        OptionBar.Visibility = Visibility.Collapsed;
        HideToolHud();
    }

    private void UpdateCrosshair(Point point)
    {
        if (_hasSelection || _dragging || _annotating)
        {
            HideCrosshair();
            return;
        }

        if (point.X < 0 ||
            point.Y < 0 ||
            point.X > Root.ActualWidth ||
            point.Y > Root.ActualHeight)
        {
            HideCrosshair();
            return;
        }

        CrossVertical.Visibility = Visibility.Visible;
        CrossHorizontal.Visibility = Visibility.Visible;
        CrossVertical.Height = Root.ActualHeight;
        CrossHorizontal.Width = Root.ActualWidth;
        Canvas.SetLeft(CrossVertical, Math.Round(point.X));
        Canvas.SetTop(CrossVertical, 0);
        Canvas.SetLeft(CrossHorizontal, 0);
        Canvas.SetTop(CrossHorizontal, Math.Round(point.Y));
    }

    private void HideCrosshair()
    {
        CrossVertical.Visibility = Visibility.Collapsed;
        CrossHorizontal.Visibility = Visibility.Collapsed;
    }

    private void PositionCrosshairFromCursor()
    {
        if (_closed || _hasSelection || _dragging || _annotating)
        {
            return;
        }

        if (!NativeMethods.GetCursorPos(out var cursor))
        {
            return;
        }

        var point = new Point(
            (cursor.X - _monitor.Bounds.Left) / _mapScaleX,
            (cursor.Y - _monitor.Bounds.Top) / _mapScaleY);

        UpdateCrosshair(point);
        UpdateMagnifier(point);
        UpdateElementHover(point);
    }

    private void IdleHoverTick()
    {
        if (_closed)
        {
            _idleHoverTimer.Stop();
            return;
        }

        if (!_revealed || !IsVisible)
        {
            return;
        }

        if (_dragging)
        {
            if (!IsLeftButtonPressed())
            {
                if (_dragKind == DragKind.Create &&
                    !ScreenGeometry.IsUsableSelection(_session.SelectionVirtual, MinSelectionSizePx))
                {
                    CancelUnusableDrag();
                }
                else
                {
                    EndDrag();
                }
            }

            return;
        }

        if (_session.HasSelection)
        {
            if (!IsLeftButtonPressed())
            {
                DiscardIncompleteTinySelection();
            }

            return;
        }

        if (_hasSelection || _annotating)
        {
            return;
        }

        if (!IsCursorInsideMonitor())
        {
            if (ElementBorder.Visibility == Visibility.Visible ||
                CrossVertical.Visibility == Visibility.Visible)
            {
                ClearElementHover();
                HideCrosshair();
                HideMagnifier();
            }

            return;
        }

        if (ElementBorder.Visibility == Visibility.Visible && !_hoverElementPx.IsEmpty)
        {
            return;
        }

        PositionCrosshairFromCursor();
    }

    internal bool DiscardIncompleteTinySelection()
    {
        if (_closed ||
            _dragging ||
            !IsSelectionHost ||
            !_session.HasSelection ||
            _session.IsSelectionComplete ||
            ScreenGeometry.IsUsableSelection(_session.SelectionVirtual, MinSelectionSizePx))
        {
            return false;
        }

        Log.Info($"清除未成形的点选区：{_session.SelectionVirtual}");
        _session.ClearSelection();
        return true;
    }

    private void CancelUnusableDrag()
    {
        _dragging = false;
        _dragKind = DragKind.None;
        _clickElementPx = default;

        try
        {
            if (Mouse.Captured == Root)
            {
                Mouse.Capture(null);
            }
        }
        catch
        {
            // 忽略释放捕获异常。
        }

        Log.Info($"忽略未成形的点拖拽：{_session.SelectionVirtual}");
        _session.ClearSelection();
    }

    private bool IsCursorInsideMonitor()
    {
        if (!NativeMethods.GetCursorPos(out var cursor))
        {
            return false;
        }

        return cursor.X >= _monitor.Bounds.Left &&
               cursor.X < _monitor.Bounds.Right &&
               cursor.Y >= _monitor.Bounds.Top &&
               cursor.Y < _monitor.Bounds.Bottom;
    }

    private void SetActiveDim(bool active)
    {
        if (_activeDim == active)
        {
            return;
        }

        _activeDim = active;
        var brush = active ? SelectionDimBrush : HoverDimBrush;
        DimTop.Fill = brush;
        DimBottom.Fill = brush;
        DimLeft.Fill = brush;
        DimRight.Fill = brush;
    }

    private void SetFullDim(bool active)
    {
        var brush = active ? SelectionDimBrush : HoverDimBrush;
        if (!ReferenceEquals(DimFull.Fill, brush))
        {
            DimFull.Fill = brush;
        }
    }

    private static Brush CreateDimBrush(byte alpha)
    {
        var brush = new SolidColorBrush(Color.FromArgb(alpha, 0, 0, 0));
        brush.Freeze();
        return brush;
    }

    private bool IsPointerOverToolbars() =>
        (Toolbar.Visibility == Visibility.Visible && Toolbar.IsMouseOver) ||
        (OptionBar.Visibility == Visibility.Visible && OptionBar.IsMouseOver);

    private void UpdateMagnifier(Point point)
    {
        if (_closed ||
            _annotating ||
            (_hasSelection && _dragKind != DragKind.Resize) ||
            _frozenBitmap is null ||
            _frozenSource is null)
        {
            HideMagnifier();
            return;
        }

        if (point.X < 0 ||
            point.Y < 0 ||
            point.X > Root.ActualWidth ||
            point.Y > Root.ActualHeight)
        {
            HideMagnifier();
            return;
        }

        var pixelX = (int)Math.Round(point.X * _mapScaleX);
        var pixelY = (int)Math.Round(point.Y * _mapScaleY);

        if (pixelX < 0 ||
            pixelY < 0 ||
            pixelX >= _frozenBitmap.Width ||
            pixelY >= _frozenBitmap.Height)
        {
            HideMagnifier();
            return;
        }

        if (Magnifier.Visibility != Visibility.Visible)
        {
            EnsureMagnifierResources();
            Magnifier.Visibility = Visibility.Visible;
        }

        var color = RenderMagnifierFrame(pixelX, pixelY);
        var swatch = new SolidColorBrush(color);
        swatch.Freeze();
        MagnifierSwatch.Background = swatch;
        MagnifierHex.Text = $"#{color.R:X2}{color.G:X2}{color.B:X2}";
        MagnifierCoord.Text = $"{_monitor.Bounds.Left + pixelX}, {_monitor.Bounds.Top + pixelY}";

        var panel = MeasureWithoutMargin(Magnifier);
        var panelWidth = panel.Width;
        var panelHeight = panel.Height;
        var rootWidth = Root.ActualWidth;
        var rootHeight = Root.ActualHeight;
        const double gap = 24;

        var left = point.X + gap;
        if (left + panelWidth > rootWidth - 6)
        {
            left = point.X - panelWidth - gap;
        }

        var top = point.Y + gap;
        if (top + panelHeight > rootHeight - 6)
        {
            top = point.Y - panelHeight - gap;
        }

        left = Math.Clamp(left, 6, Math.Max(6, rootWidth - panelWidth - 6));
        top = Math.Clamp(top, 6, Math.Max(6, rootHeight - panelHeight - 6));
        Magnifier.Margin = new Thickness(left, top, 0, 0);
    }

    private void HideMagnifier()
    {
        if (Magnifier.Visibility == Visibility.Collapsed)
        {
            return;
        }

        Magnifier.Visibility = Visibility.Collapsed;
    }

    private void EnsureMagnifierResources()
    {
        if (_magnifierBitmap is not null || _frozenSource is null)
        {
            return;
        }

        var scale = _monitor.Scale > 0.1 ? _monitor.Scale : 1.0;
        _magnifierBlock = Math.Max(4, (int)Math.Round(8 * scale));
        var size = _magnifierBlock * MagnifierPixels;

        _magnifierSource = new FormatConvertedBitmap(_frozenSource, PixelFormats.Bgr32, null, 0);
        _magnifierSource.Freeze();

        _magnifierBitmap = new WriteableBitmap(size, size, 96, 96, PixelFormats.Bgr32, null);
        _magnifierSample = new byte[MagnifierPixels * MagnifierPixels * 4];
        _magnifierFrame = new byte[size * size * 4];
        MagnifierImage.Source = _magnifierBitmap;
    }

    private Color RenderMagnifierFrame(int pixelX, int pixelY)
    {
        EnsureMagnifierResources();

        if (_magnifierBitmap is null || _magnifierSource is null || _frozenBitmap is null)
        {
            return Colors.Black;
        }

        var half = MagnifierPixels / 2;
        var sampleLeft = pixelX - half;
        var sampleTop = pixelY - half;
        Array.Clear(_magnifierSample, 0, _magnifierSample.Length);

        var left = Math.Max(0, sampleLeft);
        var top = Math.Max(0, sampleTop);
        var right = Math.Min(_frozenBitmap.Width, sampleLeft + MagnifierPixels);
        var bottom = Math.Min(_frozenBitmap.Height, sampleTop + MagnifierPixels);

        if (right > left && bottom > top)
        {
            var width = right - left;
            var height = bottom - top;
            var stride = width * 4;
            var region = new byte[stride * height];
            _magnifierSource.CopyPixels(new Int32Rect(left, top, width, height), region, stride, 0);

            for (var row = 0; row < height; row++)
            {
                var destination = (((top - sampleTop) + row) * MagnifierPixels + (left - sampleLeft)) * 4;
                Buffer.BlockCopy(region, row * stride, _magnifierSample, destination, stride);
            }
        }

        var block = _magnifierBlock;
        var frameWidth = _magnifierBitmap.PixelWidth;

        for (var sampleY = 0; sampleY < MagnifierPixels; sampleY++)
        {
            for (var sampleX = 0; sampleX < MagnifierPixels; sampleX++)
            {
                var sourceIndex = ((sampleY * MagnifierPixels) + sampleX) * 4;
                var blue = _magnifierSample[sourceIndex];
                var green = _magnifierSample[sourceIndex + 1];
                var red = _magnifierSample[sourceIndex + 2];

                for (var y = 0; y < block; y++)
                {
                    var frameIndex = ((((sampleY * block) + y) * frameWidth) + (sampleX * block)) * 4;

                    for (var x = 0; x < block; x++)
                    {
                        _magnifierFrame[frameIndex] = blue;
                        _magnifierFrame[frameIndex + 1] = green;
                        _magnifierFrame[frameIndex + 2] = red;
                        _magnifierFrame[frameIndex + 3] = 255;
                        frameIndex += 4;
                    }
                }
            }
        }

        _magnifierBitmap.WritePixels(
            new Int32Rect(0, 0, frameWidth, _magnifierBitmap.PixelHeight),
            _magnifierFrame,
            frameWidth * 4,
            0);

        var centerIndex = ((half * MagnifierPixels) + half) * 4;
        return Color.FromRgb(
            _magnifierSample[centerIndex + 2],
            _magnifierSample[centerIndex + 1],
            _magnifierSample[centerIndex]);
    }

    private void UpdateElementHover(Point point)
    {
        if (_hasSelection || _dragging || _annotating)
        {
            ClearElementHover();
            return;
        }

        if (point.X < 0 ||
            point.Y < 0 ||
            point.X > Root.ActualWidth ||
            point.Y > Root.ActualHeight)
        {
            ClearElementHover();
            return;
        }

        var screenX = _monitor.Bounds.Left + (int)Math.Round(point.X * _mapScaleX);
        var screenY = _monitor.Bounds.Top + (int)Math.Round(point.Y * _mapScaleY);
        var now = Environment.TickCount64;

        if (now - _lastElementProbeTick < ElementProbeIntervalMs &&
            Math.Abs(screenX - _lastProbeX) < 2 &&
            Math.Abs(screenY - _lastProbeY) < 2)
        {
            return;
        }

        _lastElementProbeTick = now;
        _lastProbeX = screenX;
        _lastProbeY = screenY;

        PhysicalRect? region;

        try
        {
            region = WindowProbe.FindRegionUnderPoint(screenX, screenY, _monitor.Bounds, _handle);
        }
        catch
        {
            region = null;
        }

        ShowElementHoverOrFullScreen(region);
    }

    private void ShowElementHoverOrFullScreen(PhysicalRect? region)
    {
        // 没有识别到可见窗口时退回整屏，单击即可直接框选全屏。
        ShowElementHover(region ?? MonitorLocalBounds);

        if (!_initialHoverLogged)
        {
            _initialHoverLogged = true;
            Log.Info(
                $"选区启动候选：monitor={_monitor.Bounds} cursor={_lastProbeX},{_lastProbeY} source={(region is null ? "screen" : "window")} region={_hoverElementPx}");
        }
    }

    private void ShowElementHover(PhysicalRect rect)
    {
        if (_hoverElementPx.Equals(rect) && ElementBorder.Visibility == Visibility.Visible)
        {
            return;
        }

        SetActiveDim(false);
        _hoverElementPx = rect;

        var left = rect.Left / _mapScaleX;
        var top = rect.Top / _mapScaleY;
        var right = Math.Min(Root.ActualWidth, left + (rect.Width / _mapScaleX));
        var bottom = Math.Min(Root.ActualHeight, top + (rect.Height / _mapScaleY));

        left = Math.Clamp(left, 0, Root.ActualWidth);
        top = Math.Clamp(top, 0, Root.ActualHeight);

        var width = Math.Max(0, right - left);
        var height = Math.Max(0, bottom - top);

        DimFull.Visibility = Visibility.Collapsed;
        UpdateDimRect(DimTop, 0, 0, Root.ActualWidth, top);
        UpdateDimRect(DimBottom, 0, bottom, Root.ActualWidth, Math.Max(0, Root.ActualHeight - bottom));
        UpdateDimRect(DimLeft, 0, top, left, height);
        UpdateDimRect(DimRight, right, top, Math.Max(0, Root.ActualWidth - right), height);

        ElementBorder.Visibility = Visibility.Visible;
        ElementBorder.Width = width;
        ElementBorder.Height = height;
        Canvas.SetLeft(ElementBorder, left);
        Canvas.SetTop(ElementBorder, top);

        UpdateSizeBadge(rect, left, top);
    }

    private void ClearElementHover()
    {
        if (ElementBorder.Visibility == Visibility.Collapsed && _hoverElementPx.Width == 0)
        {
            return;
        }

        _hoverElementPx = default;
        ElementBorder.Visibility = Visibility.Collapsed;
        SizeBadge.Visibility = Visibility.Collapsed;

        if (!_hasSelection && !_dragging && !_annotating)
        {
            SetActiveDim(false);
            DimFull.Visibility = Visibility.Visible;
            DimTop.Visibility = Visibility.Collapsed;
            DimBottom.Visibility = Visibility.Collapsed;
            DimLeft.Visibility = Visibility.Collapsed;
            DimRight.Visibility = Visibility.Collapsed;
        }
    }

    private PhysicalRect ToLocalPhysical(Point dipPoint)
    {
        var x = (int)Math.Round(dipPoint.X * _mapScaleX);
        var y = (int)Math.Round(dipPoint.Y * _mapScaleY);
        return new PhysicalRect(x, y, 0, 0);
    }

    private void UpdateAnnotationFrame()
    {
        if (!_hasSelection || !IsSelectionHost)
        {
            AnnotationLayer.Visibility = Visibility.Collapsed;
            return;
        }

        var local = LocalSelection;
        if (local.IsEmpty)
        {
            AnnotationLayer.Visibility = Visibility.Collapsed;
            return;
        }

        // 标注画布覆盖整个屏幕，选区只是输出范围；
        // 通过原点偏移让标注坐标仍以选区左上角为 (0,0)。
        var originX = local.Left / _mapScaleX;
        var originY = local.Top / _mapScaleY;

        AnnotationLayer.Visibility = Visibility.Visible;
        Canvas.SetLeft(AnnotationView, 0);
        Canvas.SetTop(AnnotationView, 0);
        AnnotationView.Width = Root.ActualWidth;
        AnnotationView.Height = Root.ActualHeight;
        AnnotationView.SetOrigin(originX, originY);
        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void SetDrawingTool(AnnotationTool? tool)
    {
        if (tool is not null &&
            (!_session.HasSelection || _mode != CaptureSessionMode.Screenshot || !IsSelectionHost))
        {
            return;
        }

        if (!_annotating && tool is null)
        {
            return;
        }

        if (tool is not null)
        {
            if (_textEditor is not null)
            {
                CommitTextEditor();
            }

            if (tool is AnnotationTool.Rectangle or AnnotationTool.Ellipse)
            {
                _shapeTool = tool.Value;
            }

            _annotationTool = tool.Value;
            _annotating = true;
            HideMagnifier();
            ClearElementHover();
            Root.Cursor = Cursors.Cross;
            HideCrosshair();
        }
        else
        {
            CancelTextEditor();
            CancelLiveStroke();
            _annotating = false;
            Root.Cursor = Cursors.Arrow;
        }

        UpdateAnnotationToolVisuals();
        UpdateHistoryVisuals();
        SetCornersVisible(_hasSelection && !_annotating);

        if (_hasSelection)
        {
            UpdateAnnotationFrame();
            PositionToolbar();
        }

        Keyboard.Focus(Root);
    }

    private void AnnotationTool_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        if (tag == "Select")
        {
            SetDrawingTool(null);
            Keyboard.Focus(Root);
            return;
        }

        if (tag == "Shape")
        {
            SetDrawingTool(_shapeTool);
            Keyboard.Focus(Root);
            return;
        }

        if (!Enum.TryParse<AnnotationTool>(tag, out var tool))
        {
            return;
        }

        SetDrawingTool(tool);
        Keyboard.Focus(Root);
    }

    private void AnnotationColor_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        _annotationColor = UiColors.Parse(tag);
        UpdateAnnotationColorVisuals();
        Keyboard.Focus(Root);
    }

    private void AnnotationAction_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        switch (tag)
        {
            case "Undo":
                UndoAnnotation();
                break;

            case "Redo":
                RedoAnnotation();
                break;

            case "Clear":
                ClearAnnotationStrokes();
                break;
        }

        Keyboard.Focus(Root);
    }

    private void UpdateAnnotationToolVisuals()
    {
        var accent = UiColors.Brush("#FF2F7CF6");
        var normal = UiColors.Brush("#FF3C4250");

        ToolSelectButton.Background = _annotating ? Brushes.Transparent : accent;
        ToolSelectButton.Foreground = _annotating ? normal : Brushes.White;

        var shapeActive = _annotating && _annotationTool is
            AnnotationTool.Rectangle or AnnotationTool.Ellipse;
        ToolShapeButton.Background = shapeActive ? accent : Brushes.Transparent;
        ToolShapeButton.Foreground = shapeActive ? Brushes.White : normal;

        var buttons = new (Button Button, AnnotationTool Tool)[]
        {
            (ToolArrowButton, AnnotationTool.Arrow),
            (ToolPenButton, AnnotationTool.Pen),
            (ToolHighlighterButton, AnnotationTool.Highlighter),
            (ToolEraserButton, AnnotationTool.Eraser),
            (ToolTextButton, AnnotationTool.Text),
            (ToolMosaicButton, AnnotationTool.Mosaic),
            (ToolStepButton, AnnotationTool.Step)
        };

        foreach (var (button, tool) in buttons)
        {
            var active = _annotating && tool == _annotationTool;
            button.Background = active ? accent : Brushes.Transparent;
            button.Foreground = active ? Brushes.White : normal;
        }

        UpdateToolOptionVisuals();
    }

    private void UpdateToolOptionVisuals()
    {
        var isShape = _annotationTool is AnnotationTool.Rectangle or AnnotationTool.Ellipse;
        var colorTool = _annotating &&
                        _annotationTool is not (AnnotationTool.Mosaic or AnnotationTool.Eraser);
        OptionColorGroup.Visibility = colorTool ? Visibility.Visible : Visibility.Collapsed;
        OptionColorSeparator.Visibility = colorTool ? Visibility.Visible : Visibility.Collapsed;

        ShapeKindGroup.Visibility = _annotating && isShape
            ? Visibility.Visible
            : Visibility.Collapsed;
        ShapeOptionGroup.Visibility = _annotating && isShape
            ? Visibility.Visible
            : Visibility.Collapsed;
        BrushOptionGroup.Visibility = _annotating &&
                                      _annotationTool is AnnotationTool.Pen or AnnotationTool.Arrow
                                          or AnnotationTool.Highlighter or AnnotationTool.Rectangle
                                          or AnnotationTool.Ellipse
            ? Visibility.Visible
            : Visibility.Collapsed;
        ArrowOptionGroup.Visibility = _annotating && _annotationTool == AnnotationTool.Arrow
            ? Visibility.Visible
            : Visibility.Collapsed;
        TextOptionGroup.Visibility = _annotating && _annotationTool == AnnotationTool.Text
            ? Visibility.Visible
            : Visibility.Collapsed;
        TextStyleOptionGroup.Visibility = _annotating && _annotationTool == AnnotationTool.Text
            ? Visibility.Visible
            : Visibility.Collapsed;
        MosaicOptionGroup.Visibility = _annotating && _annotationTool == AnnotationTool.Mosaic
            ? Visibility.Visible
            : Visibility.Collapsed;
        EraserOptionGroup.Visibility = _annotating && _annotationTool == AnnotationTool.Eraser
            ? Visibility.Visible
            : Visibility.Collapsed;

        var accent = UiColors.Brush("#FF2F7CF6");
        var normal = UiColors.Brush("#FF3C4250");
        SetOptionButtonState(ShapeRectangleButton, _annotationTool == AnnotationTool.Rectangle, accent, normal);
        SetOptionButtonState(ShapeEllipseButton, _annotationTool == AnnotationTool.Ellipse, accent, normal);
        SetOptionButtonState(ShapeOutlineButton, !_fillShapes, accent, normal);
        SetOptionButtonState(ShapeFillButton, _fillShapes, accent, normal);

        SetOptionButtonState(ArrowEndButton, _arrowHead == ArrowHeadMode.End, accent, normal);
        SetOptionButtonState(ArrowBothButton, _arrowHead == ArrowHeadMode.Both, accent, normal);
        SetOptionButtonState(ArrowLineButton, _arrowHead == ArrowHeadMode.None, accent, normal);
        SetOptionButtonState(BoldToggleButton, _textBold, accent, normal);
        SetOptionButtonState(ItalicToggleButton, _textItalic, accent, normal);

        BrushValueText.Text = (_annotationTool == AnnotationTool.Highlighter
            ? _highlighterThickness
            : _strokeThickness).ToString("0");
        TextValueText.Text = _textSize.ToString("0");
        MosaicValueText.Text = _mosaicBlockDip.ToString();
        EraserValueText.Text = _eraserSize.ToString("0");

        OptionBar.Visibility = _annotating && _hasSelection
            ? Visibility.Visible
            : Visibility.Collapsed;

        PositionOptionBar();
    }

    private static void SetOptionButtonState(Button button, bool active, Brush accent, Brush normal)
    {
        button.Background = active ? accent : Brushes.Transparent;
        button.Foreground = active ? Brushes.White : normal;
    }

    private void PositionOptionBar()
    {
        if (OptionBar.Visibility != Visibility.Visible)
        {
            return;
        }

        var size = MeasureWithoutMargin(OptionBar);
        var toolbarSize = MeasureWithoutMargin(Toolbar);
        var toolbarMargin = Toolbar.Margin;
        var left = toolbarMargin.Left;
        var top = toolbarMargin.Top + toolbarSize.Height + 6;

        if (top + size.Height > Root.ActualHeight - 8)
        {
            top = toolbarMargin.Top - size.Height - 6;
        }

        left = Math.Clamp(left, 8, Math.Max(8, Root.ActualWidth - size.Width - 8));
        top = Math.Clamp(top, 8, Math.Max(8, Root.ActualHeight - size.Height - 8));
        OptionBar.Margin = new Thickness(left, top, 0, 0);
    }

    private void ShapeKind_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        var shape = tag == "Ellipse" ? AnnotationTool.Ellipse : AnnotationTool.Rectangle;
        _shapeTool = shape;
        _annotationTool = shape;
        _annotating = true;
        UpdateAnnotationToolVisuals();
        UpdateHistoryVisuals();
        SetCornersVisible(false);
        UpdateAnnotationFrame();
        PositionToolbar();
        ShowToolHud(tag == "Ellipse" ? "椭圆" : "矩形");
        Keyboard.Focus(Root);
    }

    private void ShapeStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        _fillShapes = tag == "Fill";
        UpdateAnnotationToolVisuals();
        ShowToolHud(_fillShapes ? "实心填充" : "空心");
        Keyboard.Focus(Root);
    }

    private void ToggleFill()
    {
        _fillShapes = !_fillShapes;
        UpdateAnnotationToolVisuals();
        ShowToolHud(_fillShapes ? "实心填充" : "空心");
    }

    private void ArrowStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element ||
            element.Tag is not string tag ||
            !Enum.TryParse<ArrowHeadMode>(tag, out var mode))
        {
            return;
        }

        e.Handled = true;
        _arrowHead = mode;
        UpdateToolOptionVisuals();
        ShowToolHud(mode switch
        {
            ArrowHeadMode.Both => "双向箭头",
            ArrowHeadMode.None => "直线",
            _ => "末端箭头"
        });
        Keyboard.Focus(Root);
    }

    private void TextStyle_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;

        switch (tag)
        {
            case "Bold":
                _textBold = !_textBold;
                break;

            case "Italic":
                _textItalic = !_textItalic;
                break;
        }

        if (_textEditor is not null)
        {
            _textEditor.FontWeight = _textBold ? FontWeights.Bold : FontWeights.Normal;
            _textEditor.FontStyle = _textItalic ? FontStyles.Italic : FontStyles.Normal;
        }

        UpdateToolOptionVisuals();
        ShowToolHud(tag == "Bold"
            ? (_textBold ? "加粗" : "取消加粗")
            : (_textItalic ? "斜体" : "取消斜体"));
        Keyboard.Focus(Root);
    }

    private void OptionStep_Click(object sender, RoutedEventArgs e)
    {
        if (sender is not FrameworkElement element || element.Tag is not string tag)
        {
            return;
        }

        e.Handled = true;
        AdjustBrush(tag == "Up" ? 1 : -1);
        Keyboard.Focus(Root);
    }

    private void UpdateAnnotationColorVisuals()
    {
        var activeRing = UiColors.Brush("#FF2A303B");

        var buttons = new (Button Button, string Hex)[]
        {
            (ColorRedButton, "#FFFF5A5A"),
            (ColorOrangeButton, "#FFFFA94D"),
            (ColorYellowButton, "#FFFFD166"),
            (ColorGreenButton, "#FF7CE38B"),
            (ColorBlueButton, "#FF6CA8FF"),
            (ColorPurpleButton, "#FFB08CFF"),
            (ColorDarkButton, "#FF242933"),
            (ColorWhiteButton, "#FFFFFFFF")
        };

        foreach (var (button, hex) in buttons)
        {
            var color = UiColors.Parse(hex);
            button.BorderBrush = color == _annotationColor ? activeRing : Brushes.Transparent;
        }
    }

    private void BeginAnnotationStroke(Point rootPoint)
    {
        var point = RootToSelectionDip(rootPoint);

        if (_annotationTool == AnnotationTool.Eraser)
        {
            BeginErase(point);
            return;
        }

        if (_annotationTool == AnnotationTool.Step)
        {
            PlaceStep(point);
            return;
        }

        if (_annotationTool == AnnotationTool.Text)
        {
            BeginTextEditor(rootPoint, point);
            return;
        }

        _liveStroke = new AnnotationStroke
        {
            Tool = _annotationTool,
            Color = _annotationColor,
            Filled = _fillShapes,
            Thickness = _annotationTool == AnnotationTool.Highlighter
                ? _highlighterThickness
                : _strokeThickness,
            TextSize = _textSize,
            Bold = _textBold,
            Italic = _textItalic,
            ArrowHead = _arrowHead
        };

        if (_annotationTool == AnnotationTool.Mosaic)
        {
            _mosaicStart = point;
            _mosaicLastRect = default;
            _liveStroke.Points.Add(point);
            _liveStroke.Points.Add(point);
            UpdateMosaicStroke(_liveStroke, point);
        }
        else
        {
            _liveStroke.Points.Add(point);
        }

        _drawingAnnotation = true;
        Mouse.Capture(Root);
        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void UpdateAnnotationStroke(Point rootPoint)
    {
        if (_annotationTool == AnnotationTool.Eraser)
        {
            EraseAt(RootToSelectionDip(rootPoint));
            return;
        }

        if (_liveStroke is null)
        {
            return;
        }

        var point = RootToSelectionDip(rootPoint);
        var shift = Keyboard.Modifiers.HasFlag(ModifierKeys.Shift);

        if (_annotationTool == AnnotationTool.Mosaic)
        {
            UpdateMosaicStroke(
                _liveStroke,
                shift ? AnnotationGeometry.ConstrainSquare(_mosaicStart, point) : point);
            return;
        }

        if (_annotationTool is AnnotationTool.Pen or AnnotationTool.Highlighter)
        {
            if (shift)
            {
                if (_liveStroke.Points.Count > 2)
                {
                    _liveStroke.Points.RemoveRange(2, _liveStroke.Points.Count - 2);
                }

                if (_liveStroke.Points.Count == 1)
                {
                    _liveStroke.Points.Add(point);
                }
                else
                {
                    _liveStroke.Points[1] = point;
                }
            }
            else
            {
                var last = _liveStroke.Points[_liveStroke.Points.Count - 1];
                if ((point - last).Length < 0.8)
                {
                    return;
                }

                _liveStroke.Points.Add(point);
            }
        }
        else
        {
            var end = point;

            if (shift && _liveStroke.Points.Count > 0)
            {
                end = _annotationTool == AnnotationTool.Arrow
                    ? AnnotationGeometry.SnapToAngle(_liveStroke.Points[0], point)
                    : AnnotationGeometry.ConstrainSquare(_liveStroke.Points[0], point);
            }

            if (_liveStroke.Points.Count == 1)
            {
                _liveStroke.Points.Add(end);
            }
            else
            {
                _liveStroke.Points[1] = end;
            }
        }

        AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
    }

    private void FinishAnnotationStroke()
    {
        _drawingAnnotation = false;
        _eraseHistoryPushed = false;
        Mouse.Capture(null);

        var stroke = _liveStroke;
        _liveStroke = null;

        if (stroke is not null && IsMeaningfulStroke(stroke))
        {
            PushHistory();
            _strokes.Add(stroke);
        }

        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
    }

    private void BeginErase(Point point)
    {
        _drawingAnnotation = true;
        _eraseHistoryPushed = false;
        Mouse.Capture(Root);
        EraseAt(point);
    }

    private void EraseAt(Point point)
    {
        var radius = _eraserSize / 2;
        var removed = false;

        for (var i = _strokes.Count - 1; i >= 0; i--)
        {
            if (!AnnotationHitTester.HitTest(_strokes[i], point, radius))
            {
                continue;
            }

            if (!_eraseHistoryPushed)
            {
                PushHistory();
                _eraseHistoryPushed = true;
            }

            _strokes.RemoveAt(i);
            removed = true;
        }

        if (!removed)
        {
            return;
        }

        RefreshMosaicTiles();
        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
        ShowToolHud("已擦除标注");
    }

    private void PlaceStep(Point point)
    {
        var radius = AnnotationDefaults.StepRadius;
        var selection = LocalSelection;
        var minX = (-selection.Left / _mapScaleX) + radius;
        var minY = (-selection.Top / _mapScaleY) + radius;
        var maxX = ((_monitor.Bounds.Width - selection.Left) / _mapScaleX) - radius;
        var maxY = ((_monitor.Bounds.Height - selection.Top) / _mapScaleY) - radius;
        var center = new Point(
            maxX >= minX ? Math.Clamp(point.X, minX, maxX) : point.X,
            maxY >= minY ? Math.Clamp(point.Y, minY, maxY) : point.Y);

        var number = 1;

        foreach (var stroke in _strokes)
        {
            if (stroke.Tool == AnnotationTool.Step &&
                int.TryParse(stroke.Text, out var existing) &&
                existing >= number)
            {
                number = existing + 1;
            }
        }

        var step = new AnnotationStroke
        {
            Tool = AnnotationTool.Step,
            Color = _annotationColor,
            Text = number.ToString()
        };

        step.Points.Add(center);
        PushHistory();
        _strokes.Add(step);
        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
        ShowToolHud($"序号 {number}");
    }

    private void UpdateMosaicStroke(AnnotationStroke stroke, Point current)
    {
        if (_frozenSource is null)
        {
            return;
        }

        var rect = AnnotationGeometry.NormalizeRect(_mosaicStart, current);

        if (rect.Width < 0.5 || rect.Height < 0.5)
        {
            return;
        }

        var scaleX = _mapScaleX;
        var scaleY = _mapScaleY;
        var selection = LocalSelection;
        var left = selection.Left + (int)Math.Round(rect.Left * scaleX);
        var top = selection.Top + (int)Math.Round(rect.Top * scaleY);
        var right = selection.Left + (int)Math.Round(rect.Right * scaleX);
        var bottom = selection.Top + (int)Math.Round(rect.Bottom * scaleY);
        var blockPixels = Math.Max(4, (int)Math.Round(_mosaicBlockDip * scaleX));
        var limit = new PhysicalRect(0, 0, _monitor.Bounds.Width, _monitor.Bounds.Height);
        var snapped = MosaicRenderer.SnapToBlocks(left, top, right, bottom, limit, blockPixels);

        if (snapped.Width <= 0 || snapped.Height <= 0)
        {
            return;
        }

        if (snapped.Equals(_mosaicLastRect) && stroke.Mosaic is not null)
        {
            return;
        }

        var tile = CreateMosaicTile(snapped, blockPixels, _strokes);

        if (tile is null)
        {
            return;
        }

        _mosaicLastRect = snapped;
        stroke.Mosaic = tile;
        stroke.MosaicBlockPixels = blockPixels;
        stroke.Points[0] = new Point(
            (snapped.X - selection.Left) / scaleX,
            (snapped.Y - selection.Top) / scaleY);
        stroke.Points[1] = new Point(
            (snapped.X + snapped.Width - selection.Left) / scaleX,
            (snapped.Y + snapped.Height - selection.Top) / scaleY);
        AnnotationView.SetContent(_strokes, stroke, 1.0);
    }

    private BitmapSource? CreateMosaicTile(
        Int32Rect snapped,
        int blockPixels,
        IReadOnlyList<AnnotationStroke> priorStrokes)
    {
        if (_frozenSource is null)
        {
            return null;
        }

        var selection = LocalSelection;
        var transform = new Matrix(
            _mapScaleX,
            0,
            0,
            _mapScaleY,
            selection.Left - snapped.X,
            selection.Top - snapped.Y);

        return MosaicRenderer.CreateCompositedTile(
            _frozenSource,
            snapped,
            priorStrokes,
            transform,
            blockPixels);
    }

    private bool RefreshMosaicTiles()
    {
        if (_frozenSource is null)
        {
            return false;
        }

        var limit = new PhysicalRect(0, 0, _monitor.Bounds.Width, _monitor.Bounds.Height);
        var prior = new List<AnnotationStroke>();
        var changed = false;
        var selection = LocalSelection;

        foreach (var stroke in _strokes)
        {
            if (stroke.Tool == AnnotationTool.Mosaic &&
                stroke.Points.Count >= 2 &&
                stroke.MosaicBlockPixels > 0)
            {
                var left = selection.Left + (int)Math.Round(stroke.Points[0].X * _mapScaleX);
                var top = selection.Top + (int)Math.Round(stroke.Points[0].Y * _mapScaleY);
                var right = selection.Left + (int)Math.Round(stroke.Points[1].X * _mapScaleX);
                var bottom = selection.Top + (int)Math.Round(stroke.Points[1].Y * _mapScaleY);
                var snapped = MosaicRenderer.SnapToBlocks(
                    left,
                    top,
                    right,
                    bottom,
                    limit,
                    stroke.MosaicBlockPixels);

                if (snapped.Width > 0 && snapped.Height > 0)
                {
                    var tile = CreateMosaicTile(snapped, stroke.MosaicBlockPixels, prior);

                    if (tile is not null)
                    {
                        stroke.Mosaic = tile;
                        stroke.Points[0] = new Point(
                            (snapped.X - selection.Left) / _mapScaleX,
                            (snapped.Y - selection.Top) / _mapScaleY);
                        stroke.Points[1] = new Point(
                            (snapped.X + snapped.Width - selection.Left) / _mapScaleX,
                            (snapped.Y + snapped.Height - selection.Top) / _mapScaleY);
                        changed = true;
                    }
                }
            }

            prior.Add(stroke);
        }

        return changed;
    }

    private void AdjustBrush(int direction)
    {
        if (!_annotating)
        {
            return;
        }

        string label;

        switch (_annotationTool)
        {
            case AnnotationTool.Text:
                _textSize = Math.Clamp(
                    _textSize + (direction * 2),
                    AnnotationDefaults.MinTextSize,
                    AnnotationDefaults.MaxTextSize);
                label = $"字号 {_textSize:0}";
                break;

            case AnnotationTool.Highlighter:
                _highlighterThickness = Math.Clamp(
                    _highlighterThickness + (direction * 2),
                    AnnotationDefaults.MinHighlighterThickness,
                    AnnotationDefaults.MaxHighlighterThickness);
                label = $"荧光笔 {_highlighterThickness:0}";
                break;

            case AnnotationTool.Mosaic:
                _mosaicBlockDip = Math.Clamp(
                    _mosaicBlockDip + (direction * 2),
                    AnnotationDefaults.MinMosaicBlockDip,
                    AnnotationDefaults.MaxMosaicBlockDip);
                _mosaicLastRect = default;
                label = $"马赛克 {_mosaicBlockDip}";
                break;

            case AnnotationTool.Step:
                return;

            case AnnotationTool.Eraser:
                _eraserSize = Math.Clamp(
                    _eraserSize + (direction * 2),
                    AnnotationDefaults.MinEraserSize,
                    AnnotationDefaults.MaxEraserSize);
                label = $"橡皮 {_eraserSize:0}";
                break;

            default:
                _strokeThickness = Math.Clamp(
                    _strokeThickness + direction,
                    AnnotationDefaults.MinThickness,
                    AnnotationDefaults.MaxThickness);
                label = $"粗细 {_strokeThickness:0}";
                break;
        }

        if (_liveStroke is not null &&
            _annotationTool is not AnnotationTool.Mosaic and not AnnotationTool.Text)
        {
            _liveStroke.Thickness = _annotationTool == AnnotationTool.Highlighter
                ? _highlighterThickness
                : _strokeThickness;
            AnnotationView.SetContent(_strokes, _liveStroke, 1.0);
        }

        ShowToolHud(label);
        UpdateToolOptionVisuals();
    }

    private void ShowToolHud(string text)
    {
        ToolHudText.Text = text;
        ToolHud.Visibility = Visibility.Visible;
        ToolHud.BeginAnimation(OpacityProperty, null);
        ToolHud.Opacity = 1;
        _toolHudTimer.Stop();
        _toolHudTimer.Start();
        PositionToolHud();
    }

    private void PositionToolHud()
    {
        var size = MeasureWithoutMargin(ToolHud);
        var rootWidth = Root.ActualWidth;
        var rootHeight = Root.ActualHeight;
        Point? cursor = null;

        if (NativeMethods.GetCursorPos(out var raw))
        {
            var x = (raw.X - _monitor.Bounds.Left) / _mapScaleX;
            var y = (raw.Y - _monitor.Bounds.Top) / _mapScaleY;

            if (x >= 0 && y >= 0 && x <= rootWidth && y <= rootHeight)
            {
                cursor = new Point(x, y);
            }
        }

        double left;
        double top;

        if (cursor is { } point)
        {
            left = point.X + 18;
            top = point.Y + 24;

            if (left + size.Width > rootWidth - 8)
            {
                left = point.X - size.Width - 18;
            }

            if (top + size.Height > rootHeight - 8)
            {
                top = point.Y - size.Height - 18;
            }
        }
        else
        {
            left = Toolbar.Margin.Left;
            top = Toolbar.Margin.Top - size.Height - 8;
        }

        left = Math.Clamp(left, 8, Math.Max(8, rootWidth - size.Width - 8));
        top = Math.Clamp(top, 8, Math.Max(8, rootHeight - size.Height - 8));
        ToolHud.Margin = new Thickness(left, top, 0, 0);
    }

    private void FadeToolHudOut()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(160))
        {
            EasingFunction = new CubicEase { EasingMode = EasingMode.EaseIn }
        };

        fade.Completed += (_, _) => ToolHud.Visibility = Visibility.Collapsed;
        ToolHud.BeginAnimation(OpacityProperty, fade);
    }

    private void HideToolHud()
    {
        _toolHudTimer.Stop();
        ToolHud.BeginAnimation(OpacityProperty, null);
        ToolHud.Opacity = 0;
        ToolHud.Visibility = Visibility.Collapsed;
    }

    private static bool IsMeaningfulStroke(AnnotationStroke stroke)
    {
        if (stroke.Tool is AnnotationTool.Eraser)
        {
            return false;
        }

        if (stroke.Tool == AnnotationTool.Step)
        {
            return stroke.Points.Count > 0;
        }

        if (stroke.Tool == AnnotationTool.Mosaic)
        {
            if (stroke.Mosaic is null || stroke.Points.Count < 2)
            {
                return false;
            }

            var mosaicRect = AnnotationGeometry.NormalizeRect(stroke.Points[0], stroke.Points[1]);
            return mosaicRect.Width >= 4 && mosaicRect.Height >= 4;
        }

        if (stroke.Points.Count < 2)
        {
            return false;
        }

        if (stroke.Tool is AnnotationTool.Pen or AnnotationTool.Highlighter)
        {
            return true;
        }

        var vector = stroke.Points[1] - stroke.Points[0];
        return Math.Abs(vector.X) > 2 || Math.Abs(vector.Y) > 2;
    }

    private Point RootToSelectionDip(Point rootPoint)
    {
        var selection = LocalSelection;
        var left = selection.Left / _mapScaleX;
        var top = selection.Top / _mapScaleY;
        return new Point(rootPoint.X - left, rootPoint.Y - top);
    }

    private void UndoAnnotation()
    {
        if (_liveStroke is not null)
        {
            CancelLiveStroke();
            return;
        }

        if (_undoStack.Count == 0)
        {
            return;
        }

        _redoStack.Add(new List<AnnotationStroke>(_strokes));
        var previous = _undoStack[_undoStack.Count - 1];
        _undoStack.RemoveAt(_undoStack.Count - 1);
        RestoreStrokes(previous);
    }

    private void RedoAnnotation()
    {
        if (_redoStack.Count == 0)
        {
            return;
        }

        _undoStack.Add(new List<AnnotationStroke>(_strokes));
        var next = _redoStack[_redoStack.Count - 1];
        _redoStack.RemoveAt(_redoStack.Count - 1);
        RestoreStrokes(next);
    }

    private void RestoreStrokes(List<AnnotationStroke> snapshot)
    {
        _strokes.Clear();
        _strokes.AddRange(snapshot);
        RefreshMosaicTiles();
        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
    }

    private void PushHistory()
    {
        _undoStack.Add(new List<AnnotationStroke>(_strokes));

        if (_undoStack.Count > MaxHistory)
        {
            _undoStack.RemoveAt(0);
        }

        _redoStack.Clear();
        UpdateHistoryVisuals();
    }

    private void UpdateHistoryVisuals()
    {
        UndoButton.IsEnabled = _undoStack.Count > 0 || _liveStroke is not null;
        RedoButton.IsEnabled = _redoStack.Count > 0;
        ClearButton.IsEnabled = _strokes.Count > 0 || _liveStroke is not null;
    }

    private void CancelLiveStroke()
    {
        _liveStroke = null;
        _drawingAnnotation = false;
        _eraseHistoryPushed = false;

        if (Mouse.Captured == Root)
        {
            Mouse.Capture(null);
        }

        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
    }

    private void ClearAnnotationStrokes()
    {
        if (_strokes.Count == 0 && _liveStroke is null)
        {
            return;
        }

        PushHistory();
        CancelTextEditor();
        CancelLiveStroke();
        _strokes.Clear();
        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
    }

    private void ClearAnnotations()
    {
        CancelTextEditor();
        _undoStack.Clear();
        _redoStack.Clear();
        _strokes.Clear();
        CancelLiveStroke();
        UpdateHistoryVisuals();
    }

    private void BeginTextEditor(Point rootPoint, Point selectionPoint)
    {
        CancelTextEditor();

        var color = _annotationColor;
        var brush = new SolidColorBrush(color);
        brush.Freeze();

        var box = new TextBox
        {
            MinWidth = 90,
            MaxWidth = 360,
            FontSize = _textSize,
            FontWeight = _textBold ? FontWeights.Bold : FontWeights.Normal,
            FontStyle = _textItalic ? FontStyles.Italic : FontStyles.Normal,
            Padding = new Thickness(5, 3, 5, 3),
            Background = UiColors.Brush("#CC10141C"),
            Foreground = brush,
            BorderBrush = brush,
            BorderThickness = new Thickness(1),
            CaretBrush = brush,
            SelectionBrush = brush,
            FocusVisualStyle = null,
            AcceptsReturn = false
        };

        box.PreviewKeyDown += TextEditor_PreviewKeyDown;
        box.LostFocus += (_, _) => CommitTextEditor();

        Canvas.SetLeft(box, rootPoint.X);
        Canvas.SetTop(box, rootPoint.Y);
        EditorLayer.Children.Add(box);

        _textEditor = box;
        _textPoint = selectionPoint;
        box.Focus();
    }

    private void TextEditor_PreviewKeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter)
        {
            e.Handled = true;
            CommitTextEditor();
            Keyboard.Focus(Root);
        }
        else if (e.Key == Key.Escape)
        {
            e.Handled = true;
            CancelTextEditor();
            Keyboard.Focus(Root);
        }
    }

    private void CommitTextEditor()
    {
        var box = _textEditor;

        if (box is null)
        {
            return;
        }

        _textEditor = null;
        var text = box.Text.Trim();
        EditorLayer.Children.Remove(box);

        if (text.Length == 0)
        {
            return;
        }

        var stroke = new AnnotationStroke
        {
            Tool = AnnotationTool.Text,
            Color = _annotationColor,
            Text = text,
            TextSize = _textSize,
            Bold = _textBold,
            Italic = _textItalic
        };

        stroke.Points.Add(_textPoint);
        PushHistory();
        _strokes.Add(stroke);
        AnnotationView.SetContent(_strokes, null, 1.0);
        UpdateHistoryVisuals();
    }

    private void CancelTextEditor()
    {
        var box = _textEditor;

        if (box is null)
        {
            return;
        }

        _textEditor = null;
        EditorLayer.Children.Remove(box);
    }

    private bool IsWithinTextEditor(object? source)
    {
        if (_textEditor is null)
        {
            return false;
        }

        var current = source as DependencyObject;

        while (current is not null)
        {
            if (ReferenceEquals(current, _textEditor))
            {
                return true;
            }

            current = current is Visual
                ? VisualTreeHelper.GetParent(current)
                : LogicalTreeHelper.GetParent(current);
        }

        return false;
    }
}
