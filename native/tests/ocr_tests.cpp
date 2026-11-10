// S4 的 OCR 前后处理：检测输入、检测后处理、识别输入、CTC 解码、按行排版与搜索。
#include "ocr.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {

namespace core = halocap::core;
namespace ocr = halocap::core::ocr;

int failures = 0;
int checks = 0;

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckNear(float actual, float expected, float tolerance, const char* what) {
    ++checks;
    if (std::fabs(actual - expected) > tolerance) {
        ++failures;
        std::printf("FAIL %s: got %.3f want %.3f\n", what, actual, expected);
    }
}

std::vector<std::uint8_t> MakeBgra(int width, int height) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t offset = (static_cast<std::size_t>(y) * width + x) * 4;
            pixels[offset + 0] = static_cast<std::uint8_t>(x * 255 / std::max(1, width - 1));
            pixels[offset + 1] = static_cast<std::uint8_t>(y * 255 / std::max(1, height - 1));
            pixels[offset + 2] = 128;
            pixels[offset + 3] = 255;
        }
    }
    return pixels;
}

void TestPrepareDetInput() {
    const int width = 100;
    const int height = 50;
    const std::vector<std::uint8_t> pixels = MakeBgra(width, height);
    const ocr::OcrImage image{ pixels.data(), width, height, width * 4 };

    const ocr::DetInput input = ocr::PrepareDetInput(image, 64);
    CheckEqual(input.width, 64, "det width is padded to a multiple of 32");
    CheckEqual(input.height, 32, "det height is padded to a multiple of 32");
    CheckNear(input.scaleX, 0.64f, 0.001f, "det scale x");
    CheckNear(input.scaleY, 0.64f, 0.001f, "det scale y");
    CheckEqual(static_cast<long long>(input.data.size()), 3LL * 64 * 32, "det NCHW size");

    // 左上角的 B 通道是 0 → (0 - 0.485) / 0.229。
    CheckNear(input.data[0], (0.0f - 0.485f) / 0.229f, 0.01f, "det normalisation of B");
}

void TestExtractTextBoxes() {
    const int width = 64;
    const int height = 32;
    std::vector<float> probability(static_cast<std::size_t>(width) * height, 0.01f);

    for (int y = 8; y < 20; ++y) {
        for (int x = 10; x < 30; ++x) {
            probability[static_cast<std::size_t>(y) * width + x] = 0.9f;
        }
    }
    // 第二个框的平均分低于 boxThreshold，应该被丢掉。
    for (int y = 22; y < 30; ++y) {
        for (int x = 40; x < 55; ++x) {
            probability[static_cast<std::size_t>(y) * width + x] = 0.4f;
        }
    }

    ocr::DetInput input;
    input.width = width;
    input.height = height;
    input.scaleX = 0.64f;
    input.scaleY = 0.64f;

    ocr::OcrOptions options;
    const std::vector<core::Rect> boxes =
        ocr::ExtractTextBoxes(probability.data(), width, height, input, options);

    CheckEqual(static_cast<long long>(boxes.size()), 1, "only the confident box survives");
    if (!boxes.empty()) {
        // 10..30, 8..20 在 0.64 缩放下约等于原图 15.6..46.9 / 12.5..31.2，加 unclip。
        CheckTrue(boxes[0].left < 16.0f, "box left maps back to the original image");
        CheckTrue(boxes[0].right > 45.0f, "box right maps back to the original image");
        CheckTrue(boxes[0].Width() > 25.0f, "unclip grows the box");
    }
}

void TestPrepareRecInput() {
    const int width = 100;
    const int height = 50;
    const std::vector<std::uint8_t> pixels = MakeBgra(width, height);
    const ocr::OcrImage image{ pixels.data(), width, height, width * 4 };

    // 裁图四周留 6 px：40x20 的框变成 52x32，宽高比 1.625 → 48 高时宽 78。
    const ocr::RecInput input =
        ocr::PrepareRecInput(image, core::Rect{ 10.0f, 10.0f, 50.0f, 30.0f }, 48);
    CheckEqual(input.height, 48, "rec height");
    CheckEqual(input.width, 78, "rec keeps the aspect ratio with padding");
    CheckEqual(static_cast<long long>(input.data.size()), 3LL * 78 * 48, "rec NCHW size");
    CheckNear(input.data[0], (0.039f - 0.5f) / 0.5f, 0.06f, "rec normalisation");
}

