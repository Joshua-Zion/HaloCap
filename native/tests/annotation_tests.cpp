// S3 的标注模型：九种工具的几何、命中测试、撤销/重做、序号与显示列表。
// 坐标是选区局部像素；文本用一个假栅格器，只验证「该不该出现一次 Blit」。
#include "annotation.h"

#include <cstdio>
#include <string>
#include <vector>

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
        std::printf("FAIL %s: got %.3f want %.3f\n", what, actual, expected);
    }
}

class FakeRasterizer final : public core::IFontRasterizer {
public:
    int layouts = 0;

    core::TextCell LayOut(std::string_view text, const core::FontSpec& spec,
                          core::Point origin) override {
        ++layouts;
        const float width = static_cast<float>(text.size()) * spec.emSizePx * 0.5f;
        const float height = spec.emSizePx;
        return core::TextCell{ 1, core::Rect{ 0.0f, 0.0f, width, height },
                               core::Rect{ origin.x, origin.y, origin.x + width,
                                           origin.y + height } };
    }
};

core::AnnotationStyle Style(core::AnnotationTool tool) {
    core::AnnotationStyle style;
    style.color = core::annotation_defaults::PaletteColor(0);
    style.thickness = 2.0f;
    if (tool == core::AnnotationTool::Highlighter) {
        style.thickness = core::annotation_defaults::kHighlighterThickness;
    }
    return style;
}

core::Annotation MakeTwoPoint(core::AnnotationTool tool, core::Point a, core::Point b) {
    core::Annotation annotation;
    annotation.tool = tool;
    annotation.style = Style(tool);
    annotation.points = { a, b };
    return annotation;
}

void TestGeometry() {
    const core::Rect rect = core::NormalizeRect(core::Point{ 40.0f, 30.0f }, core::Point{ 10.0f, 5.0f });
    CheckNear(rect.left, 10.0f, 0.001f, "normalize left");
    CheckNear(rect.top, 5.0f, 0.001f, "normalize top");
    CheckNear(rect.right, 40.0f, 0.001f, "normalize right");
    CheckNear(rect.bottom, 30.0f, 0.001f, "normalize bottom");

    const core::Point square = core::ConstrainSquare(core::Point{ 0.0f, 0.0f }, core::Point{ 30.0f, 10.0f });
    CheckNear(square.x, 30.0f, 0.001f, "square keeps the longer axis");
    CheckNear(square.y, 30.0f, 0.001f, "square mirrors the longer axis");

    const core::Point snapped = core::SnapToAngle(core::Point{ 0.0f, 0.0f }, core::Point{ 10.0f, 3.0f });
    CheckNear(snapped.y, 0.0f, 0.001f, "45 degree snap flattens a shallow line");
    const core::Point diagonal = core::SnapToAngle(core::Point{ 0.0f, 0.0f }, core::Point{ 10.0f, 8.0f });
    CheckNear(diagonal.x, diagonal.y, 0.001f, "45 degree snap on the diagonal");

    CheckTrue(core::IsLightColor(core::Color{ 1.0f, 1.0f, 1.0f, 1.0f }), "white is light");
    CheckTrue(!core::IsLightColor(core::Color{ 0.1f, 0.1f, 0.1f, 1.0f }), "dark grey is not light");
    CheckTrue(core::annotation_defaults::PaletteColor(7).r == 1.0f, "palette has eight entries");
}

