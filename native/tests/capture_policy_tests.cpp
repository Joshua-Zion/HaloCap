#include "capture_source.h"

#include <cstdio>

namespace {

using halocap::core::BackoffUs;
using halocap::core::DirtySummary;
using halocap::core::IntRect;
using halocap::core::SummarizeDirtyRects;

int failures = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckIntRect(const IntRect& actual, const IntRect& expected, const char* what) {
    if (actual.left != expected.left || actual.top != expected.top || actual.right != expected.right ||
        actual.bottom != expected.bottom) {
        ++failures;
        std::printf("FAIL %s: got [%d %d %d %d] want [%d %d %d %d]\n", what, actual.left, actual.top,
                    actual.right, actual.bottom, expected.left, expected.top, expected.right,
                    expected.bottom);
    }
}

void TestEmpty() {
    const DirtySummary summary = SummarizeDirtyRects({});
    CheckEqual(summary.rectCount, 0, "empty: no rects");
    CheckEqual(summary.sumAreaPx, 0, "empty: no area");
    CheckEqual(summary.boundingBoxAreaPx, 0, "empty: no bounding box");
}

void TestDisjointRects() {
    const DirtySummary summary =
        SummarizeDirtyRects({ IntRect{ 0, 0, 100, 50 }, IntRect{ 1000, 1000, 1100, 1050 } });

    CheckEqual(summary.rectCount, 2, "disjoint: two rects");
    CheckEqual(summary.sumAreaPx, 10000, "disjoint: sum is what must be moved");
    CheckIntRect(summary.boundingBox, IntRect{ 0, 0, 1100, 1050 }, "disjoint: bounding box");
    CheckEqual(summary.boundingBoxAreaPx, 1100LL * 1050, "disjoint: box is far bigger than the sum");
}

void TestTouchingRectsShareBoxAreaWithSum() {
    const DirtySummary summary =
        SummarizeDirtyRects({ IntRect{ 0, 0, 100, 50 }, IntRect{ 100, 0, 200, 50 } });
    CheckEqual(summary.sumAreaPx, 10000, "adjacent: sum area");
    CheckEqual(summary.boundingBoxAreaPx, 10000, "adjacent: box == sum when they tile exactly");
}

void TestZeroAreaAndInverted() {
    const DirtySummary summary =
        SummarizeDirtyRects({ IntRect{ 5, 5, 5, 9 }, IntRect{ 10, 10, 0, 0 } });
    CheckEqual(summary.rectCount, 1, "zero-area rect carries nothing");
    CheckEqual(summary.sumAreaPx, 100, "inverted rect is normalized, not dropped");
    CheckIntRect(summary.boundingBox, IntRect{ 0, 0, 10, 10 }, "inverted rect normalized");
}

void TestBackoff() {
    CheckEqual(BackoffUs(0, 1000000, 30000000), 1000000, "attempt 0 is the base");
    CheckEqual(BackoffUs(1, 1000000, 30000000), 2000000, "doubles");
    CheckEqual(BackoffUs(2, 1000000, 30000000), 4000000, "doubles again");
    CheckEqual(BackoffUs(5, 1000000, 30000000), 30000000, "capped at the ceiling");
    CheckEqual(BackoffUs(50, 1000000, 30000000), 30000000, "huge attempt still capped");
    CheckEqual(BackoffUs(-1, 1000000, 30000000), 1000000, "negative attempt is the base");
    CheckEqual(BackoffUs(3, 0, 30000000), 0, "zero base means no waiting");
    CheckEqual(BackoffUs(3, 1000000, 500000), 500000, "ceiling below base is honored");
}

}

int main() {
    TestEmpty();
    TestDisjointRects();
    TestTouchingRectsShareBoxAreaWithSum();
    TestZeroAreaAndInverted();
    TestBackoff();

    if (failures != 0) {
        std::printf("CAPTURE_POLICY_TESTS=FAILED %d\n", failures);
        return 1;
    }

    std::printf("CAPTURE_POLICY_TESTS=OK\n");
    return 0;
}
