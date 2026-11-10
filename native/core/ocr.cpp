#include "ocr.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <numeric>

namespace halocap::core::ocr {

namespace {

struct Bgr {
    float b = 0.0f;
    float g = 0.0f;
    float r = 0.0f;
};

Bgr SampleBilinear(const OcrImage& image, float x, float y) {
    const float clampedX = std::clamp(x, 0.0f, static_cast<float>(image.width - 1));
    const float clampedY = std::clamp(y, 0.0f, static_cast<float>(image.height - 1));

    const int x0 = static_cast<int>(clampedX);
    const int y0 = static_cast<int>(clampedY);
    const int x1 = std::min(x0 + 1, image.width - 1);
    const int y1 = std::min(y0 + 1, image.height - 1);
    const float fx = clampedX - static_cast<float>(x0);
    const float fy = clampedY - static_cast<float>(y0);

    const auto* bits = static_cast<const std::uint8_t*>(image.bgra);
    const auto pixel = [&](int px, int py) {
        const std::uint8_t* p = bits + static_cast<std::size_t>(py) * image.stride +
                               static_cast<std::size_t>(px) * 4;
        return Bgr{ static_cast<float>(p[0]) / 255.0f, static_cast<float>(p[1]) / 255.0f,
                    static_cast<float>(p[2]) / 255.0f };
    };

    const Bgr p00 = pixel(x0, y0);
    const Bgr p10 = pixel(x1, y0);
    const Bgr p01 = pixel(x0, y1);
    const Bgr p11 = pixel(x1, y1);

    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    return Bgr{ lerp(lerp(p00.b, p10.b, fx), lerp(p01.b, p11.b, fx), fy),
                lerp(lerp(p00.g, p10.g, fx), lerp(p01.g, p11.g, fx), fy),
                lerp(lerp(p00.r, p10.r, fx), lerp(p01.r, p11.r, fx), fy) };
}

int RoundUpTo(int value, int multiple) {
    return (value + multiple - 1) / multiple * multiple;
}

// 识别结果里常见的「假字符」：替换符、控制符、几何符号（◆ 之类）、变体选择符、
// 零宽字符。它们多半来自图标/分隔线被当成字，留着只会让复制出来的文本变脏。
bool IsJunkCodePoint(std::uint32_t code) {
    return code == 0xFFFD || code == 0x7F || code < 0x20 || code == 0x200B ||
           code == 0x200C || code == 0x200D || code == 0x200E || code == 0x200F ||
           (code >= 0x25A0 && code <= 0x25FF) || (code >= 0x2B00 && code <= 0x2BFF) ||
           (code >= 0xFE00 && code <= 0xFE0F) || (code >= 0xE000 && code <= 0xF8FF);
}

std::string SanitizeLine(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        std::uint32_t code = lead;
        std::size_t length = 1;
        if ((lead & 0x80) == 0) {
            code = lead;
            length = 1;
        } else if ((lead & 0xE0) == 0xC0) {
            code = lead & 0x1F;
            length = 2;
        } else if ((lead & 0xF0) == 0xE0) {
            code = lead & 0x0F;
            length = 3;
        } else if ((lead & 0xF8) == 0xF0) {
            code = lead & 0x07;
            length = 4;
        } else {
            ++index;   // 非法首字节，丢掉
            continue;
        }
        if (index + length > text.size()) {
            break;
        }
        bool valid = true;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            code = (code << 6) | (continuation & 0x3F);
        }
        if (!valid || IsJunkCodePoint(code)) {
            index += length;
            continue;
        }
        out.append(text.substr(index, length));
        index += length;
    }

    const auto isSpace = [](char c) { return c == ' ' || c == '\t'; };
    while (!out.empty() && isSpace(out.back())) {
        out.pop_back();
    }
    std::size_t start = 0;
    while (start < out.size() && isSpace(out[start])) {
        ++start;
    }
    return out.substr(start);
}

}  // namespace

