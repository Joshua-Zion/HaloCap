#include "update.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <vector>

namespace halocap::core {

namespace {

struct Scanner {
    std::string_view text;
    std::size_t position = 0;

    void SkipWhitespace() {
        while (position < text.size() &&
               (text[position] == ' ' || text[position] == '\t' || text[position] == '\r' ||
                text[position] == '\n')) {
            ++position;
        }
    }

    bool Consume(char expected) {
        SkipWhitespace();
        if (position >= text.size() || text[position] != expected) {
            return false;
        }
        ++position;
        return true;
    }

    bool Peek(char expected) {
        SkipWhitespace();
        return position < text.size() && text[position] == expected;
    }

    bool ReadString(std::string* out, std::string* error) {
        SkipWhitespace();
        if (position >= text.size() || text[position] != '"') {
            *error = "expected a string";
            return false;
        }
        ++position;
        out->clear();
        while (position < text.size()) {
            const char character = text[position++];
            if (character == '"') {
                return true;
            }
            if (character == '\\') {
                if (position >= text.size()) {
                    *error = "truncated escape";
                    return false;
                }
                const char escaped = text[position++];
                switch (escaped) {
                case '"': out->push_back('"'); break;
                case '\\': out->push_back('\\'); break;
                case '/': out->push_back('/'); break;
                case 'b': out->push_back('\b'); break;
                case 'f': out->push_back('\f'); break;
                case 'n': out->push_back('\n'); break;
                case 'r': out->push_back('\r'); break;
                case 't': out->push_back('\t'); break;
                default:
                    *error = "unsupported escape";
                    return false;
                }
                continue;
            }
            out->push_back(character);
        }
        *error = "unterminated string";
        return false;
    }

    bool ReadNumber(std::int64_t* out, std::string* error) {
        SkipWhitespace();
        const std::size_t start = position;
        while (position < text.size() &&
               (std::isdigit(static_cast<unsigned char>(text[position])) ||
                text[position] == '-' || text[position] == '+' || text[position] == '.' ||
                text[position] == 'e' || text[position] == 'E')) {
            ++position;
        }
        if (position == start) {
            *error = "expected a number";
            return false;
        }
        *out = std::strtoll(std::string(text.substr(start, position - start)).c_str(), nullptr, 10);
        return true;
    }
};

std::vector<std::string_view> SplitVersion(std::string_view version) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= version.size(); ++i) {
        if (i == version.size() || version[i] == '.') {
            parts.push_back(version.substr(start, i - start));
            start = i + 1;
        }
    }
    return parts;
}

}  // namespace

bool ParseUpdateManifest(std::string_view json, UpdateManifest* manifest, std::string* error) {
    std::string localError;
    if (error == nullptr) {
        error = &localError;
    }
    if (manifest == nullptr) {
        *error = "manifest is null";
        return false;
    }

    Scanner scanner{ json };
    if (!scanner.Consume('{')) {
        *error = "expected an object";
        return false;
    }

    bool hasVersion = false;
    bool hasUrl = false;

    while (true) {
        scanner.SkipWhitespace();
        if (scanner.Peek('}')) {
            scanner.Consume('}');
            break;
        }
        if (scanner.position >= json.size()) {
            *error = "unterminated object";
            return false;
        }

        std::string key;
        if (!scanner.ReadString(&key, error)) {
            return false;
        }
        if (!scanner.Consume(':')) {
            *error = "expected a colon";
            return false;
        }

        scanner.SkipWhitespace();
        if (scanner.position < json.size() && json[scanner.position] == '"') {
            std::string value;
            if (!scanner.ReadString(&value, error)) {
                return false;
            }
            if (key == "version") {
                manifest->version = value;
                hasVersion = true;
            } else if (key == "url") {
                manifest->url = value;
                hasUrl = true;
            } else if (key == "sha256") {
                manifest->sha256 = value;
            } else if (key == "notes") {
                manifest->notes = value;
            }
        } else if (scanner.position < json.size() &&
                   (std::isdigit(static_cast<unsigned char>(json[scanner.position])) ||
                    json[scanner.position] == '-')) {
            std::int64_t number = 0;
            if (!scanner.ReadNumber(&number, error)) {
                return false;
            }
            if (key == "size") {
                manifest->size = number;
            }
        } else {
            // true / false / null：跳过，未知字段不参与校验。
            while (scanner.position < json.size() && json[scanner.position] != ',' &&
                   json[scanner.position] != '}') {
                ++scanner.position;
            }
        }

        scanner.SkipWhitespace();
        if (scanner.Peek(',')) {
            scanner.Consume(',');
            continue;
        }
        if (scanner.Peek('}')) {
            scanner.Consume('}');
            break;
        }
        *error = "expected a comma or the end of the object";
        return false;
    }

    if (!hasVersion) {
        *error = "manifest has no version";
        return false;
    }
    if (!hasUrl) {
        *error = "manifest has no url";
        return false;
    }
    return true;
}

