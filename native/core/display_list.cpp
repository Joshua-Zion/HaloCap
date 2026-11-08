#include "display_list.h"

#include <algorithm>
#include <type_traits>

namespace halocap::core {

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
            } else if constexpr (std::is_same_v<T, FillEllipse>) {
                return specific.rect;
            } else if constexpr (std::is_same_v<T, StrokeEllipse>) {
                const float half = specific.thickness * 0.5f;
                return Rect{ specific.rect.left - half, specific.rect.top - half,
                             specific.rect.right + half, specific.rect.bottom + half };
            } else if constexpr (std::is_same_v<T, Blit>) {
                return specific.destination;
            } else {
                const float half = specific.thickness * 0.5f;
                return Rect{ std::min(specific.from.x, specific.to.x) - half,
                             std::min(specific.from.y, specific.to.y) - half,
                             std::max(specific.from.x, specific.to.x) + half,
                             std::max(specific.from.y, specific.to.y) + half };
            }
        },
        item);
}

void AppendIntersecting(const FramePlan& source, Rect bounds, FramePlan& destination) {
    const bool wholeTarget = bounds.IsEmpty();

    for (const Item& item : source.Items()) {
        if (wholeTarget || !bounds.Intersect(BoundsOf(item)).IsEmpty()) {
            destination.Add(item);
        }
    }
}

}
