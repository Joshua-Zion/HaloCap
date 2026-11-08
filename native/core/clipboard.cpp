#include "clipboard.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <regex>
#include <string>

namespace halocap::core::clipboard {

namespace {

bool IsSpace(char character) {
    return character == ' ' || character == '\t' || character == '\r' || character == '\n';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && IsSpace(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && IsSpace(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool IsBlank(std::string_view text) {
    for (const char character : text) {
        if (!IsSpace(character)) {
            return false;
        }
    }
    return true;
}

int HexValue(char character) {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

bool ParseHexColor(std::string_view hex, Color* color) {
    if (hex.size() != 3 && hex.size() != 4 && hex.size() != 6 && hex.size() != 8) {
        return false;
    }
    for (const char character : hex) {
        if (HexValue(character) < 0) {
            return false;
        }
    }

    std::string expanded(hex);
    if (hex.size() == 3 || hex.size() == 4) {
        std::string doubled;
        doubled.reserve(hex.size() * 2);
        for (const char character : hex) {
            doubled.push_back(character);
            doubled.push_back(character);
        }
        expanded = std::move(doubled);
    }

    const auto byteAt = [&](std::size_t index) {
        return static_cast<std::uint8_t>(HexValue(expanded[index]) * 16 +
                                         HexValue(expanded[index + 1]));
    };

    color->r = byteAt(0);
    color->g = byteAt(2);
    color->b = byteAt(4);
    // CSS 的 alpha 在末位；WPF 的 ARGB 顺序不是输入格式。
    color->a = expanded.size() == 8 ? byteAt(6) : 255;
    return true;
}

// 一个分量只允许 `\d+(\.\d*)?` 或 `\.\d+`：不接受符号、指数、NaN/Infinity。
bool ParseChannelToken(std::string_view token, bool* hasDot, double* value) {
    *hasDot = false;
    if (token.empty()) {
        return false;
    }
    std::size_t i = 0;
    bool digitsBefore = false;
    while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) {
        ++i;
        digitsBefore = true;
    }
    if (i < token.size() && token[i] == '.') {
        *hasDot = true;
        ++i;
        while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) {
            ++i;
        }
    }
    if (i != token.size()) {
        return false;
    }
    if (!digitsBefore && !*hasDot) {
        return false;
    }

    const std::string text(token);
    *value = std::strtod(text.c_str(), nullptr);
    return true;
}

std::string_view FindIgnoreCase(std::string_view haystack, std::string_view needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return {};
    }
    const auto lower = [](char character) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    };
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool equal = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (lower(haystack[i + k]) != lower(needle[k])) {
                equal = false;
                break;
            }
        }
        if (equal) {
            return haystack.substr(i, needle.size());
        }
    }
    return {};
}

std::string RegexReplace(std::string_view input, const char* pattern, const char* replacement,
                         bool caseInsensitive = true) {
    const std::regex::flag_type flags =
        std::regex::ECMAScript | (caseInsensitive ? std::regex::icase : std::regex::flag_type{});
    return std::regex_replace(std::string(input), std::regex(pattern, flags), replacement);
}

std::string DecodeEntities(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] != '&') {
            result.push_back(text[i++]);
            continue;
        }
        const std::size_t semicolon = text.find(';', i + 1);
        if (semicolon == std::string_view::npos || semicolon - i > 12) {
            result.push_back(text[i++]);
            continue;
        }

        const std::string_view entity = text.substr(i + 1, semicolon - i - 1);
        bool decoded = true;
        if (entity == "amp") {
            result.push_back('&');
        } else if (entity == "lt") {
            result.push_back('<');
        } else if (entity == "gt") {
            result.push_back('>');
        } else if (entity == "quot") {
            result.push_back('"');
        } else if (entity == "apos") {
            result.push_back('\'');
        } else if (entity == "nbsp") {
            result.push_back(' ');
        } else if (entity.size() > 1 && entity[0] == '#') {
            unsigned long value = 0;
            if (entity[1] == 'x' || entity[1] == 'X') {
                value = std::strtoul(std::string(entity.substr(2)).c_str(), nullptr, 16);
            } else {
                value = std::strtoul(std::string(entity.substr(1)).c_str(), nullptr, 10);
            }
            if (value == 0 || value > 0x10FFFF) {
                decoded = false;
            } else {
                // UTF-8 编码。
                if (value < 0x80) {
                    result.push_back(static_cast<char>(value));
                } else if (value < 0x800) {
                    result.push_back(static_cast<char>(0xC0 | (value >> 6)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                } else if (value < 0x10000) {
                    result.push_back(static_cast<char>(0xE0 | (value >> 12)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                } else {
                    result.push_back(static_cast<char>(0xF0 | (value >> 18)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 12) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | ((value >> 6) & 0x3F)));
                    result.push_back(static_cast<char>(0x80 | (value & 0x3F)));
                }
            }
        } else {
            decoded = false;
        }

        if (decoded) {
            i = semicolon + 1;
        } else {
            result.push_back(text[i++]);
        }
    }
    return result;
}

std::string NormalizeLineEndings(std::string text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
            result.push_back('\n');
            ++i;
        } else {
            result.push_back(text[i]);
        }
    }
    return result;
}