void TestHitTest() {
    core::Annotation pen;
    pen.tool = core::AnnotationTool::Pen;
    pen.style = Style(core::AnnotationTool::Pen);
    pen.points = { { 10.0f, 10.0f }, { 110.0f, 10.0f } };
    CheckTrue(core::HitTest(pen, core::Point{ 60.0f, 12.0f }, 2.0f), "pen hit near the stroke");
    CheckTrue(!core::HitTest(pen, core::Point{ 60.0f, 40.0f }, 2.0f), "pen miss far from the stroke");

    core::Annotation rectangle = MakeTwoPoint(core::AnnotationTool::Rectangle, { 10.0f, 10.0f },
                                              { 110.0f, 60.0f });
    CheckTrue(core::HitTest(rectangle, core::Point{ 60.0f, 10.0f }, 2.0f), "rectangle hits its edge");
    CheckTrue(!core::HitTest(rectangle, core::Point{ 60.0f, 35.0f }, 2.0f),
              "unfilled rectangle misses its inside");
    rectangle.style.filled = true;
    CheckTrue(core::HitTest(rectangle, core::Point{ 60.0f, 35.0f }, 2.0f), "filled rectangle hits inside");

    const core::Annotation ellipse = MakeTwoPoint(core::AnnotationTool::Ellipse, { 0.0f, 0.0f },
                                                  { 100.0f, 50.0f });
    CheckTrue(core::HitTest(ellipse, core::Point{ 100.0f, 25.0f }, 3.0f), "ellipse hits its rim");
    CheckTrue(!core::HitTest(ellipse, core::Point{ 50.0f, 25.0f }, 3.0f), "ellipse misses its centre");

    const core::Annotation arrow = MakeTwoPoint(core::AnnotationTool::Arrow, { 0.0f, 0.0f },
                                                { 100.0f, 100.0f });
    CheckTrue(core::HitTest(arrow, core::Point{ 50.0f, 50.0f }, 2.0f), "arrow hits the shaft");

    core::Annotation step;
    step.tool = core::AnnotationTool::Step;
    step.points = { { 100.0f, 100.0f } };
    CheckTrue(core::HitTest(step, core::Point{ 108.0f, 100.0f }, 1.0f), "step circle hits");
    CheckTrue(!core::HitTest(step, core::Point{ 130.0f, 100.0f }, 1.0f), "step circle misses");

    core::Annotation text;
    text.tool = core::AnnotationTool::Text;
    text.points = { { 10.0f, 10.0f } };
    text.textBounds = core::Rect{ 10.0f, 10.0f, 90.0f, 40.0f };
    CheckTrue(core::HitTest(text, core::Point{ 50.0f, 20.0f }, 0.0f), "text hits its bounds");
    CheckTrue(!core::HitTest(text, core::Point{ 200.0f, 20.0f }, 0.0f), "text misses outside bounds");

    const core::Annotation mosaic = MakeTwoPoint(core::AnnotationTool::Mosaic, { 0.0f, 0.0f },
                                                 { 50.0f, 50.0f });
    CheckTrue(core::HitTest(mosaic, core::Point{ 25.0f, 25.0f }, 0.0f), "mosaic hits inside");
}

void TestErase() {
    std::vector<core::Annotation> items = {
        MakeTwoPoint(core::AnnotationTool::Rectangle, { 0.0f, 0.0f }, { 100.0f, 100.0f }),
        MakeTwoPoint(core::AnnotationTool::Rectangle, { 200.0f, 0.0f }, { 300.0f, 100.0f }),
    };

    CheckEqual(core::EraseAt(items, core::Point{ 0.0f, 50.0f }, 3.0f), 1, "one rectangle erased");
    CheckEqual(static_cast<long long>(items.size()), 1, "one rectangle left");
    CheckEqual(core::EraseAt(items, core::Point{ 0.0f, 50.0f }, 3.0f), 0, "erasing nothing is zero");
}