void TestDecodeCtc() {
    const std::vector<std::string> dictionary = { "A", "B", "C" };
    const int classes = 4;
    const int timeSteps = 5;

    // blank, A, A, blank, B → "AB"
    std::vector<float> logits(static_cast<std::size_t>(timeSteps) * classes, 0.1f);
    logits[0 * classes + 0] = 5.0f;
    logits[1 * classes + 1] = 5.0f;
    logits[2 * classes + 1] = 5.0f;
    logits[3 * classes + 0] = 5.0f;
    logits[4 * classes + 2] = 5.0f;

    float confidence = 0.0f;
    const std::string text = ocr::DecodeCtc(logits.data(), timeSteps, classes, dictionary, &confidence);
    CheckTrue(text == "AB", "CTC collapses blanks and repeated characters");
    CheckTrue(confidence > 0.5f, "CTC confidence is the softmax of the winning class");

    // 已 softmax 的两行：第一字符 0.9、第二字符 0.4 → 置信度取平均 0.65，不是最低分 0.4。
    // 1.x RapidOcr 的 TextScore 就是块内字符分均值：一个难字不该把整行判死。
    std::vector<float> mixed(static_cast<std::size_t>(3) * classes, 0.0f);
    mixed[0 * classes + 0] = 0.1f;
    mixed[0 * classes + 1] = 0.9f;
    mixed[1 * classes + 0] = 1.0f;
    mixed[2 * classes + 0] = 0.35f;
    mixed[2 * classes + 2] = 0.4f;
    mixed[2 * classes + 3] = 0.25f;
    float meanConfidence = 0.0f;
    const std::string mixedText =
        ocr::DecodeCtc(mixed.data(), 3, classes, dictionary, &meanConfidence);
    CheckTrue(mixedText == "AB", "mixed text decodes");
    CheckNear(meanConfidence, 0.65f, 0.02f, "CTC confidence is the mean of character scores");

    // 空 logits 返回空串而不是崩。
    float emptyConfidence = 1.0f;
    CheckTrue(ocr::DecodeCtc(nullptr, 0, 0, dictionary, &emptyConfidence).empty(), "empty logits");
    CheckNear(emptyConfidence, 0.0f, 0.001f, "empty confidence");
}

void TestBuildResult() {
    std::vector<ocr::OcrLine> lines;
    // 同一行两个不相邻的框（间距 > 0.6 倍行高，不会触发行内碎片合并）。
    lines.push_back({ "right", core::Rect{ 80.0f, 100.0f, 150.0f, 120.0f }, 0.9f });
    lines.push_back({ "top", core::Rect{ 10.0f, 10.0f, 80.0f, 30.0f }, 0.9f });
    lines.push_back({ "left", core::Rect{ 10.0f, 100.0f, 45.0f, 120.0f }, 0.9f });

    const ocr::OcrResult result = ocr::BuildResult(lines);
    CheckEqual(static_cast<long long>(result.lines.size()), 3, "three lines");
    if (result.lines.size() == 3) {
        CheckTrue(result.lines[0].text == "top", "top row first");
        CheckTrue(result.lines[1].text == "left", "same row sorts by x");
        CheckTrue(result.lines[2].text == "right", "same row sorts by x (2)");
    }
    CheckTrue(result.plainText == "top\r\nleft\r\nright", "plain text joins lines with CRLF");

    const std::vector<std::size_t> matches = ocr::FindMatchingLines(result, "LEF");
    CheckEqual(static_cast<long long>(matches.size()), 1, "search is case-insensitive");
    if (!matches.empty()) {
        CheckEqual(static_cast<long long>(matches[0]), 1, "search returns the right line");
    }
    CheckEqual(static_cast<long long>(ocr::FindMatchingLines(result, "zzz").size()), 0,
               "no match is empty");
}