DetInput PrepareDetInput(const OcrImage& image, int targetSide) {
    DetInput input;
    if (image.bgra == nullptr || image.width <= 0 || image.height <= 0 || image.stride <= 0) {
        return input;
    }

    const int longest = std::max(image.width, image.height);
    const float ratio =
        targetSide > 0 && longest > targetSide
            ? static_cast<float>(targetSide) / static_cast<float>(longest)
            : 1.0f;

    const int resizedWidth = std::max(1, static_cast<int>(std::lround(image.width * ratio)));
    const int resizedHeight = std::max(1, static_cast<int>(std::lround(image.height * ratio)));

    input.width = RoundUpTo(resizedWidth, 32);
    input.height = RoundUpTo(resizedHeight, 32);
    input.scaleX = static_cast<float>(resizedWidth) / static_cast<float>(image.width);
    input.scaleY = static_cast<float>(resizedHeight) / static_cast<float>(image.height);
    input.padX = 0;
    input.padY = 0;

    const std::size_t plane =
        static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
    input.data.assign(plane * 3, 0.0f);

    for (int y = 0; y < resizedHeight; ++y) {
        const float sourceY = (static_cast<float>(y) + 0.5f) / input.scaleY - 0.5f;
        for (int x = 0; x < resizedWidth; ++x) {
            const float sourceX = (static_cast<float>(x) + 0.5f) / input.scaleX - 0.5f;
            const Bgr bgr = SampleBilinear(image, sourceX, sourceY);
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(input.width) +
                static_cast<std::size_t>(x);
            input.data[index] = (bgr.b - 0.485f) / 0.229f;
            input.data[plane + index] = (bgr.g - 0.456f) / 0.224f;
            input.data[plane * 2 + index] = (bgr.r - 0.406f) / 0.225f;
        }
    }

    return input;
}

std::vector<Rect> ExtractTextBoxes(const float* probability, int width, int height,
                                   const DetInput& input, const OcrOptions& options) {
    std::vector<Rect> boxes;
    if (probability == nullptr || width <= 0 || height <= 0) {
        return boxes;
    }

    const std::size_t total = static_cast<std::size_t>(width) * height;
    std::vector<std::uint8_t> visited(total, 0);
    std::vector<int> queue;
    queue.reserve(1024);

    for (int startY = 0; startY < height; ++startY) {
        for (int startX = 0; startX < width; ++startX) {
            const std::size_t start = static_cast<std::size_t>(startY) * width + startX;
            if (visited[start] != 0 || probability[start] < options.detThreshold) {
                continue;
            }

            visited[start] = 1;
            queue.clear();
            queue.push_back(static_cast<int>(start));

            int minX = startX;
            int maxX = startX;
            int minY = startY;
            int maxY = startY;
            double sum = 0.0;
            int count = 0;

            for (std::size_t head = 0; head < queue.size(); ++head) {
                const int index = queue[head];
                const int x = index % width;
                const int y = index / width;
                sum += probability[index];
                ++count;
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
                minY = std::min(minY, y);
                maxY = std::max(maxY, y);

                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const std::size_t neighbour = static_cast<std::size_t>(ny) * width + nx;
                        if (visited[neighbour] != 0 || probability[neighbour] < options.detThreshold) {
                            continue;
                        }
                        visited[neighbour] = 1;
                        queue.push_back(static_cast<int>(neighbour));
                    }
                }
            }

            const int boxWidth = maxX - minX + 1;
            const int boxHeight = maxY - minY + 1;
            if (boxWidth < 3 || boxHeight < 3 || count == 0) {
                continue;
            }

            const double score = sum / count;
            if (score < options.boxThreshold) {
                continue;
            }

            // unclip：按面积/周长给出一个距离，把框外扩（1.x 用 pyclipper，这里用轴对齐近似）。
            const double distance =
                static_cast<double>(boxWidth) * boxHeight * options.unclipRatio /
                (2.0 * (boxWidth + boxHeight));
            const float expand = static_cast<float>(distance);

            Rect box{ static_cast<float>(minX), static_cast<float>(minY),
                      static_cast<float>(maxX + 1), static_cast<float>(maxY + 1) };
            box.left -= expand;
            box.top -= expand;
            box.right += expand;
            box.bottom += expand;

            // 网络坐标 → 原图坐标（右侧/底部的 padding 不需要减）。
            box.left = (box.left - input.padX) / input.scaleX;
            box.right = (box.right - input.padX) / input.scaleX;
            box.top = (box.top - input.padY) / input.scaleY;
            box.bottom = (box.bottom - input.padY) / input.scaleY;

            box.left = std::max(0.0f, box.left);
            box.top = std::max(0.0f, box.top);
            if (box.Width() < options.minBoxWidth || box.Height() < options.minBoxHeight) {
                continue;
            }
            boxes.push_back(box);
        }
    }

    std::sort(boxes.begin(), boxes.end(), [](const Rect& a, const Rect& b) {
        if (std::fabs(a.top - b.top) > 4.0f) {
            return a.top < b.top;
        }
        return a.left < b.left;
    });
    return boxes;
}