void TestDocument() {
    core::AnnotationDocument document;

    document.Begin(core::AnnotationTool::Pen, Style(core::AnnotationTool::Pen));
    document.AddPoint({ 10.0f, 10.0f });
    document.AddPoint({ 20.0f, 12.0f });
    document.Commit();

    CheckEqual(static_cast<long long>(document.Items().size()), 1, "one pen stroke committed");
    CheckTrue(document.Undo(), "undo works");
    CheckEqual(static_cast<long long>(document.Items().size()), 0, "undo removed the stroke");
    CheckTrue(document.Redo(), "redo works");
    CheckEqual(static_cast<long long>(document.Items().size()), 1, "redo restored the stroke");

    // 新标注会清掉 redo 分支。
    document.Undo();
    document.Begin(core::AnnotationTool::Rectangle, Style(core::AnnotationTool::Rectangle));
    document.SetEnd({ 0.0f, 0.0f });
    document.SetEnd({ 50.0f, 50.0f });
    document.Commit();
    CheckTrue(!document.Redo(), "a new commit clears the redo branch");

    // 橡皮也是一次快照：撤销能把它救回来。
    CheckEqual(document.EraseAt(core::Point{ 0.0f, 0.0f }, 3.0f), 1, "eraser removes the rectangle");
    CheckTrue(document.Undo(), "undo after erase");
    CheckEqual(static_cast<long long>(document.Items().size()), 1, "erase is undone");

    // 序号从 1 起递增。
    core::AnnotationDocument steps;
    steps.Begin(core::AnnotationTool::Step, Style(core::AnnotationTool::Step));
    steps.SetEnd({ 0.0f, 0.0f });
    steps.SetEnd({ 0.0f, 0.0f });
    steps.Commit();
    steps.Begin(core::AnnotationTool::Step, Style(core::AnnotationTool::Step));
    steps.SetEnd({ 10.0f, 10.0f });
    steps.SetEnd({ 10.0f, 10.0f });
    steps.Commit();
    CheckEqual(static_cast<long long>(steps.Items().size()), 2, "two steps");
    if (steps.Items().size() == 2) {
        CheckTrue(steps.Items()[0].text == "1", "first step is 1");
        CheckTrue(steps.Items()[1].text == "2", "second step is 2");
    }

    // 取消不落盘。
    core::AnnotationDocument cancelled;
    cancelled.Begin(core::AnnotationTool::Arrow, Style(core::AnnotationTool::Arrow));
    cancelled.SetEnd({ 0.0f, 0.0f });
    cancelled.SetEnd({ 10.0f, 10.0f });
    cancelled.Cancel();
    CheckEqual(static_cast<long long>(cancelled.Items().size()), 0, "cancel discards the stroke");

    // 抽稀：太近的点不收。
    core::AnnotationDocument thinned;
    thinned.Begin(core::AnnotationTool::Pen, Style(core::AnnotationTool::Pen));
    thinned.AddPoint({ 0.0f, 0.0f });
    thinned.AddPoint({ 0.5f, 0.5f }, 2.0f);
    thinned.AddPoint({ 5.0f, 0.0f }, 2.0f);
    CheckEqual(static_cast<long long>(thinned.Active().points.size()), 2,
               "points closer than the threshold are dropped");
}

