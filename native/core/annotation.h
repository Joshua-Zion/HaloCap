#pragma once

#include "display_list.h"
#include "font_rasterizer.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// S3 的标注模型：九种工具、样式、命中测试、撤销/重做与显示列表生成。
// 坐标一律是「选区局部像素」：截图区域左上角是 (0,0)，这样导出时不需要再做变换。
//
// 与 1.x 的 AnnotationModels/AnnotationRenderer/AnnotationHitTester 同语义，
// 差别只在平台：1.x 画进 WPF 的 DrawingContext，这里只产显示列表（§3.3 规则一）。

enum class AnnotationTool : std::int32_t {
    Pen = 0,
    Rectangle = 1,
    Ellipse = 2,
    Arrow = 3,
    Text = 4,
    Highlighter = 5,
    Mosaic = 6,
    Step = 7,       // 序号圆
    Eraser = 8,     // 橡皮不是图元，是一个动作
};

enum class ArrowHeadMode : std::int32_t {
    End = 0,
    Both = 1,
    None = 2,
};

// 1.x AnnotationDefaults 的原值。工具栏的滑杆范围也用它，不另立一套。
namespace annotation_defaults {
constexpr float kThickness = 2.0f;
constexpr float kMinThickness = 1.0f;
constexpr float kMaxThickness = 24.0f;
constexpr float kHighlighterThickness = 14.0f;
constexpr float kTextSize = 18.0f;
constexpr float kMinTextSize = 10.0f;
constexpr float kMaxTextSize = 48.0f;
constexpr int kMosaicBlockPx = 12;
constexpr float kEraserSize = 18.0f;
constexpr float kStepRadius = 11.0f;
constexpr float kHighlighterAlpha = 0.42f;
constexpr float kArrowHeadSpreadRadians = 0.44879895f;   // pi / 7
constexpr float kMinArrowHeadPx = 8.0f;
constexpr int kPaletteCount = 8;

// 1.x 的色板（红、橙、黄、绿、蓝、紫、深灰、白）。
Color PaletteColor(int index);
}  // namespace annotation_defaults

struct AnnotationStyle {
    Color color = annotation_defaults::PaletteColor(0);
    float thickness = annotation_defaults::kThickness;
    float textSize = annotation_defaults::kTextSize;
    bool bold = true;
    bool italic = false;
    bool filled = false;
    ArrowHeadMode arrowHead = ArrowHeadMode::End;
    int mosaicBlockPx = annotation_defaults::kMosaicBlockPx;
};

struct Annotation {
    AnnotationTool tool = AnnotationTool::Pen;
    AnnotationStyle style;
    std::vector<Point> points;

    // Text 的内容；Step 的编号（"1"…）。其它工具为空。
    std::string text;

    // Text 排版后的边界（由 AddText 填），命中测试用它而不是再排版一次。
    Rect textBounds{};

    // 马赛克块的句柄：由调用方把降采样后的格子注册到后端资源表后填进来。
    // 0 表示还没准备好，计划生成时跳过这一块。
    ImageHandle mosaicTile = 0;
    Size mosaicTileSize{};
};

// ---- 几何（与 1.x AnnotationGeometry 逐条对齐） ----

Rect NormalizeRect(Point a, Point b);
Point ConstrainSquare(Point start, Point end);
Point SnapToAngle(Point start, Point end);
bool IsLightColor(const Color& color);

// ---- 命中测试与擦除 ----

// radius 是「手指/光标额外容忍度」（像素）。橡皮传它自己的半径。
bool HitTest(const Annotation& annotation, Point point, float radius);

// 擦掉命中的标注，返回删掉的数量。
int EraseAt(std::vector<Annotation>& annotations, Point point, float radius);

// ---- 显示列表 ----

struct AnnotationPlanOptions {
    // 文本与序号需要排版；空指针时这两类跳过（导出/预览都必须有它）。
    IFontRasterizer* rasterizer = nullptr;
    std::string fontFamily = "Microsoft YaHei UI";

    // DIP → 像素的缩放。选区是 1:1 时是 1.0；预览缩放时由调用方给。
    float scale = 1.0f;
};

void AppendAnnotationPlan(const std::vector<Annotation>& annotations,
                          const AnnotationPlanOptions& options, FramePlan& plan);

// ---- 文档：撤销/重做与序号 ----

// 撤销用「整份快照」而不是逐操作反演：一次截图里的标注是几十条量级，快照更不容易错，
// 而橡皮的删除、序号回退、未来的移动/缩放都不需要各写一套 undo 逻辑。
class AnnotationDocument {
public:
    AnnotationDocument();

    // 开始一条新标注。Step 会自动分配下一个序号并写进 text。
    Annotation& Begin(AnnotationTool tool, const AnnotationStyle& style);

    bool Drawing() const { return drawing_; }

    Annotation& Active() { return current_; }
    const Annotation& Active() const { return current_; }

    // 画笔/荧光笔的抽稀：距上一个点太近就不收（1.x 也会丢，否则点密度没有意义）。
    void AddPoint(Point point, float minDistance = 0.0f);

    // 两点工具的终点（矩形/椭圆/箭头/马赛克/文字/序号）。第一次调用补第二个点。
    void SetEnd(Point point);

    void Commit();
    void Cancel();

    // 橡皮：删掉命中的标注并落一次快照。返回删掉的数量。
    int EraseAt(Point point, float radius);

    bool Undo();
    bool Redo();
    void Clear();

    const std::vector<Annotation>& Items() const { return items_; }

    std::size_t UndoDepth() const { return cursor_; }
    std::size_t HistorySize() const { return history_.size(); }

    // 序号从 1 起；Clear 之后重新从 1 开始。
    int NextStepNumber() const { return nextStep_; }

private:
    void Snapshot();

    std::vector<Annotation> items_;
    Annotation current_;
    bool drawing_ = false;

    // history_[cursor_] 永远等于 items_ 的当前状态。
    std::vector<std::vector<Annotation>> history_;
    std::size_t cursor_ = 0;

    int nextStep_ = 1;
};

}
