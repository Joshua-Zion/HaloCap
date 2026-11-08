// S3 的选区状态机：拖动、正方形约束、夹进画面、八个把手、整体移动、Esc 取消。
#include "selection.h"

#include <cstdio>

namespace {

namespace core = halocap::core;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s: expected true\n", what);
    }
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    ++checks;
    if (actual < expected - tolerance || actual > expected + tolerance) {
        ++failures;
        std::printf("FAIL %s: got %.2f want %.2f\n", what, actual, expected);
    }
}

core::SelectionModel Selected(core::Point a, core::Point b) {
    core::SelectionModel model;
    model.Begin(a);
    model.Update(b, false);
    model.Commit();
    return model;
}

void TestDragAndCommit() {
    core::SelectionModel model;
    CheckTrue(model.Phase() == core::SelectionPhase::Idle, "starts idle");

    model.Begin({ 100.0f, 100.0f });
    CheckTrue(model.Phase() == core::SelectionPhase::Dragging, "dragging after begin");
    model.Update({ 300.0f, 250.0f }, false);
    CheckNear(model.Bounds().Width(), 200.0f, 0.01f, "bounds width while dragging");
    model.Commit();

    CheckTrue(model.Phase() == core::SelectionPhase::Selected, "selected after commit");
    CheckNear(model.Bounds().Width(), 200.0f, 0.01f, "committed width");
    CheckNear(model.Bounds().Height(), 150.0f, 0.01f, "committed height");

    // 反向拖动也得到同一块矩形。
    const core::SelectionModel reversed = Selected({ 300.0f, 250.0f }, { 100.0f, 100.0f });
    CheckNear(reversed.Bounds().left, 100.0f, 0.01f, "reversed selection left");
    CheckNear(reversed.Bounds().right, 300.0f, 0.01f, "reversed selection right");

    // 太小的选区被丢掉。
    core::SelectionModel tiny;
    tiny.Begin({ 10.0f, 10.0f });
    tiny.Update({ 10.5f, 10.5f }, false);
    tiny.Commit();
    CheckTrue(tiny.Phase() == core::SelectionPhase::Idle, "a sub-pixel drag is discarded");
    CheckTrue(tiny.Bounds().IsEmpty(), "discarded selection has no bounds");
}

void TestConstrainAndLimit() {
    core::SelectionModel model;
    model.SetLimit(core::Rect{ 0.0f, 0.0f, 1920.0f, 1080.0f });

    model.Begin({ 500.0f, 300.0f });
    model.Update({ 600.0f, 350.0f }, true);
    CheckNear(model.Bounds().Width(), 100.0f, 0.01f, "square width");
    CheckNear(model.Bounds().Height(), 100.0f, 0.01f, "square height");

    // 超出画面：拖到右下角外面会被夹回去。
    model.Update({ 2000.0f, 1200.0f }, false);
    CheckTrue(model.Bounds().right <= 1920.0f, "selection is clamped to the limit (right)");
    CheckTrue(model.Bounds().bottom <= 1080.0f, "selection is clamped to the limit (bottom)");

    // 起点也夹。
    core::SelectionModel outside;
    outside.SetLimit(core::Rect{ 0.0f, 0.0f, 100.0f, 100.0f });
    outside.Begin({ -50.0f, -50.0f });
    outside.Update({ 50.0f, 50.0f }, false);
    outside.Commit();
    CheckNear(outside.Bounds().left, 0.0f, 0.01f, "start point is clamped");
    CheckNear(outside.Bounds().top, 0.0f, 0.01f, "start point is clamped vertically");
}

