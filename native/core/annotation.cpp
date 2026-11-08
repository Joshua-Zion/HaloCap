#include "annotation.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace halocap::core {

namespace annotation_defaults {

namespace {

const Color kPalette[8] = {
    Color{ 1.0f, 0.353f, 0.353f, 1.0f },   // #FF5A5A
    Color{ 1.0f, 0.663f, 0.302f, 1.0f },   // #FFA94D
    Color{ 1.0f, 0.820f, 0.400f, 1.0f },   // #FFD166
    Color{ 0.486f, 0.890f, 0.545f, 1.0f }, // #7CE38B
    Color{ 0.424f, 0.659f, 1.0f, 1.0f },   // #6CA8FF
    Color{ 0.690f, 0.549f, 1.0f, 1.0f },   // #B08CFF
    Color{ 0.141f, 0.161f, 0.200f, 1.0f }, // #242933
    Color{ 1.0f, 1.0f, 1.0f, 1.0f },       // #FFFFFF
};

}  // namespace

Color PaletteColor(int index) {
    if (index < 0 || index >= kPaletteCount) {
        return kPalette[0];
    }
    return kPalette[index];
}

}  // namespace annotation_defaults

namespace {

float Distance(Point a, Point b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

Rect Inflate(Rect rect, float radius) {
    const Rect normalized = rect.Normalized();
    return Rect{ normalized.left - radius, normalized.top - radius, normalized.right + radius,
                 normalized.bottom + radius };
}

// 点到线段的最短距离（1.x NearSegment 的同一算法）。
float DistanceToSegment(Point start, Point end, Point point) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float lengthSquared = dx * dx + dy * dy;
    if (lengthSquared < 0.0001f) {
        return Distance(point, start);
    }
    float t = ((point.x - start.x) * dx + (point.y - start.y) * dy) / lengthSquared;
    t = std::clamp(t, 0.0f, 1.0f);
    const Point projection{ start.x + t * dx, start.y + t * dy };
    return Distance(point, projection);
}

bool NearPolyline(const std::vector<Point>& points, Point point, float threshold) {
    if (points.empty()) {
        return false;
    }
    if (points.size() == 1) {
        return Distance(points[0], point) <= threshold;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (DistanceToSegment(points[i - 1], points[i], point) <= threshold) {
            return true;
        }
    }
    return false;
}

float EffectiveThickness(const Annotation& annotation) {
    const float thickness = annotation.tool == AnnotationTool::Highlighter
                                ? std::max(2.0f, annotation.style.thickness)
                                : annotation.style.thickness;
    return std::max(0.5f, thickness);
}

// 椭圆按「归一化半径」判：r > 1 在外面，r ≈ 1 在边框上。
float EllipseRadius(Point point, Point center, float rx, float ry) {
    if (rx <= 0.0001f || ry <= 0.0001f) {
        return 0.0f;
    }
    const float dx = (point.x - center.x) / rx;
    const float dy = (point.y - center.y) / ry;
    return std::sqrt(dx * dx + dy * dy);
}

Rect RectFromPoints(Point a, Point b) {
    return Rect{ std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
}

Point Placed(Point point, const AnnotationPlanOptions& options) {
    return Point{ point.x * options.scale + options.origin.x,
                  point.y * options.scale + options.origin.y };
}

Rect Placed(Rect rect, const AnnotationPlanOptions& options) {
    const Rect normalized = rect.Normalized();
    return Rect{ normalized.left * options.scale + options.origin.x,
                 normalized.top * options.scale + options.origin.y,
                 normalized.right * options.scale + options.origin.x,
                 normalized.bottom * options.scale + options.origin.y };
}

}  // namespace

Rect NormalizeRect(Point a, Point b) {
    return Rect{ std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
}

Point ConstrainSquare(Point start, Point end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float size = std::max(std::fabs(dx), std::fabs(dy));
    const float signX = dx < 0 ? -1.0f : 1.0f;
    const float signY = dy < 0 ? -1.0f : 1.0f;
    return Point{ start.x + size * signX, start.y + size * signY };
}

Point SnapToAngle(Point start, Point end) {
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 0.001f) {
        return end;
    }
    // 45° 一档：画箭头时按住 Shift 能画出「正」方向。
    constexpr float kStep = 0.78539816f;
    const float angle = std::round(std::atan2(dy, dx) / kStep) * kStep;
    return Point{ start.x + length * std::cos(angle), start.y + length * std::sin(angle) };
}

bool IsLightColor(const Color& color) {
    const int luminance = static_cast<int>((color.r * 255.0f * 299.0f + color.g * 255.0f * 587.0f +
                                            color.b * 255.0f * 114.0f) /
                                           1000.0f);
    return luminance > 178;
}

bool HitTest(const Annotation& annotation, Point point, float radius) {
    radius = std::max(0.0f, radius);

    switch (annotation.tool) {
    case AnnotationTool::Eraser:
        return false;

    case AnnotationTool::Step:
        return !annotation.points.empty() &&
               Distance(annotation.points[0], point) <=
                   annotation_defaults::kStepRadius + radius;

    case AnnotationTool::Text:
        return !annotation.points.empty() &&
               Inflate(annotation.textBounds, radius).Contains(point);

    case AnnotationTool::Mosaic:
        return annotation.points.size() >= 2 &&
               Inflate(RectFromPoints(annotation.points[0], annotation.points[1]), radius)
                   .Contains(point);

    case AnnotationTool::Pen:
    case AnnotationTool::Highlighter:
        return NearPolyline(annotation.points, point, radius + EffectiveThickness(annotation) * 0.5f);

    case AnnotationTool::Arrow:
        return annotation.points.size() >= 2 &&
               DistanceToSegment(annotation.points.front(), annotation.points.back(), point) <=
                   radius + EffectiveThickness(annotation) * 0.5f;

    case AnnotationTool::Rectangle: {
        if (annotation.points.size() < 2) {
            return false;
        }
        const Rect rect = RectFromPoints(annotation.points[0], annotation.points[1]);
        if (rect.Width() < 0.5f || rect.Height() < 0.5f) {
            return false;
        }
        const float threshold = radius + EffectiveThickness(annotation) * 0.5f;
        const bool nearEdge =
            (point.x >= rect.left - threshold && point.x <= rect.right + threshold &&
             (std::fabs(point.y - rect.top) <= threshold ||
              std::fabs(point.y - rect.bottom) <= threshold)) ||
            (point.y >= rect.top - threshold && point.y <= rect.bottom + threshold &&
             (std::fabs(point.x - rect.left) <= threshold ||
              std::fabs(point.x - rect.right) <= threshold));
        return nearEdge || (annotation.style.filled && rect.Contains(point));
    }

    case AnnotationTool::Ellipse: {
        if (annotation.points.size() < 2) {
            return false;
        }
        const Rect rect = RectFromPoints(annotation.points[0], annotation.points[1]);
        if (rect.Width() < 0.5f || rect.Height() < 0.5f) {
            return false;
        }
        const Point center{ (rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f };
        const float rx = rect.Width() * 0.5f;
        const float ry = rect.Height() * 0.5f;
        const float normalized = EllipseRadius(point, center, rx, ry);
        const float tolerance = (radius + EffectiveThickness(annotation) * 0.5f) /
                                std::max(1.0f, std::min(rx, ry));
        if (std::fabs(normalized - 1.0f) <= tolerance) {
            return true;
        }
        return annotation.style.filled && normalized <= 1.0f;
    }
    }

    return false;
}

int EraseAt(std::vector<Annotation>& annotations, Point point, float radius) {
    const auto removed = std::remove_if(annotations.begin(), annotations.end(),
                                        [&](const Annotation& annotation) {
                                            return HitTest(annotation, point, radius);
                                        });
    const int count = static_cast<int>(std::distance(removed, annotations.end()));
    annotations.erase(removed, annotations.end());
    return count;
}

// ---- 显示列表 ----

namespace {

void AppendArrowHead(FramePlan& plan, const Annotation& annotation, Point tip, float angle,
                     float scale) {
    const float thickness = std::max(0.4f, annotation.style.thickness * scale);
    const float head = std::max(annotation_defaults::kMinArrowHeadPx * scale,
                                annotation.style.thickness * scale * 3.2f);
    for (int direction = -1; direction <= 1; direction += 2) {
        const float wingAngle =
            angle + 3.14159265f +
            static_cast<float>(direction) * annotation_defaults::kArrowHeadSpreadRadians;
        const Point wing{ tip.x + head * std::cos(wingAngle),
                          tip.y + head * std::sin(wingAngle) };
        plan.Add(Segment{ tip, wing, annotation.style.color, thickness });
    }
}

void AppendStepCircle(FramePlan& plan, const Annotation& annotation, float scale,
                      const AnnotationPlanOptions& options) {
    if (annotation.points.empty()) {
        return;
    }

    const Point center = Placed(annotation.points[0], options);
    const float radius = annotation_defaults::kStepRadius * scale;
    const Color ink = IsLightColor(annotation.style.color)
                          ? Color{ 0.106f, 0.122f, 0.141f, 1.0f }
                          : Color{ 1.0f, 1.0f, 1.0f, 1.0f };

    const Rect circle{ center.x - radius, center.y - radius, center.x + radius, center.y + radius };
    plan.Add(FillEllipse{ circle, annotation.style.color });
    plan.Add(StrokeEllipse{ circle, ink, std::max(1.0f, 1.2f * scale) });

    if (options.rasterizer == nullptr || annotation.text.empty()) {
        return;
    }

    const FontSpec spec{ options.fontFamily, 600, 13.0f * scale, ink };
    // 先排一次拿尺寸，再按中心排第二次（第二次命中图集缓存，成本是查表）。
    const TextCell measured = options.rasterizer->LayOut(annotation.text, spec, Point{ 0.0f, 0.0f });
    if (measured.atlas == 0) {
        return;
    }
    const float width = measured.destination.Width();
    const float height = measured.destination.Height();
    const TextCell cell = options.rasterizer->LayOut(
        annotation.text, spec, Point{ center.x - width * 0.5f, center.y - height * 0.5f });
    if (cell.atlas != 0) {
        plan.Add(cell.ToBlit());
    }
}

}  // namespace

void AppendAnnotationPlan(const std::vector<Annotation>& annotations,
                          const AnnotationPlanOptions& options, FramePlan& plan) {
    const float scale = options.scale > 0.0001f ? options.scale : 1.0f;

    for (const Annotation& annotation : annotations) {
        const AnnotationStyle& style = annotation.style;

        switch (annotation.tool) {
        case AnnotationTool::Eraser:
            break;

        case AnnotationTool::Pen: {
            const float thickness = std::max(0.4f, style.thickness * scale);
            if (annotation.points.size() == 1) {
                const Point center = Placed(annotation.points[0], options);
                const float radius = std::max(0.6f, thickness * 0.5f);
                plan.Add(FillEllipse{ Rect{ center.x - radius, center.y - radius,
                                            center.x + radius, center.y + radius },
                                      style.color });
                break;
            }
            for (std::size_t i = 1; i < annotation.points.size(); ++i) {
                plan.Add(Segment{ Placed(annotation.points[i - 1], options),
                                  Placed(annotation.points[i], options), style.color, thickness });
            }
            break;
        }

        case AnnotationTool::Highlighter: {
            Color color = style.color;
            color.a = annotation_defaults::kHighlighterAlpha;
            const float thickness = std::max(2.0f, style.thickness) * scale;
            for (std::size_t i = 1; i < annotation.points.size(); ++i) {
                plan.Add(Segment{ Placed(annotation.points[i - 1], options),
                                  Placed(annotation.points[i], options), color, thickness });
            }
            break;
        }

        case AnnotationTool::Rectangle: {
            if (annotation.points.size() < 2) {
                break;
            }
            const Rect rect = Placed(NormalizeRect(annotation.points[0], annotation.points[1]), options);
            if (style.filled) {
                plan.Add(FillRect{ rect, style.color });
            } else {
                plan.Add(StrokeRect{ rect, style.color, std::max(0.4f, style.thickness * scale) });
            }
            break;
        }

        case AnnotationTool::Ellipse: {
            if (annotation.points.size() < 2) {
                break;
            }
            const Rect rect = Placed(NormalizeRect(annotation.points[0], annotation.points[1]), options);
            if (style.filled) {
                plan.Add(FillEllipse{ rect, style.color });
            } else {
                plan.Add(StrokeEllipse{ rect, style.color, std::max(0.4f, style.thickness * scale) });
            }
            break;
        }

        case AnnotationTool::Arrow: {
            if (annotation.points.size() < 2) {
                break;
            }
            const Point start = Placed(annotation.points.front(), options);
            const Point end = Placed(annotation.points.back(), options);
            const float dx = end.x - start.x;
            const float dy = end.y - start.y;
            if (std::sqrt(dx * dx + dy * dy) < 0.5f) {
                break;
            }
            const float thickness = std::max(0.4f, style.thickness * scale);
            plan.Add(Segment{ start, end, style.color, thickness });
            const float angle = std::atan2(dy, dx);
            if (style.arrowHead == ArrowHeadMode::End || style.arrowHead == ArrowHeadMode::Both) {
                AppendArrowHead(plan, annotation, end, angle, scale);
            }
            if (style.arrowHead == ArrowHeadMode::Both) {
                AppendArrowHead(plan, annotation, start, angle + 3.14159265f, scale);
            }
            break;
        }

        case AnnotationTool::Text: {
            if (options.rasterizer == nullptr || annotation.text.empty() ||
                annotation.points.empty()) {
                break;
            }
            const FontSpec spec{ options.fontFamily, style.bold ? 700 : 400,
                                 style.textSize * scale, style.color };
            const TextCell cell =
                options.rasterizer->LayOut(annotation.text, spec, Placed(annotation.points[0], options));
            if (cell.atlas != 0) {
                plan.Add(cell.ToBlit());
            }
            break;
        }

        case AnnotationTool::Mosaic: {
            if (annotation.points.size() < 2 || annotation.mosaicTile == 0) {
                break;
            }
            const Rect destination =
                Placed(NormalizeRect(annotation.points[0], annotation.points[1]), options);
            plan.Add(Blit{ annotation.mosaicTile,
                           Rect{ 0.0f, 0.0f, annotation.mosaicTileSize.width,
                                 annotation.mosaicTileSize.height },
                           destination, 1.0f });
            break;
        }

        case AnnotationTool::Step:
            AppendStepCircle(plan, annotation, scale, options);
            break;
        }
    }
}

// ---- 文档 ----

AnnotationDocument::AnnotationDocument() { history_.push_back({}); }

Annotation& AnnotationDocument::Begin(AnnotationTool tool, const AnnotationStyle& style) {
    Cancel();

    current_ = Annotation{};
    current_.tool = tool;
    current_.style = style;
    if (tool == AnnotationTool::Step) {
        current_.text = std::to_string(nextStep_);
        ++nextStep_;
    }
    drawing_ = true;
    return current_;
}

void AnnotationDocument::AddPoint(Point point, float minDistance) {
    if (!drawing_) {
        return;
    }
    if (!current_.points.empty() && minDistance > 0.0f) {
        if (Distance(current_.points.back(), point) < minDistance) {
            return;
        }
    }
    current_.points.push_back(point);
}

void AnnotationDocument::SetEnd(Point point) {
    if (!drawing_) {
        return;
    }
    if (current_.points.size() < 2) {
        current_.points.push_back(point);
    } else {
        current_.points[1] = point;
    }
}

void AnnotationDocument::Commit() {
    if (!drawing_) {
        return;
    }

    // 只有「两个点定义一条边」的工具需要第二点；文字与序号是单击落点。
    const bool twoPointTool = current_.tool == AnnotationTool::Rectangle ||
                              current_.tool == AnnotationTool::Ellipse ||
                              current_.tool == AnnotationTool::Arrow ||
                              current_.tool == AnnotationTool::Mosaic;

    const bool usable = twoPointTool ? current_.points.size() >= 2 : !current_.points.empty();
    if (usable) {
        items_.push_back(current_);
        Snapshot();
    }
    current_ = Annotation{};
    drawing_ = false;
}

void AnnotationDocument::Cancel() {
    current_ = Annotation{};
    drawing_ = false;
}

bool AnnotationDocument::MoveAnnotation(std::size_t index, Point delta) {
    if (index >= items_.size()) {
        return false;
    }
    Annotation& annotation = items_[index];
    for (Point& point : annotation.points) {
        point.x += delta.x;
        point.y += delta.y;
    }
    annotation.textBounds.left += delta.x;
    annotation.textBounds.right += delta.x;
    annotation.textBounds.top += delta.y;
    annotation.textBounds.bottom += delta.y;
    return true;
}

bool AnnotationDocument::RemoveAnnotation(std::size_t index) {
    if (index >= items_.size()) {
        return false;
    }
    items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(index));
    Snapshot();
    return true;
}

void AnnotationDocument::CommitAnnotationChange() {
    if (!items_.empty()) {
        Snapshot();
    }
}

int AnnotationDocument::EraseAt(Point point, float radius) {
    const int removed = core::EraseAt(items_, point, radius);
    if (removed > 0) {
        Snapshot();
    }
    return removed;
}

bool AnnotationDocument::Undo() {
    if (cursor_ == 0) {
        return false;
    }
    --cursor_;
    items_ = history_[cursor_];
    return true;
}

bool AnnotationDocument::Redo() {
    if (cursor_ + 1 >= history_.size()) {
        return false;
    }
    ++cursor_;
    items_ = history_[cursor_];
    return true;
}

void AnnotationDocument::Clear() {
    items_.clear();
    current_ = Annotation{};
    drawing_ = false;
    history_.clear();
    history_.push_back({});
    cursor_ = 0;
    nextStep_ = 1;
}

void AnnotationDocument::Snapshot() {
    history_.resize(cursor_ + 1);
    history_.push_back(items_);
    cursor_ = history_.size() - 1;
}

}