int CompareVersions(std::string_view left, std::string_view right) {
    const auto split = [](std::string_view version, std::string_view* corePart,
                          std::string_view* suffix) {
        const std::size_t dash = version.find('-');
        if (dash == std::string_view::npos) {
            *corePart = version;
            *suffix = {};
        } else {
            *corePart = version.substr(0, dash);
            *suffix = version.substr(dash + 1);
        }
    };

    std::string_view leftCore;
    std::string_view leftSuffix;
    std::string_view rightCore;
    std::string_view rightSuffix;
    split(left, &leftCore, &leftSuffix);
    split(right, &rightCore, &rightSuffix);

    const std::vector<std::string_view> leftParts = SplitVersion(leftCore);
    const std::vector<std::string_view> rightParts = SplitVersion(rightCore);
    const std::size_t count = std::max(leftParts.size(), rightParts.size());

    for (std::size_t i = 0; i < count; ++i) {
        const int leftValue =
            i < leftParts.size() ? std::atoi(std::string(leftParts[i]).c_str()) : 0;
        const int rightValue =
            i < rightParts.size() ? std::atoi(std::string(rightParts[i]).c_str()) : 0;
        if (leftValue != rightValue) {
            return leftValue < rightValue ? -1 : 1;
        }
    }

    // 核心段相同：有后缀的是预发布版，比正式版旧。
    if (leftSuffix.empty() && rightSuffix.empty()) {
        return 0;
    }
    if (leftSuffix.empty()) {
        return 1;
    }
    if (rightSuffix.empty()) {
        return -1;
    }
    if (leftSuffix == rightSuffix) {
        return 0;
    }
    return leftSuffix < rightSuffix ? -1 : 1;
}

bool IsNewerVersion(std::string_view candidate, std::string_view current) {
    return CompareVersions(candidate, current) > 0;
}

bool ParseLatestTagFromAtom(std::string_view xml, std::string* tag, std::string* error) {
    std::string localError;
    if (error == nullptr) {
        error = &localError;
    }
    if (tag == nullptr) {
        *error = "tag is null";
        return false;
    }

    // 第一个 <entry> 是最新一次发布；它的备用链接形如
    //   <link rel="alternate" ... href="https://github.com/<owner>/<repo>/releases/tag/v2.1.0"/>
    // 取 href 里的 tag 部分，比解析 <title>（发布名可能带前缀）可靠。
    const std::size_t entry = xml.find("<entry");
    if (entry == std::string_view::npos) {
        *error = "feed has no entry";
        return false;
    }
    constexpr std::string_view kMarker = "/releases/tag/";
    const std::size_t marker = xml.find(kMarker, entry);
    if (marker == std::string_view::npos) {
        *error = "entry has no tag link";
        return false;
    }

    std::size_t start = marker + kMarker.size();
    std::size_t end = start;
    while (end < xml.size()) {
        const char character = xml[end];
        if (character == '"' || character == '<' || character == ' ' || character == '\r' ||
            character == '\n' || character == '\t' || character == '/') {
            break;
        }
        ++end;
    }
    if (end <= start) {
        *error = "tag is empty";
        return false;
    }

    std::string value(xml.substr(start, end - start));
    if (!value.empty() && (value.front() == 'v' || value.front() == 'V')) {
        value.erase(value.begin());
    }
    if (value.empty() || !std::isdigit(static_cast<unsigned char>(value.front()))) {
        *error = "tag is not a version";
        return false;
    }
    *tag = std::move(value);
    return true;
}

}
