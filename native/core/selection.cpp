#include "selection.h"

#include <algorithm>
#include <cmath>

namespace halocap::core {

namespace {

Rect Normalize(Rect rect) { return rect.Normalized(); }

// 与 annotation.h 的 ConstrainSquare 同一条规则（45° 与正方形都靠它）。
// 这里不 include annotation.h：选区不该依赖标注模型。
Point ConstrainSquare(Point start, Point end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float size = std::max(std::fabs(dx), std::fabs(dy));
    const float signX = dx < 0 ? -1.0f : 1.0f;
    const float signY = dy < 0 ? -1.0f : 1.0f;
    return Point{ start.x + size * signX, start.y + size * signY };
}

}  // namespace

void SelectionModel::SetLimit(Rect limit) {
    limit_ = Normalize(limit);
    if (phase_ != SelectionPhase::Idle) {
        bounds_ = ClampRect(bounds_);
    }
}

Point SelectionModel::ClampPoint(Point point) const {
    if (limit_.IsEmpty()) {
        return point;
    }
    return Point{ std::clamp(point.x, limit_.left, limit_.right),
                  std::clamp(point.y, limit_.top, limit_.bottom) };
}

Rect SelectionModel::ClampRect(Rect rect) const {
    Rect normalized = Normalize(rect);
    if (limit_.IsEmpty()) {
        return normalized;
    }

    normalized.left = std::clamp(normalized.left, limit_.left, limit_.right);
    normalized.right = std::clamp(normalized.right, limit_.left, limit_.right);
    normalized.top = std::clamp(normalized.top, limit_.top, limit_.bottom);
    normalized.bottom = std::clamp(normalized.bottom, limit_.top, limit_.bottom);
    return Normalize(normalized);
}

void SelectionModel::Begin(Point point) {
    phase_ = SelectionPhase::Dragging;
    anchor_ = ClampPoint(point);
    current_ = anchor_;
    bounds_ = Rect{ anchor_.x, anchor_.y, anchor_.x, anchor_.y };
}

void SelectionModel::Update(Point point, bool constrainSquare) {
    if (phase_ != SelectionPhase::Dragging) {
        return;
    }
    const Point clamped = ClampPoint(point);
    current_ = constrainSquare ? ClampPoint(ConstrainSquare(anchor_, clamped)) : clamped;
    bounds_ = Normalize(Rect{ anchor_.x, anchor_.y, current_.x, current_.y });
}

void SelectionModel::Commit() {
    if (phase_ != SelectionPhase::Dragging) {
        return;
    }
    if (bounds_.Width() < kMinSize || bounds_.Height() < kMinSize) {
        Cancel();
        return;
    }
    phase_ = SelectionPhase::Selected;
}

void SelectionModel::Cancel() {
    phase_ = SelectionPhase::Idle;
    bounds_ = Rect{};
    anchor_ = {};
    current_ = {};
}

bool SelectionModel::MoveBy(Point delta) {
    if (phase_ != SelectionPhase::Selected) {
        return false;
    }
    Rect moved{ bounds_.left + delta.x, bounds_.top + delta.y, bounds_.right + delta.x,
                bounds_.bottom + delta.y };

    if (!limit_.IsEmpty()) {
        // 移动是整体平移：撞边时把整块推回去，而不是裁小它。
        if (moved.left < limit_.left) {
            const float shift = limit_.left - moved.left;
            moved.left += shift;
            moved.right += shift;
        }
        if (moved.right > limit_.right) {
            const float shift = moved.right - limit_.right;
            moved.left -= shift;
            moved.right -= shift;
        }
        if (moved.top < limit_.top) {
            const float shift = limit_.top - moved.top;
            moved.top += shift;
            moved.bottom += shift;
        }
        if (moved.bottom > limit_.bottom) {
            const float shift = moved.bottom - limit_.bottom;
            moved.top -= shift;
            moved.bottom -= shift;
        }
    }

    bounds_ = moved;
    return true;
}

bool SelectionModel::ResizeTo(SelectionHandle handle, Point point) {
    if (phase_ != SelectionPhase::Selected || handle == SelectionHandle::None ||
        handle == SelectionHandle::Move) {
        return false;
    }

    const Point clamped = ClampPoint(point);
    Rect rect = bounds_;

    switch (handle) {
    case SelectionHandle::TopLeft:
        rect.left = clamped.x;
        rect.top = clamped.y;
        break;
    case SelectionHandle::Top:
        rect.top = clamped.y;
        break;
    case SelectionHandle::TopRight:
        rect.right = clamped.x;
        rect.top = clamped.y;
        break;
    case SelectionHandle::Right:
        rect.right = clamped.x;
        break;
    case SelectionHandle::BottomRight:
        rect.right = clamped.x;
        rect.bottom = clamped.y;
        break;
    case SelectionHandle::Bottom:
        rect.bottom = clamped.y;
        break;
    case SelectionHandle::BottomLeft:
        rect.left = clamped.x;
        rect.bottom = clamped.y;
        break;
    case SelectionHandle::Left:
        rect.left = clamped.x;
        break;
    case SelectionHandle::None:
    case SelectionHandle::Move:
        return false;
    }

    rect = Normalize(rect);
    if (rect.Width() < kMinSize || rect.Height() < kMinSize) {
        // 反向拖过头：保持原来的最小尺寸，不产生一个空选区。
        rect.right = std::max(rect.right, rect.left + kMinSize);
        rect.bottom = std::max(rect.bottom, rect.top + kMinSize);
        rect = ClampRect(rect);
    }
    bounds_ = rect;
    return true;
}

SelectionHandle SelectionModel::HitTest(Point point) const {
    return HitTest(point, kHandleTolerance);
}

SelectionHandle SelectionModel::HitTest(Point point, float tolerance) const {
    if (phase_ != SelectionPhase::Selected) {
        return SelectionHandle::None;
    }

    const Rect rect = bounds_;
    const float cx = (rect.left + rect.right) * 0.5f;
    const float cy = (rect.top + rect.bottom) * 0.5f;

    const struct {
        SelectionHandle handle;
        Point position;
    } handles[] = {
        { SelectionHandle::TopLeft, { rect.left, rect.top } },
        { SelectionHandle::Top, { cx, rect.top } },
        { SelectionHandle::TopRight, { rect.right, rect.top } },
        { SelectionHandle::Right, { rect.right, cy } },
        { SelectionHandle::BottomRight, { rect.right, rect.bottom } },
        { SelectionHandle::Bottom, { cx, rect.bottom } },
        { SelectionHandle::BottomLeft, { rect.left, rect.bottom } },
        { SelectionHandle::Left, { rect.left, cy } },
    };

    for (const auto& handle : handles) {
        if (std::fabs(point.x - handle.position.x) <= tolerance &&
            std::fabs(point.y - handle.position.y) <= tolerance) {
            return handle.handle;
        }
    }

    return rect.Contains(point) ? SelectionHandle::Move : SelectionHandle::None;
}

}