RecInput PrepareRecInput(const OcrImage& image, Rect box, int recHeight) {
    RecInput input;
    if (image.bgra == nullptr || image.width <= 0 || image.height <= 0 || recHeight <= 0) {
        return input;
    }

    const Rect normalized = box.Normalized();
    // 1.x 的 RapidOcr 用 Padding=6：识别裁图四周留 6 px，避免把上下伸部/边缘笔画裁掉。
    constexpr int kRecPadding = 6;
    const int left = std::clamp(static_cast<int>(std::floor(normalized.left)) - kRecPadding, 0,
                                image.width - 1);
    const int top = std::clamp(static_cast<int>(std::floor(normalized.top)) - kRecPadding, 0,
                               image.height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(normalized.right)) + kRecPadding,
                                 left + 1, image.width);
    const int bottom = std::clamp(static_cast<int>(std::ceil(normalized.bottom)) + kRecPadding,
                                  top + 1, image.height);

    const int cropWidth = right - left;
    const int cropHeight = bottom - top;
    if (cropWidth <= 0 || cropHeight <= 0) {
        return input;
    }

    const float ratio = static_cast<float>(recHeight) / static_cast<float>(cropHeight);
    input.height = recHeight;
    input.width = std::max(1, static_cast<int>(std::lround(cropWidth * ratio)));

    const std::size_t plane =
        static_cast<std::size_t>(input.width) * static_cast<std::size_t>(input.height);
    input.data.assign(plane * 3, 0.0f);

    for (int y = 0; y < input.height; ++y) {
        const float sourceY = top + (static_cast<float>(y) + 0.5f) / ratio - 0.5f;
        for (int x = 0; x < input.width; ++x) {
            const float sourceX = left + (static_cast<float>(x) + 0.5f) / ratio - 0.5f;
            const Bgr bgr = SampleBilinear(image, sourceX, sourceY);
            const std::size_t index =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(input.width) +
                static_cast<std::size_t>(x);
            input.data[index] = (bgr.b - 0.5f) / 0.5f;
            input.data[plane + index] = (bgr.g - 0.5f) / 0.5f;
            input.data[plane * 2 + index] = (bgr.r - 0.5f) / 0.5f;
        }
    }

    return input;
}

std::string DecodeCtc(const float* logits, int timeSteps, int classes,
                      const std::vector<std::string>& dictionary, float* confidence) {
    if (confidence != nullptr) {
        *confidence = 0.0f;
    }
    if (logits == nullptr || timeSteps <= 0 || classes <= 1) {
        return {};
    }

    std::string text;
    double confidenceSum = 0.0;
    int confidenceCount = 0;
    int previous = -1;

    for (int t = 0; t < timeSteps; ++t) {
        const float* row = logits + static_cast<std::size_t>(t) * classes;
        int best = 0;
        float bestLogit = row[0];
        for (int c = 1; c < classes; ++c) {
            if (row[c] > bestLogit) {
                bestLogit = row[c];
                best = c;
            }
        }

        if (best == 0) {
            previous = best;
            continue;
        }
        if (best == previous) {
            continue;
        }
        previous = best;

        // 有些导出的 rec 已经在图里做了 softmax（值域 [0,1]、行和≈1），有些是 logits。
        // 先看行和：像概率就直接取最大值，否则现算 softmax，免得把 0.99 算成 1/6906。
        double rowSum = 0.0;
        for (int c = 0; c < classes; ++c) {
            rowSum += row[c];
        }
        float probability = 0.0f;
        if (bestLogit <= 1.001f && rowSum > 0.5 && rowSum < 1.5) {
            probability = bestLogit;
        } else {
            double denominator = 0.0;
            for (int c = 0; c < classes; ++c) {
                denominator += std::exp(static_cast<double>(row[c] - bestLogit));
            }
            probability = static_cast<float>(1.0 / denominator);
        }
        confidenceSum += probability;
        ++confidenceCount;

        const std::size_t dictionaryIndex = static_cast<std::size_t>(best - 1);
        if (dictionaryIndex < dictionary.size()) {
            text += dictionary[dictionaryIndex];
        }
    }

    // 取输出字符的平均概率，和 1.x RapidOcr 的 TextScore（块内字符分均值）同语义。
    // 用最低分会因为一个难字把整行丢掉，长行尤其吃亏。
    if (confidence != nullptr) {
        *confidence = confidenceCount > 0
                          ? static_cast<float>(confidenceSum / confidenceCount)
                          : 0.0f;
    }
    return text;
}