std::string TrimTrailingNuls(std::string text) {
    while (!text.empty() && text.back() == '\0') {
        text.pop_back();
    }
    return text;
}

int FragmentOffset(std::string_view html, std::string_view key) {
    const std::string_view found = FindIgnoreCase(html, key);
    if (found.empty()) {
        return -1;
    }
    std::size_t position = static_cast<std::size_t>(found.data() - html.data()) + key.size();
    while (position < html.size() && (html[position] == ' ' || html[position] == '\t')) {
        ++position;
    }
    int value = 0;
    bool hasDigits = false;
    while (position < html.size() &&
           std::isdigit(static_cast<unsigned char>(html[position]))) {
        value = value * 10 + (html[position] - '0');
        ++position;
        hasDigits = true;
    }
    return hasDigits ? value : -1;
}

}  // namespace

bool ParseColor(std::string_view text, Color* color) {
    if (color == nullptr) {
        return false;
    }
    std::string_view value = Trim(text);
    if (value.empty()) {
        return false;
    }

    if (value.front() == '#') {
        return ParseHexColor(value.substr(1), color);
    }

    // RGB / RGBA 前缀，大小写不敏感；后面可跟冒号与括号。
    if (value.size() >= 3 && (value[0] == 'r' || value[0] == 'R') &&
        (value[1] == 'g' || value[1] == 'G') && (value[2] == 'b' || value[2] == 'B')) {
        value = value.substr(3);
    }

    const auto trimPunctuation = [](std::string_view text) {
        while (!text.empty()) {
            if (text.front() == ' ' || text.front() == '\t' || text.front() == ':') {
                text.remove_prefix(1);
            } else if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                       static_cast<unsigned char>(text[1]) == 0xBC &&
                       static_cast<unsigned char>(text[2]) == 0x9A) {
                text.remove_prefix(3);   // 全角冒号（不碰全角括号）
            } else {
                break;
            }
        }
        return text;
    };
    value = trimPunctuation(value);

    bool hadOpenParen = false;
    if (!value.empty() && value.front() == '(') {
        hadOpenParen = true;
        value.remove_prefix(1);
        value = Trim(value);
    } else if (value.size() >= 3 && static_cast<unsigned char>(value[0]) == 0xEF &&
               static_cast<unsigned char>(value[1]) == 0xBC &&
               static_cast<unsigned char>(value[2]) == 0x88) {
        hadOpenParen = true;
        value.remove_prefix(3);
        value = Trim(value);
    }

    value = Trim(value);
    if (hadOpenParen) {
        // 有左括号就必须有右括号；"RGB(1,2,3" 不能当颜色。
        bool closed = false;
        if (!value.empty() && value.back() == ')') {
            value.remove_suffix(1);
            closed = true;
        } else if (value.size() >= 3 &&
                   static_cast<unsigned char>(value[value.size() - 3]) == 0xEF &&
                   static_cast<unsigned char>(value[value.size() - 2]) == 0xBC &&
                   static_cast<unsigned char>(value[value.size() - 1]) == 0x89) {
            value.remove_suffix(3);
            closed = true;
        }
        if (!closed) {
            return false;
        }
        value = Trim(value);
    } else if (!value.empty() && value.back() == ')') {
        return false;
    }
    value = Trim(value);
    if (value.empty()) {
        return false;
    }

    // 三个分量，逗号（中英文）或空白分隔。
    std::vector<std::string_view> tokens;
    std::size_t position = 0;
    while (position < value.size()) {
        while (position < value.size() && IsSpace(value[position])) {
            ++position;
        }
        if (position >= value.size()) {
            break;
        }
        if (value[position] == ',' || value[position] == static_cast<char>(0xEF)) {
            return false;
        }

        const std::size_t start = position;
        while (position < value.size() && !IsSpace(value[position]) && value[position] != ',' &&
               !(static_cast<unsigned char>(value[position]) == 0xEF)) {
            ++position;
        }
        tokens.push_back(value.substr(start, position - start));

        while (position < value.size() && IsSpace(value[position])) {
            ++position;
        }
        if (position < value.size() && (value[position] == ',' ||
                                        static_cast<unsigned char>(value[position]) == 0xEF)) {
            position += value[position] == ',' ? 1 : 3;
            while (position < value.size() && IsSpace(value[position])) {
                ++position;
            }
            if (position >= value.size()) {
                return false;
            }
        }
    }

    if (tokens.size() != 3) {
        return false;
    }

    bool anyDot = false;
    double channels[3] = {};
    for (std::size_t i = 0; i < 3; ++i) {
        bool hasDot = false;
        if (!ParseChannelToken(tokens[i], &hasDot, &channels[i])) {
            return false;
        }
        anyDot = anyDot || hasDot;
    }

    std::uint8_t bytes[3] = {};
    for (std::size_t i = 0; i < 3; ++i) {
        if (anyDot) {
            if (channels[i] < 0.0 || channels[i] > 1.0) {
                return false;
            }
            const double scaled = channels[i] * 255.0;
            bytes[i] = static_cast<std::uint8_t>(std::round(scaled));
        } else {
            if (channels[i] < 0.0 || channels[i] > 255.0) {
                return false;
            }
            bytes[i] = static_cast<std::uint8_t>(std::lround(channels[i]));
        }
    }

    color->r = bytes[0];
    color->g = bytes[1];
    color->b = bytes[2];
    color->a = 255;
    return true;
}