void TestBuildResultMergesRowFragments() {
    std::vector<ocr::OcrLine> lines;
    // 同一行三个碎片：两个挨着（6 px = 0.3 倍行高）→ 拼成 "Halo Cap"；
    // 第三个隔了 95 px → 仍是单独一行。
    lines.push_back({ "Halo", core::Rect{ 10.0f, 10.0f, 60.0f, 30.0f }, 0.9f });
    lines.push_back({ "Cap", core::Rect{ 66.0f, 10.0f, 105.0f, 30.0f }, 0.9f });
    lines.push_back({ "far", core::Rect{ 200.0f, 10.0f, 240.0f, 30.0f }, 0.9f });
    // 中文碎片之间不加空格。
    lines.push_back({ "截图", core::Rect{ 10.0f, 60.0f, 50.0f, 80.0f }, 0.9f });
    lines.push_back({ "工具", core::Rect{ 55.0f, 60.0f, 95.0f, 80.0f }, 0.9f });

    const ocr::OcrResult result = ocr::BuildResult(lines);
    CheckEqual(static_cast<long long>(result.lines.size()), 3, "row fragments merged");
    if (result.lines.size() == 3) {
        CheckTrue(result.lines[0].text == "Halo Cap", "latin fragments join with a space");
        CheckTrue(result.lines[1].text == "far", "distant fragment stays separate");
        CheckTrue(result.lines[2].text == "截图工具", "cjk fragments join without a space");
    }
}

void TestBuildResultSanitize() {
    std::vector<ocr::OcrLine> lines;
    // "A◆B�C"（◆=U+25C6，�=U+FFFD）→ 去掉假字符后再去首尾空白。
    lines.push_back({ "  A" "\xE2\x97\x86" "B" "\xEF\xBF\xBD" "C  ",
                      core::Rect{ 10.0f, 10.0f, 120.0f, 30.0f }, 0.9f });
    // 只有符号 / 只有空白的行整行丢掉。
    lines.push_back({ "\xE2\x97\x86", core::Rect{ 10.0f, 40.0f, 60.0f, 60.0f }, 0.9f });
    lines.push_back({ "   ", core::Rect{ 10.0f, 70.0f, 60.0f, 90.0f }, 0.9f });

    const ocr::OcrResult result = ocr::BuildResult(lines);
    CheckEqual(static_cast<long long>(result.lines.size()), 1, "junk-only lines dropped");
    if (!result.lines.empty()) {
        CheckTrue(result.lines[0].text == "ABC", "junk characters stripped and trimmed");
    }
}

// 真机回归：一行在破折号处被 det 切成两段，下一行的短框被 unclip 撑高后，
// 与上一行的框竖直方向蹭到 6 px。旧逻辑（与首框的重叠 >= 6px 就并进同一行）
// 会把下一行并进来，再按 left 排序，下一行（left 2.9 < 3.0）反而排到了最前，
// 整句顺序就乱了——用户截图里 "中)。" 跑到第一行就是这个原因。
void TestBuildResultKeepsTightRowOrder() {
    std::vector<ocr::OcrLine> lines;
    lines.push_back(
        { "原图预览不再画红色识别框", core::Rect{ 3.0f, 0.0f, 302.0f, 15.8f }, 0.9f });
    lines.push_back({ "现在只有原图本身；", core::Rect{ 325.0f, 0.6f, 739.0f, 11.4f }, 0.9f });
    lines.push_back({ "命中)。", core::Rect{ 2.9f, 9.8f, 52.1f, 35.0f }, 0.8f });

    const ocr::OcrResult result = ocr::BuildResult(lines);
    CheckEqual(static_cast<long long>(result.lines.size()), 3, "tight rows stay three lines");
    CheckTrue(result.plainText ==
                  "原图预览不再画红色识别框\r\n现在只有原图本身；\r\n命中)。",
              "tight rows keep reading order");
    if (!result.lines.empty()) {
        CheckTrue(result.lines.back().text == "命中)。", "second row stays last");
    }
}

// 行聚类：同一行的碎片按 left 排序，竖直中心拉开半个行高以上就分行。
void TestGroupTextBoxesIntoLines() {
    std::vector<core::Rect> boxes;
    boxes.push_back(core::Rect{ 300.0f, 1.0f, 700.0f, 15.0f });   // 0: 第一行右段
    boxes.push_back(core::Rect{ 10.0f, 0.0f, 290.0f, 16.0f });    // 1: 第一行左段
    boxes.push_back(core::Rect{ 10.0f, 20.0f, 200.0f, 36.0f });   // 2: 第二行
    boxes.push_back(core::Rect{ 10.0f, 40.0f, 200.0f, 56.0f });   // 3: 第三行

    const std::vector<std::vector<std::size_t>> rows = ocr::GroupTextBoxesIntoLines(boxes);
    CheckEqual(static_cast<long long>(rows.size()), 3, "three rows");
    if (rows.size() == 3) {
        CheckEqual(static_cast<long long>(rows[0].size()), 2, "first row has two fragments");
        if (rows[0].size() == 2) {
            CheckEqual(static_cast<long long>(rows[0][0]), 1, "row sorts by left (first)");
            CheckEqual(static_cast<long long>(rows[0][1]), 0, "row sorts by left (second)");
        }
        CheckEqual(static_cast<long long>(rows[1][0]), 2, "second row index");
        CheckEqual(static_cast<long long>(rows[2][0]), 3, "third row index");
    }
}