std::vector<std::vector<std::size_t>> GroupTextBoxesIntoLines(const std::vector<Rect>& boxes,
                                                              float lineTolerance) {
    std::vector<std::vector<std::size_t>> rows;
    if (boxes.empty()) {
        return rows;
    }

    const auto centerY = [&boxes](std::size_t index) {
        return (boxes[index].top + boxes[index].bottom) * 0.5f;
    };
    const auto height = [&boxes](std::size_t index) {
        return std::max(1.0f, boxes[index].Height());
    };

    std::vector<std::size_t> order(boxes.size());
    std::iota(order.begin(), order.end(), std::size_t{ 0 });
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        if (centerY(a) != centerY(b)) {
            return centerY(a) < centerY(b);
        }
        if (boxes[a].left != boxes[b].left) {
            return boxes[a].left < boxes[b].left;
        }
        return a < b;
    });

    std::size_t start = 0;
    while (start < order.size()) {
        const float rowCenter = centerY(order[start]);
        float rowHeight = height(order[start]);
        std::size_t end = start + 1;
        while (end < order.size()) {
            // 同一行的碎片的中心几乎重合；相邻两行的中心至少差半个行高。
            // 用较矮框的一半做阈值：行里混进一个被 det 撑高的大框时，
            // 不会因此把下一行也吸进来。
            const float tolerance =
                std::max(lineTolerance, 0.5f * std::min(rowHeight, height(order[end])));
            if (centerY(order[end]) - rowCenter > tolerance) {
                break;
            }
            rowHeight = std::max(rowHeight, height(order[end]));
            ++end;
        }

        std::vector<std::size_t> row(order.begin() + static_cast<std::ptrdiff_t>(start),
                                     order.begin() + static_cast<std::ptrdiff_t>(end));
        std::sort(row.begin(), row.end(), [&](std::size_t a, std::size_t b) {
            if (boxes[a].left != boxes[b].left) {
                return boxes[a].left < boxes[b].left;
            }
            return boxes[a].top < boxes[b].top;
        });
        rows.push_back(std::move(row));
        start = end;
    }
    return rows;
}

std::vector<Rect> MergeRowBoxes(const std::vector<Rect>& boxes,
                                const std::vector<std::size_t>& row, float gapRatio) {
    std::vector<Rect> merged;
    merged.reserve(row.size());
    for (const std::size_t index : row) {
        const Rect& piece = boxes[index];
        if (!merged.empty()) {
            Rect& last = merged.back();
            const float lastHeight = std::max(1.0f, last.Height());
            const float pieceHeight = std::max(1.0f, piece.Height());
            const float shorter = std::min(lastHeight, pieceHeight);
            const float taller = std::max(lastHeight, pieceHeight);
            const float centerDelta =
                std::fabs((last.top + last.bottom) - (piece.top + piece.bottom)) * 0.5f;
            const float gap = piece.left - last.right;
            if (gap <= shorter * gapRatio && shorter >= taller * 0.6f &&
                centerDelta <= shorter * 0.35f) {
                last.left = std::min(last.left, piece.left);
                last.top = std::min(last.top, piece.top);
                last.right = std::max(last.right, piece.right);
                last.bottom = std::max(last.bottom, piece.bottom);
                continue;
            }
        }
        merged.push_back(piece);
    }
    return merged;
}

