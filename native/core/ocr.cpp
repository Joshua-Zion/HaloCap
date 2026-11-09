#include "ocr.h"

#include <algorithm>
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
    const int left = std::clamp(static_cast<int>(std::floor(normalized.left)), 0, image.width - 1);
    const int top = std::clamp(static_cast<int>(std::floor(normalized.top)), 0, image.height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(normalized.right)), left + 1, image.width);
    const int bottom =
        std::clamp(static_cast<int>(std::ceil(normalized.bottom)), top + 1, image.height);

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
    float minimum = 1.0f;
    bool hasCharacter = false;
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
        minimum = hasCharacter ? std::min(minimum, probability) : probability;
        hasCharacter = true;

        const std::size_t dictionaryIndex = static_cast<std::size_t>(best - 1);
        if (dictionaryIndex < dictionary.size()) {
            text += dictionary[dictionaryIndex];
        }
    }

    if (confidence != nullptr) {
        *confidence = hasCharacter ? minimum : 0.0f;
    }
    return text;
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

    std::sort(lines.begin(), lines.end(), [](const OcrLine& a, const OcrLine& b) {
        return a.box.top < b.box.top;
    });

    std::vector<OcrLine> ordered;
    ordered.reserve(lines.size());

    std::size_t startIndex = 0;
    while (startIndex < lines.size()) {
        // 同一行 = 垂直重叠超过较矮那个框的一半。用重叠而不是中心距，
        // 是因为同一行里字号可能差一倍（标题 + 正文），中心距会把它们拆开。
        const Rect rowBox = lines[startIndex].box;
        std::size_t end = startIndex + 1;
        while (end < lines.size()) {
            const Rect other = lines[end].box;
            const float overlap = std::min(rowBox.bottom, other.bottom) -
                                  std::max(rowBox.top, other.top);
            const float smaller = std::max(1.0f, std::min(rowBox.Height(), other.Height()));
            if (overlap < smaller * 0.5f && overlap < lineTolerance) {
                break;
            }
            ++end;
        }

        std::vector<OcrLine> row(lines.begin() + static_cast<std::ptrdiff_t>(startIndex),
                                 lines.begin() + static_cast<std::ptrdiff_t>(end));
        std::sort(row.begin(), row.end(), [](const OcrLine& a, const OcrLine& b) {
            return a.box.left < b.box.left;
        });
        ordered.insert(ordered.end(), row.begin(), row.end());

        startIndex = end;
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
            text.push_back('\n');
        }
    }
    return text;
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