void TestHandles() {
    core::SelectionModel model = Selected({ 100.0f, 100.0f }, { 300.0f, 300.0f });

    CheckTrue(model.HitTest({ 100.0f, 100.0f }) == core::SelectionHandle::TopLeft, "top-left handle");
    CheckTrue(model.HitTest({ 200.0f, 100.0f }) == core::SelectionHandle::Top, "top handle");
    CheckTrue(model.HitTest({ 300.0f, 100.0f }) == core::SelectionHandle::TopRight, "top-right handle");
    CheckTrue(model.HitTest({ 300.0f, 200.0f }) == core::SelectionHandle::Right, "right handle");
    CheckTrue(model.HitTest({ 300.0f, 300.0f }) == core::SelectionHandle::BottomRight,
              "bottom-right handle");
    CheckTrue(model.HitTest({ 200.0f, 300.0f }) == core::SelectionHandle::Bottom, "bottom handle");
    CheckTrue(model.HitTest({ 100.0f, 300.0f }) == core::SelectionHandle::BottomLeft,
              "bottom-left handle");
    CheckTrue(model.HitTest({ 100.0f, 200.0f }) == core::SelectionHandle::Left, "left handle");
    CheckTrue(model.HitTest({ 200.0f, 200.0f }) == core::SelectionHandle::Move, "inside is move");
    CheckTrue(model.HitTest({ 500.0f, 500.0f }) == core::SelectionHandle::None, "outside is none");

    // 把手优先于内部：角点同时也在矩形边界上。
    CheckTrue(model.HitTest({ 102.0f, 102.0f }) == core::SelectionHandle::TopLeft,
              "corner wins over move when close");

    // 没有选中时没有把手。
    core::SelectionModel idle;
    CheckTrue(idle.HitTest({ 0.0f, 0.0f }) == core::SelectionHandle::None, "idle has no handles");
}

void TestResizeAndMove() {
    core::SelectionModel model = Selected({ 100.0f, 100.0f }, { 300.0f, 300.0f });

    CheckTrue(model.ResizeTo(core::SelectionHandle::TopLeft, { 120.0f, 80.0f }), "resize top-left");
    CheckNear(model.Bounds().left, 120.0f, 0.01f, "resized left");
    CheckNear(model.Bounds().top, 80.0f, 0.01f, "resized top");

    CheckTrue(model.ResizeTo(core::SelectionHandle::BottomRight, { 400.0f, 400.0f }),
              "resize bottom-right");
    CheckNear(model.Bounds().right, 400.0f, 0.01f, "resized right");
    CheckNear(model.Bounds().bottom, 400.0f, 0.01f, "resized bottom");

    // 反向拖过头不会产生负数尺寸。
    CheckTrue(model.ResizeTo(core::SelectionHandle::Right, { 0.0f, 200.0f }), "resize past the left edge");
    CheckTrue(model.Bounds().Width() >= core::SelectionModel::kMinSize, "width stays at the minimum");
    CheckTrue(model.Bounds().Height() >= core::SelectionModel::kMinSize, "height stays valid");

    // 移动。
    core::SelectionModel moved = Selected({ 100.0f, 100.0f }, { 200.0f, 200.0f });
    CheckTrue(moved.MoveBy({ 10.0f, -5.0f }), "move");
    CheckNear(moved.Bounds().left, 110.0f, 0.01f, "moved left");
    CheckNear(moved.Bounds().top, 95.0f, 0.01f, "moved top");
    CheckNear(moved.Bounds().Width(), 100.0f, 0.01f, "move keeps the size");

    // 移动撞边：整块推回去，不变形。
    core::SelectionModel bounded = Selected({ 10.0f, 10.0f }, { 110.0f, 60.0f });
    bounded.SetLimit(core::Rect{ 0.0f, 0.0f, 200.0f, 200.0f });
    bounded.MoveBy({ -50.0f, 0.0f });
    CheckNear(bounded.Bounds().left, 0.0f, 0.01f, "move clamps at the left edge");
    CheckNear(bounded.Bounds().Width(), 100.0f, 0.01f, "move keeps its width at the edge");

    // 未选中时移动/调整都是空操作。
    core::SelectionModel idle;
    CheckTrue(!idle.MoveBy({ 1.0f, 1.0f }), "idle cannot move");
    CheckTrue(!idle.ResizeTo(core::SelectionHandle::Left, { 0.0f, 0.0f }), "idle cannot resize");
}

void TestCancel() {
    core::SelectionModel model = Selected({ 100.0f, 100.0f }, { 300.0f, 300.0f });
    model.Cancel();
    CheckTrue(model.Phase() == core::SelectionPhase::Idle, "cancel goes idle");
    CheckTrue(model.Bounds().IsEmpty(), "cancel clears the bounds");
    CheckTrue(model.HitTest({ 200.0f, 200.0f }) == core::SelectionHandle::None, "no handles after cancel");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestDragAndCommit();
    TestConstrainAndLimit();
    TestHandles();
    TestResizeAndMove();
    TestCancel();

    if (failures != 0) {
        std::printf("SELECTION_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("SELECTION_TESTS=OK checks=%d\n", checks);
    return 0;
}