void TestPlan() {
    FakeRasterizer rasterizer;

    // 画笔：N 个点 → N-1 条线段。
    core::FramePlan penPlan;
    std::vector<core::Annotation> pens;
    core::Annotation pen;
    pen.tool = core::AnnotationTool::Pen;
    pen.style = Style(core::AnnotationTool::Pen);
    pen.points = { { 0.0f, 0.0f }, { 10.0f, 0.0f }, { 20.0f, 0.0f } };
    pens.push_back(pen);
    core::AnnotationPlanOptions options;
    options.rasterizer = &rasterizer;
    core::AppendAnnotationPlan(pens, options, penPlan);
    CheckEqual(static_cast<long long>(penPlan.ItemCount()), 2, "pen produces one segment per pair");

    // 荧光笔：alpha 被压到 0.42。
    core::FramePlan highlighterPlan;
    std::vector<core::Annotation> highlighters;
    core::Annotation highlighter = pen;
    highlighter.tool = core::AnnotationTool::Highlighter;
    highlighter.style = Style(core::AnnotationTool::Highlighter);
    highlighters.push_back(highlighter);
    core::AppendAnnotationPlan(highlighters, options, highlighterPlan);
    CheckEqual(static_cast<long long>(highlighterPlan.ItemCount()), 2, "highlighter has two segments");
    if (const auto* segment = std::get_if<core::Segment>(&highlighterPlan.Items()[0])) {
        CheckNear(segment->color.a, core::annotation_defaults::kHighlighterAlpha, 0.001f,
                  "highlighter alpha");
    } else {
        ++checks;
        ++failures;
        std::printf("FAIL highlighter item is a segment\n");
    }

    // 矩形/椭圆：描边与填充各一个图元。
    std::vector<core::Annotation> shapes;
    shapes.push_back(MakeTwoPoint(core::AnnotationTool::Rectangle, { 0.0f, 0.0f }, { 100.0f, 50.0f }));
    shapes.push_back(MakeTwoPoint(core::AnnotationTool::Ellipse, { 0.0f, 0.0f }, { 100.0f, 50.0f }));
    core::FramePlan shapePlan;
    core::AppendAnnotationPlan(shapes, options, shapePlan);
    CheckEqual(static_cast<long long>(shapePlan.ItemCount()), 2, "one item per shape");
    CheckTrue(std::get_if<core::StrokeRect>(&shapePlan.Items()[0]) != nullptr, "rectangle is a stroke rect");
    CheckTrue(std::get_if<core::StrokeEllipse>(&shapePlan.Items()[1]) != nullptr,
              "ellipse is a stroke ellipse");

    // 箭头：杆 + 两个箭头翼。
    core::FramePlan arrowPlan;
    std::vector<core::Annotation> arrows;
    arrows.push_back(MakeTwoPoint(core::AnnotationTool::Arrow, { 0.0f, 0.0f }, { 100.0f, 0.0f }));
    core::AppendAnnotationPlan(arrows, options, arrowPlan);
    CheckEqual(static_cast<long long>(arrowPlan.ItemCount()), 3, "arrow shaft plus two head wings");

    // 文字：一次 Blit。
    core::FramePlan textPlan;
    std::vector<core::Annotation> texts;
    core::Annotation text;
    text.tool = core::AnnotationTool::Text;
    text.style = Style(core::AnnotationTool::Text);
    text.points = { { 5.0f, 5.0f } };
    text.text = "hello";
    texts.push_back(text);
    core::AppendAnnotationPlan(texts, options, textPlan);
    CheckEqual(static_cast<long long>(textPlan.ItemCount()), 1, "text is one blit");
    CheckTrue(std::get_if<core::Blit>(&textPlan.Items()[0]) != nullptr, "text item is a blit");

    // 没有栅格器时文本与序号跳过，而不是崩。
    core::AnnotationPlanOptions noRasterizer;
    core::FramePlan noTextPlan;
    core::AppendAnnotationPlan(texts, noRasterizer, noTextPlan);
    CheckEqual(static_cast<long long>(noTextPlan.ItemCount()), 0, "text without a rasterizer is skipped");

    // 序号：填充圆 + 描边圆 + 数字。
    core::FramePlan stepPlan;
    std::vector<core::Annotation> steps;
    core::Annotation step;
    step.tool = core::AnnotationTool::Step;
    step.style = Style(core::AnnotationTool::Step);
    step.points = { { 50.0f, 50.0f } };
    step.text = "3";
    steps.push_back(step);
    core::AppendAnnotationPlan(steps, options, stepPlan);
    CheckEqual(static_cast<long long>(stepPlan.ItemCount()), 3, "step is fill + stroke + digit");
    CheckTrue(std::get_if<core::FillEllipse>(&stepPlan.Items()[0]) != nullptr, "step fill circle");
    CheckTrue(std::get_if<core::StrokeEllipse>(&stepPlan.Items()[1]) != nullptr, "step ring");
    CheckTrue(std::get_if<core::Blit>(&stepPlan.Items()[2]) != nullptr, "step digit");

    // 马赛克：没有块句柄时跳过，有句柄时一次 Blit。
    core::FramePlan mosaicPlan;
    std::vector<core::Annotation> mosaics;
    core::Annotation mosaic = MakeTwoPoint(core::AnnotationTool::Mosaic, { 0.0f, 0.0f }, { 40.0f, 40.0f });
    mosaics.push_back(mosaic);
    core::AppendAnnotationPlan(mosaics, options, mosaicPlan);
    CheckEqual(static_cast<long long>(mosaicPlan.ItemCount()), 0, "mosaic without a tile is skipped");

    mosaics[0].mosaicTile = 9;
    mosaics[0].mosaicTileSize = core::Size{ 4.0f, 4.0f };
    core::FramePlan mosaicPlan2;
    core::AppendAnnotationPlan(mosaics, options, mosaicPlan2);
    CheckEqual(static_cast<long long>(mosaicPlan2.ItemCount()), 1, "mosaic with a tile is one blit");

    // scale 应用到坐标与线宽。
    core::FramePlan scaledPlan;
    core::AnnotationPlanOptions scaled = options;
    scaled.scale = 2.0f;
    core::AppendAnnotationPlan(pens, scaled, scaledPlan);
    if (const auto* segment = std::get_if<core::Segment>(&scaledPlan.Items()[0])) {
        CheckNear(segment->from.x, 0.0f, 0.001f, "scaled start x");
        CheckNear(segment->to.x, 20.0f, 0.001f, "scaled end x");
        CheckNear(segment->thickness, 4.0f, 0.001f, "scaled thickness");
    }
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestGeometry();
    TestHitTest();
    TestErase();
    TestDocument();
    TestPlan();

    if (failures != 0) {
        std::printf("ANNOTATION_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("ANNOTATION_TESTS=OK checks=%d\n", checks);
    return 0;
}