OcrResult BuildResult(std::vector<OcrLine> lines, float lineTolerance) {
    OcrResult result;

    // 先按 1.x 的做法逐行清一遍：去首尾空白 + 丢掉图标/分隔线识别出来的假字符。
    // 过滤后为空的整行直接扔掉，避免结果里出现只有 ◆ 的「空行」。
    {
        std::vector<OcrLine> kept;
        kept.reserve(lines.size());
        for (OcrLine& line : lines) {
            line.box = line.box.Normalized();
            line.text = SanitizeLine(line.text);
            if (line.text.empty()) {
                continue;
            }
            kept.push_back(std::move(line));
        }
        lines = std::move(kept);
    }

    std::vector<Rect> boxes;
    boxes.reserve(lines.size());
    for (const OcrLine& line : lines) {
        boxes.push_back(line.box);
    }

    std::vector<OcrLine> ordered;
    ordered.reserve(lines.size());
    for (const std::vector<std::size_t>& row : GroupTextBoxesIntoLines(boxes, lineTolerance)) {
        // 1.x（RapidOcr）一个文本框就是一整行；我们的连通域检测常把同一行按词拆开。
        // 这里把同一行里水平间距小于 0.6 倍行高的碎片拼回一行，拉丁词之间补一个空格。
        std::vector<const OcrLine*> pieces;
        pieces.reserve(row.size());
        for (const std::size_t index : row) {
            pieces.push_back(&lines[index]);
        }

        std::vector<OcrLine> merged;
        merged.reserve(pieces.size());
        for (const OcrLine* piecePtr : pieces) {
            const OcrLine& piece = *piecePtr;
            if (!merged.empty()) {
                OcrLine& last = merged.back();
                const float gap = piece.box.left - last.box.right;
                const float firstHeight = last.box.Height();
                const float secondHeight = piece.box.Height();
                const float shorter = std::min(firstHeight, secondHeight);
                const float taller = std::max(firstHeight, secondHeight);
                if (gap <= std::max(1.0f, shorter) * 0.6f && shorter >= taller * 0.5f) {
                    const char tail = last.text.empty() ? '\0' : last.text.back();
                    const char head = piece.text.empty() ? '\0' : piece.text.front();
                    const auto isWord = [](char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9');
                    };
                    if (isWord(tail) && isWord(head) && gap > shorter * 0.15f) {
                        last.text += " ";
                    }
                    last.text += piece.text;
                    last.box.left = std::min(last.box.left, piece.box.left);
                    last.box.top = std::min(last.box.top, piece.box.top);
                    last.box.right = std::max(last.box.right, piece.box.right);
                    last.box.bottom = std::max(last.box.bottom, piece.box.bottom);
                    last.confidence = std::min(last.confidence, piece.confidence);
                    continue;
                }
            }
            merged.push_back(piece);
        }
        ordered.insert(ordered.end(), merged.begin(), merged.end());
    }

    result.lines = std::move(ordered);
    result.plainText = ToPlainText(result);
    return result;
}

std::string ToPlainText(const OcrResult& result) {
    std::string text;
    for (std::size_t i = 0; i < result.lines.size(); ++i) {
        text += result.lines[i].text;
        if (i + 1 < result.lines.size()) {
            // Win32 的 EDIT 只把 CRLF 当换行；用单纯的 '\n' 会把所有行并成一段。
            text += "\r\n";
        }
    }
    return text;
}

bool ParseOcrLineRecord(std::string_view record, OcrLine* line) {
    if (line == nullptr) {
        return false;
    }
    float values[5] = {};
    std::size_t cursor = 0;
    for (int field = 0; field < 5; ++field) {
        const std::size_t tab = record.find('\t', cursor);
        if (tab == std::string_view::npos) {
            return false;
        }
        const std::string_view value = record.substr(cursor, tab - cursor);
        const auto parsed =
            std::from_chars(value.data(), value.data() + value.size(), values[field]);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return false;
        }
        cursor = tab + 1;
    }

    std::string_view text = record.substr(cursor);
    if (!text.empty() && text.back() == '\r') {
        text.remove_suffix(1);
    }
    if (text.empty()) {
        return false;
    }
    line->text.assign(text);
    line->box = Rect{ values[0], values[1], values[0] + values[2], values[1] + values[3] };
    line->confidence = values[4];
    return true;
}

std::vector<std::size_t> FindMatchingLines(const OcrResult& result, std::string_view query) {
    std::vector<std::size_t> matches;
    if (query.empty()) {
        return matches;
    }

    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };

    for (std::size_t i = 0; i < result.lines.size(); ++i) {
        const std::string& text = result.lines[i].text;
        if (text.size() < query.size()) {
            continue;
        }
        bool found = false;
        for (std::size_t start = 0; start + query.size() <= text.size() && !found; ++start) {
            bool equal = true;
            for (std::size_t k = 0; k < query.size(); ++k) {
                if (lower(text[start + k]) != lower(query[k])) {
                    equal = false;
                    break;
                }
            }
            found = equal;
        }
        if (found) {
            matches.push_back(i);
        }
    }
    return matches;
}

}