// 识别前合并：间距 2 倍行高的破折号切段要并回整行；隔了 8 倍行高的列不并。
void TestMergeRowBoxes() {
    std::vector<core::Rect> boxes;
    boxes.push_back(core::Rect{ 0.0f, 0.0f, 300.0f, 16.0f });
    boxes.push_back(core::Rect{ 322.0f, 1.0f, 700.0f, 15.0f });
    boxes.push_back(core::Rect{ 860.0f, 0.0f, 1000.0f, 16.0f });

    const std::vector<std::size_t> row{ 0, 1, 2 };
    const std::vector<core::Rect> merged = ocr::MergeRowBoxes(boxes, row, 2.5f);
    CheckEqual(static_cast<long long>(merged.size()), 2, "adjacent fragments merge before rec");
    if (merged.size() == 2) {
        CheckNear(merged[0].left, 0.0f, 0.01f, "merged box left");
        CheckNear(merged[0].right, 700.0f, 0.01f, "merged box right");
        CheckNear(merged[1].left, 860.0f, 0.01f, "distant column stays separate");
    }

    // 高度差超过 1.6 倍的（标题 + 小图标）不并。
    std::vector<core::Rect> uneven;
    uneven.push_back(core::Rect{ 0.0f, 0.0f, 120.0f, 30.0f });
    uneven.push_back(core::Rect{ 130.0f, 10.0f, 160.0f, 24.0f });
    const std::vector<core::Rect> kept = ocr::MergeRowBoxes(uneven, { 0, 1 }, 2.5f);
    CheckEqual(static_cast<long long>(kept.size()), 2, "uneven heights stay separate");
}

// worker 协议回归：`ReadOcrWorkerOutput` 以前用 `stream >> separator` 收尾，
// `>>` 会跳过制表符并把文本的第一个字读进 separator，于是每一行都少一个字
// （用户截图里「3、…」变成「、…」、「命中)。」变成「中)。」）。
void TestParseOcrLineRecord() {
    ocr::OcrLine line;
    const bool parsed = ocr::ParseOcrLineRecord(
        "19.5\t23.8\t169.46\t23.46\t0.998\t3、原图里有奇怪标注", &line);
    CheckTrue(parsed, "worker record parses");
    CheckTrue(line.text == "3、原图里有奇怪标注", "first character of the line survives parsing");
    CheckNear(line.box.left, 19.5f, 0.01f, "record x");
    CheckNear(line.box.top, 23.8f, 0.01f, "record y");
    CheckNear(line.box.right, 188.96f, 0.01f, "record right = x + w");
    CheckNear(line.box.bottom, 47.26f, 0.01f, "record bottom = y + h");
    CheckNear(line.confidence, 0.998f, 0.001f, "record confidence");

    // 字段不足 / 空文本 / 数值非法都跳过，不崩也不产生空行。
    CheckTrue(!ocr::ParseOcrLineRecord("broken", &line), "short record rejected");
    CheckTrue(!ocr::ParseOcrLineRecord("1\t2\t3\t4\t5\t", &line), "empty text rejected");
    CheckTrue(!ocr::ParseOcrLineRecord("a\t2\t3\t4\t5\ttext", &line), "bad number rejected");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestPrepareDetInput();
    TestExtractTextBoxes();
    TestPrepareRecInput();
    TestDecodeCtc();
    TestBuildResult();
    TestBuildResultMergesRowFragments();
    TestBuildResultSanitize();
    TestBuildResultKeepsTightRowOrder();
    TestGroupTextBoxesIntoLines();
    TestMergeRowBoxes();
    TestParseOcrLineRecord();

    if (failures != 0) {
        std::printf("OCR_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }
    std::printf("OCR_TESTS=OK checks=%d\n", checks);
    return 0;
}
