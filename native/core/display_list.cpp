#include "display_list.h"

namespace halocap::core {

namespace {

Rect PathBounds(const Path& path) {
    if (path.points.empty()) {
        return Rect{};
    }
    float left = path.points.front().x;
    float top = path.points.front().y;
    float right = left;
    float bottom = top;
    for (const Point& p : path.points) {
        left = std::min(left, p.x);
        top = std::min(top, p.y);
        right = std::max(right, p.x);
        bottom = std::max(bottom, p.y);
    }
    const float half = path.thickness * 0.5f;
    return Rect{ left - half, top - half, right + half, bottom + half };
}

}

void FramePlan::Clear() {
    items_.clear();
}

void FramePlan::Add(Item item) {
    items_.push_back(std::move(item));
}

Rect FramePlan::ContentBounds() const {
    Rect result{};
    for (const Item& item : items_) {
        result = result.Union(BoundsOf(item));
    }
    return result;
}

Rect BoundsOf(const Item& item) {
    return std::visit(
        [](const auto& specific) -> Rect {
            using T = std::decay_t<decltype(specific)>;
            if constexpr (std::is_same_v<T, FillRect>) {
                return specific.rect;
            } else if constexpr (std::is_same_v<T, StrokeRect>) {
                const float half = specific.thickness * 0.5f;
                return Rect{ specific.rect.left - half, specific.rect.top - half,
                             specific.rect.right + half, specific.rect.bottom + half };
            } else if constexpr (std::is_same_v<T, Blit>) {
                return specific.destination;
            } else if constexpr (std::is_same_v<T, GlyphRun>) {
                return specific.bounds;
            } else {
                return PathBounds(specific);
            }
        },
        item);
}

}
