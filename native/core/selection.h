#pragma once

#include "geometry.h"

#include <cstdint>

namespace halocap::core {

// S3 的选区状态机：拖动选一块矩形、八个把手调整、整块移动、Esc 取消。
// 与 1.x 的 ScreenshotSession 同语义，但把「鼠标在哪、按了哪个键」留给 UI 层，
// 这里只接收已经换算到图像坐标的点，所以能完全单测。
enum class SelectionPhase : std::int32_t {
    Idle = 0,
    Dragging = 1,   // 手指还按着
    Selected = 2,   // 松手之后
};

enum class SelectionHandle : std::int32_t {
    None = 0,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Move,
};

class SelectionModel {
public:
    // 选区至少要有这么多像素宽高才算数（1.x 是 1，太小的误触会被丢掉）。
    static constexpr float kMinSize = 2.0f;
    static constexpr float kHandleTolerance = 6.0f;

    // 边界的上下限（通常是整块画面）。空矩形 = 不限制。
    void SetLimit(Rect limit);
    Rect Limit() const { return limit_; }

    void Begin(Point point);
    void Update(Point point, bool constrainSquare);
    void Commit();
    void Cancel();

    // 只在 Selected 相位有效。
    bool MoveBy(Point delta);
    bool ResizeTo(SelectionHandle handle, Point point);

    // 命中：先看八个把手（容忍度 tolerance），再看是否落在选区内部（Move）。
    SelectionHandle HitTest(Point point) const;
    SelectionHandle HitTest(Point point, float tolerance) const;

    Rect Bounds() const { return bounds_; }
    SelectionPhase Phase() const { return phase_; }
    bool IsEmpty() const { return bounds_.IsEmpty(); }

private:
    Point ClampPoint(Point point) const;
    Rect ClampRect(Rect rect) const;

    Rect limit_{};
    Rect bounds_{};
    Point anchor_{};
    Point current_{};
    SelectionPhase phase_ = SelectionPhase::Idle;
};

}
