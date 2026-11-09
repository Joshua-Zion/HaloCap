#pragma once

#include "display_list.h"
#include "geometry.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core::ocr {

// S4 的 OCR 前后处理：图片预处理、DBNet 检测后处理、CTC 解码、按行排版与搜索。
// 模型推理在平台侧（IOcrEngine），这一层只处理「数组进、数组出」，所以能完全单测。
//
// 与 1.x 的 OcrImageProcessor/ModelOcrEngine 同语义：
// 检测输入按最长边缩到 targetSide（不放大），补到 32 的倍数，ImageNet 均值方差归一化；
// 检测输出是概率图，阈值 + 连通域 + unclip 得到文本框；识别按 CTC 去空白、去重复。

struct OcrImage {
    const std::uint8_t* bgra = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
};

struct OcrOptions {
    float detThreshold = 0.30f;   // 概率图二值化
    float boxThreshold = 0.60f;   // 连通域平均分下限
    float unclipRatio = 1.7f;     // 文本框外扩（DBNet 的 mask 是收缩过的）
    float minBoxWidth = 8.0f;     // 小于这个尺寸的检测框当噪声（图标、勾选框）
    float minBoxHeight = 6.0f;
    int detTargetSide = 960;      // 最长边
    int recHeight = 48;           // 识别输入高
    // 1.x 的 RapidOcr 用 TextScore=0.4；低于这个值的行基本都是噪点/乱码。
    float minLineConfidence = 0.35f;
    int maxLines = 200;
};

struct OcrLine {
    std::string text;
    Rect box;                 // 原图坐标
    float confidence = 0.0f;
};

struct OcrResult {
    std::vector<OcrLine> lines;
    std::string plainText;
};

// 检测输入：NCHW float，尺寸补到 32 的倍数。
struct DetInput {
    std::vector<float> data;   // 3 * height * width
    int width = 0;
    int height = 0;
    float scaleX = 1.0f;       // 原图 → 网络
    float scaleY = 1.0f;
    int padX = 0;              // 网络坐标 → 原图要减掉的边距（缩放前）
    int padY = 0;
};

DetInput PrepareDetInput(const OcrImage& image, int targetSide);

// probability 是检测网络的输出（宽高与 DetInput 一致）。
std::vector<Rect> ExtractTextBoxes(const float* probability, int width, int height,
                                   const DetInput& input, const OcrOptions& options);

// 识别输入：一条文本行裁出来，缩放到 recHeight，右侧补 0 到 width 的倍数（这里不补）。
struct RecInput {
    std::vector<float> data;   // 3 * height * width
    int width = 0;
    int height = 0;
};

RecInput PrepareRecInput(const OcrImage& image, Rect box, int recHeight);

// logits 形状 [timeSteps, classes]，第 0 类为 blank。
std::string DecodeCtc(const float* logits, int timeSteps, int classes,
                      const std::vector<std::string>& dictionary, float* confidence);

// 把识别结果按阅读顺序排好（同一行的按 x 排，不同行按 y 排）并生成纯文本。
OcrResult BuildResult(std::vector<OcrLine> lines, float lineTolerance = 6.0f);

std::vector<std::size_t> FindMatchingLines(const OcrResult& result, std::string_view query);

// 结果窗口的「复制全部」与调试输出的共用格式。
std::string ToPlainText(const OcrResult& result);

}