std::string HtmlToText(std::string_view html) {
    if (html.empty()) {
        return {};
    }

    const std::string_view startMarker = FindIgnoreCase(html, "<!--StartFragment-->");
    const std::string_view endMarker = FindIgnoreCase(html, "<!--EndFragment-->");
    if (!startMarker.empty() && !endMarker.empty() &&
        endMarker.data() > startMarker.data() + startMarker.size()) {
        const std::size_t start =
            static_cast<std::size_t>(startMarker.data() - html.data()) + startMarker.size();
        const std::size_t end = static_cast<std::size_t>(endMarker.data() - html.data());
        html = html.substr(start, end - start);
    } else if (html.size() >= 8 && FindIgnoreCase(html.substr(0, 8), "version:") ==
                                      html.substr(0, 8)) {
        const int start = FragmentOffset(html, "StartFragment:");
        const int end = FragmentOffset(html, "EndFragment:");
        if (start >= 0 && end > start && static_cast<std::size_t>(end) <= html.size()) {
            html = html.substr(static_cast<std::size_t>(start),
                               static_cast<std::size_t>(end - start));
        } else {
            const std::size_t angle = html.find('<');
            html = angle == std::string_view::npos ? std::string_view{} : html.substr(angle);
        }
    }

    std::string text(html);
    text = RegexReplace(text, "<(script|style|head|iframe|object)\\b[^>]*>[\\s\\S]*?</\\1\\s*>", "");
    text = RegexReplace(text, "<!--[\\s\\S]*?-->", "");
    text = RegexReplace(text,
                        "<br\\b[^>]*>|</(?:p|div|li|h[1-6]|tr|pre|blockquote|section)\\s*>",
                        "\n");
    text = RegexReplace(text, "</t[dh]\\s*>", "\t");
    text = RegexReplace(text, "<[^>]*>", "");
    text = DecodeEntities(text);
    text = NormalizeLineEndings(std::move(text));

    // trim
    std::string_view view = Trim(text);
    return std::string(view);
}

bool Snapshot::HasContent() const {
    return hasImage || !filePaths.empty() || !IsBlank(text);
}

