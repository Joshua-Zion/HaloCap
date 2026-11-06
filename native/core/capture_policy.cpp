#include "capture_source.h"

#include <algorithm>

namespace halocap::core {

namespace {

long long SpanPx(int from, int to) {
    return static_cast<long long>(std::max(from, to)) - std::min(from, to);
}

}

DirtySummary SummarizeDirtyRects(const std::vector<IntRect>& rects) {
    DirtySummary summary;

    for (const IntRect& rect : rects) {
        const long long width = SpanPx(rect.left, rect.right);
        const long long height = SpanPx(rect.top, rect.bottom);
        if (width <= 0 || height <= 0) {
            continue;  // 零面积矩形不搬运任何东西，也不该进包围盒。
        }

        const int left = std::min(rect.left, rect.right);
        const int top = std::min(rect.top, rect.bottom);
        const int right = std::max(rect.left, rect.right);
        const int bottom = std::max(rect.top, rect.bottom);

        if (summary.rectCount == 0) {
            summary.boundingBox = IntRect{ left, top, right, bottom };
        } else {
            summary.boundingBox.left = std::min(summary.boundingBox.left, left);
            summary.boundingBox.top = std::min(summary.boundingBox.top, top);
            summary.boundingBox.right = std::max(summary.boundingBox.right, right);
            summary.boundingBox.bottom = std::max(summary.boundingBox.bottom, bottom);
        }

        ++summary.rectCount;
        summary.sumAreaPx += width * height;
    }

    if (summary.rectCount > 0) {
        summary.boundingBoxAreaPx =
            SpanPx(summary.boundingBox.left, summary.boundingBox.right) *
            SpanPx(summary.boundingBox.top, summary.boundingBox.bottom);
    }

    return summary;
}

std::int64_t BackoffUs(int attempt, std::int64_t baseUs, std::int64_t ceilingUs) {
    if (baseUs <= 0) {
        return 0;
    }
    if (ceilingUs <= baseUs) {
        return ceilingUs > 0 ? ceilingUs : baseUs;
    }
    if (attempt <= 0) {
        return baseUs;
    }
    if (attempt >= 20) {
        return ceilingUs;
    }

    const std::int64_t shifted = baseUs << attempt;
    return shifted > ceilingUs ? ceilingUs : shifted;
}

}
