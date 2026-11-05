#include "display_list.h"
#include "geometry.h"

#include <cstdio>
#include <variant>

namespace {

using namespace halocap::core;

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckRect(Rect actual, Rect expected, const char* what) {
    if (!(actual == expected)) {
        ++failures;
        std::printf("FAIL %s: got [%.2f %.2f %.2f %.2f] want [%.2f %.2f %.2f %.2f]\n",
                    what, actual.left, actual.top, actual.right, actual.bottom,
                    expected.left, expected.top, expected.right, expected.bottom);
    }
}

void TestRectBasics() {
    const Rect r = Rect::FromTopLeft(Point{ 10.0f, 20.0f }, Size{ 30.0f, 40.0f });
    CheckRect(r, Rect{ 10.0f, 20.0f, 40.0f, 60.0f }, "FromTopLeft");
    Check(r.Width() == 30.0f && r.Height() == 40.0f, "Width/Height");
    Check(!r.IsEmpty(), "non-empty");
    Check(Rect{ 5.0f, 5.0f, 5.0f, 9.0f }.IsEmpty(), "zero width is empty");
    CheckRect(Rect{ 40.0f, 60.0f, 10.0f, 20.0f }.Normalized(), r, "Normalized swaps inverted");

    Check(r.Contains(Point{ 10.0f, 20.0f }), "Contains top-left inclusive");
    Check(!r.Contains(Point{ 40.0f, 60.0f }), "Contains right-bottom exclusive");
}

void TestUnionAndIntersect() {
    const Rect a{ 0.0f, 0.0f, 10.0f, 10.0f };
    const Rect b{ 5.0f, 5.0f, 20.0f, 30.0f };
    CheckRect(a.Union(b), Rect{ 0.0f, 0.0f, 20.0f, 30.0f }, "Union overlapping");
    CheckRect(Rect{}.Union(a), a, "Union with empty");
    CheckRect(a.Union(Rect{}), a, "Union with empty on the right");
    const Rect far{ 100.0f, 100.0f, 120.0f, 120.0f };
    CheckRect(a.Union(far), Rect{ 0.0f, 0.0f, 120.0f, 120.0f }, "Union disjoint spans the gap");

    CheckRect(a.Intersect(b), Rect{ 5.0f, 5.0f, 10.0f, 10.0f }, "Intersect overlapping");
    Check(a.Intersect(far).IsEmpty(), "Intersect disjoint is empty");
    const Rect touching{ 10.0f, 0.0f, 20.0f, 10.0f };
    Check(a.Intersect(touching).IsEmpty(), "Intersect edge-touching is empty");
    CheckRect(Rect{ 40.0f, 60.0f, 10.0f, 20.0f }.Intersect(Rect{ 15.0f, 25.0f, 35.0f, 55.0f }),
              Rect{ 15.0f, 25.0f, 35.0f, 55.0f },
              "Intersect normalizes inverted input");
}

void TestIntRect() {
    Check(IntRect{}.IsEmpty(), "default clip is empty");
    Check(!IntRect{ 0, 0, 640, 360 }.IsEmpty(), "dirty rect is not empty");
    Check(IntRect{ 10, 0, 10, 20 }.IsEmpty(), "zero-width clip is empty");
}

void TestItemBounds() {
    const Color black{};
    CheckRect(BoundsOf(FillRect{ Rect{ 1.0f, 2.0f, 3.0f, 4.0f }, black }),
              Rect{ 1.0f, 2.0f, 3.0f, 4.0f },
              "FillRect bounds");

    CheckRect(BoundsOf(StrokeRect{ Rect{ 10.0f, 10.0f, 20.0f, 20.0f }, black, 4.0f }),
              Rect{ 8.0f, 8.0f, 22.0f, 22.0f },
              "StrokeRect grows by half thickness");

    Blit blit{};
    blit.image = 7;
    blit.source = Rect{ 0.0f, 0.0f, 64.0f, 64.0f };
    blit.destination = Rect{ 100.0f, 200.0f, 400.0f, 400.0f };
    CheckRect(BoundsOf(blit), blit.destination, "Blit uses destination not source");

    CheckRect(BoundsOf(Segment{ Point{ 0.0f, 0.0f }, Point{ 10.0f, 4.0f }, black, 2.0f }),
              Rect{ -1.0f, -1.0f, 11.0f, 5.0f },
              "Segment bounds span endpoints plus thickness");
    CheckRect(BoundsOf(Segment{ Point{ 10.0f, 4.0f }, Point{ 0.0f, 0.0f }, black, 2.0f }),
              Rect{ -1.0f, -1.0f, 11.0f, 5.0f },
              "Segment direction does not change bounds");
}

void TestFramePlan() {
    FramePlan plan;
    Check(plan.ItemCount() == 0, "Fresh plan is empty");
    Check(plan.ContentBounds().IsEmpty(), "Empty plan has no content bounds");

    plan.Add(FillRect{ Rect{ 0.0f, 0.0f, 100.0f, 40.0f }, Color{} });
    plan.Add(Blit{ 3, Rect{}, Rect{ 20.0f, 50.0f, 60.0f, 90.0f }, 1.0f });
    plan.Add(Segment{ Point{ 200.0f, 10.0f }, Point{ 200.0f, 10.0f }, Color{}, 1.0f });
    Check(plan.ItemCount() == 3, "Three items added");
    CheckRect(plan.ContentBounds(), Rect{ 0.0f, 0.0f, 200.5f, 90.0f }, "Plan bounds is the union");

    Check(std::holds_alternative<FillRect>(plan.Items()[0]), "Item 0 is FillRect");
    Check(std::holds_alternative<Blit>(plan.Items()[1]), "Item 1 is Blit");

    plan.Clear();
    Check(plan.ItemCount() == 0 && plan.ContentBounds().IsEmpty(), "Clear resets the plan");
}

}

int main() {
    TestRectBasics();
    TestUnionAndIntersect();
    TestIntRect();
    TestItemBounds();
    TestFramePlan();

    if (failures != 0) {
        std::printf("CORE_TESTS=FAILED %d\n", failures);
        return 1;
    }
    std::printf("CORE_TESTS=OK\n");
    return 0;
}