Snapshot NormalizeSnapshot(const SnapshotInput& input) {
    Snapshot snapshot;
    snapshot.sequence = input.sequence;
    snapshot.hasImage = input.hasImage;

    for (const std::string& path : input.filePaths) {
        if (!IsBlank(path)) {
            snapshot.filePaths.push_back(path);
        }
    }
    if (!snapshot.filePaths.empty()) {
        // 文件优先于位图与文本（1.x 的行为）。
        snapshot.hasImage = false;
        return snapshot;
    }

    if (input.hasImage) {
        return snapshot;
    }

    std::string text;
    if (input.hasHtml) {
        text = HtmlToText(input.html);
        if (!IsBlank(text)) {
            snapshot.text = std::move(text);
            return snapshot;
        }
    }
    if (input.hasText) {
        snapshot.text = TrimTrailingNuls(input.text);
    }
    return snapshot;
}

bool LooksLikeLocalAbsolutePath(std::string_view utf8Path) {
    if (utf8Path.size() < 3) {
        return false;
    }
    if (!std::isalpha(static_cast<unsigned char>(utf8Path[0])) || utf8Path[1] != ':') {
        return false;
    }
    if (utf8Path[2] != '\\' && utf8Path[2] != '/') {
        return false;
    }
    // UNC 与设备路径不是「本机绝对路径」。
    if (utf8Path.size() >= 2 && utf8Path[0] == '\\' && utf8Path[1] == '\\') {
        return false;
    }
    return true;
}

std::vector<PinPlan> PinPlanner::PlanFiles(const Snapshot& snapshot, const PinOptions& options,
                                           const std::function<bool(const std::string&)>& loadImage) const {
    std::vector<PinPlan> pins;
    if (snapshot.filePaths.empty()) {
        return pins;
    }

    const bool convert =
        options.convertFilePathsToImages && !FileImagePasteConsumed(snapshot.sequence);
    std::vector<std::string> fallback;
    int imageCount = 0;

    for (const std::string& path : snapshot.filePaths) {
        const bool eligible = convert && imageCount < options.maxFileImagePins &&
                              LooksLikeLocalAbsolutePath(path) && loadImage(path);
        if (eligible) {
            pins.push_back(PinPlan{ PinKind::FileImage, path, true, snapshot.sequence });
            ++imageCount;
        } else {
            fallback.push_back(path);
        }
    }

    if (!fallback.empty()) {
        std::string joined;
        for (std::size_t i = 0; i < fallback.size(); ++i) {
            joined += fallback[i];
            if (i + 1 < fallback.size()) {
                joined += "\r\n";
            }
        }
        pins.push_back(PinPlan{ PinKind::FilePath, std::move(joined), true, snapshot.sequence });
    }

    return pins;
}

std::vector<PinPlan> PinPlanner::PlanContent(const Snapshot& snapshot) const {
    std::vector<PinPlan> pins;
    if (snapshot.hasImage) {
        pins.push_back(PinPlan{ PinKind::Image, {}, true, snapshot.sequence });
        return pins;
    }
    if (IsBlank(snapshot.text)) {
        return pins;
    }

    Color color;
    if (ParseColor(snapshot.text, &color)) {
        pins.push_back(PinPlan{ PinKind::Color, snapshot.text, false, snapshot.sequence });
    } else {
        pins.push_back(PinPlan{ PinKind::Text, snapshot.text, false, snapshot.sequence });
    }
    return pins;
}

void PinPlanner::MarkPasted(const PinPlan& plan) {
    if (plan.kind == PinKind::FileImage) {
        pastedSequence_ = plan.sequence;
        hasPastedSequence_ = true;
    }
}

bool PinPlanner::FileImagePasteConsumed(std::uint32_t sequence) const {
    return hasPastedSequence_ && pastedSequence_ == sequence;
}

TextCardMetrics TextCardBounds(std::string_view text, double scale) {
    TextCardMetrics metrics;
    if (scale <= 0.0) {
        scale = 1.0;
    }

    int lines = 1;
    int lineLength = 0;
    int longestLine = 0;
    for (const char character : text) {
        if (character == '\n') {
            longestLine = std::max(longestLine, lineLength);
            lineLength = 0;
            ++lines;
        } else {
            ++lineLength;
        }
    }
    longestLine = std::max(longestLine, lineLength);

    const int width = std::clamp(32 + longestLine * 9, 160, 672);
    const int height = std::clamp(32 + lines * 24, 56, 2832);
    metrics.width = static_cast<int>(std::lround(width * scale));
    metrics.height = static_cast<int>(std::lround(height * scale));
    return metrics;
}

}
