#include "settings.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace halocap::core {

namespace {

std::string Lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return out;
}

std::string_view TrimSpaces(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

std::uint32_t HexValue(char c) {
    if (c >= '0' && c <= '9') return static_cast<std::uint32_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<std::uint32_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<std::uint32_t>(c - 'A' + 10);
    return 0xFFFFFFFFu;
}

void AppendUtf8(std::string& out, std::uint32_t codePoint) {
    if (codePoint < 0x80u) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800u) {
        out.push_back(static_cast<char>(0xC0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint < 0x10000u) {
        out.push_back(static_cast<char>(0xE0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        out.push_back(static_cast<char>(0xF0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3Fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

// ---- 解析 ----

class JsonParser {
public:
    JsonParser(std::string_view text, ConfigTree& tree) : text_(text), tree_(tree) {}

    bool Run();

    const std::string& ErrorKind() const { return errorKind_; }
    std::size_t ErrorOffset() const { return errorOffset_; }

private:
    bool Eof() const { return cursor_ >= text_.size(); }
    char Peek() const { return text_[cursor_]; }
    void SkipSpace();
    bool Fail(std::string kind);
    bool MatchWord(const char* word);
    bool ReadValue(std::size_t parent, const std::string& name, int depth);
    bool ReadObject(std::size_t node, int depth);
    bool ReadString(std::string& out);
    bool ReadNumber(std::size_t parent, const std::string& name);
    bool ReadEscape(std::string& out);

    std::string_view text_;
    ConfigTree& tree_;
    std::size_t cursor_ = 0;
    std::string errorKind_;
    std::size_t errorOffset_ = 0;
};

bool JsonParser::Fail(std::string kind) {
    errorKind_ = std::move(kind);
    errorOffset_ = cursor_;
    return false;
}

void JsonParser::SkipSpace() {
    while (!Eof()) {
        const char c = Peek();
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++cursor_;
        } else {
            break;
        }
    }
}

bool JsonParser::MatchWord(const char* word) {
    const std::size_t length = std::strlen(word);
    if (text_.size() - cursor_ < length) {
        return false;
    }
    if (text_.compare(cursor_, length, word) != 0) {
        return false;
    }
    cursor_ += length;
    return true;
}

bool JsonParser::Run() {
    // 带 BOM 的文件是人手编辑出来的常见结果，1.x 的 File.ReadAllText 也吃它。
    if (text_.size() >= 3 && static_cast<unsigned char>(text_[0]) == 0xEF &&
        static_cast<unsigned char>(text_[1]) == 0xBB && static_cast<unsigned char>(text_[2]) == 0xBF) {
        cursor_ = 3;
    }

    SkipSpace();
    if (Eof()) {
        return Fail("empty");
    }
    if (Peek() != '{') {
        return Fail("not_object");
    }

    ++cursor_;
    if (!ReadObject(ConfigTree::kRoot, 1)) {
        return false;
    }

    SkipSpace();
    if (!Eof()) {
        return Fail("trailing");
    }
    return true;
}

bool JsonParser::ReadObject(std::size_t node, int depth) {
    SkipSpace();
    if (Eof()) {
        return Fail("unterminated");
    }
    if (Peek() == '}') {
        ++cursor_;
        return true;
    }

    while (true) {
        SkipSpace();
        std::string name;
        if (!ReadString(name)) {
            return false;
        }

        SkipSpace();
        if (Eof() || Peek() != ':') {
            return Fail("expected_colon");
        }
        ++cursor_;

        if (!ReadValue(node, name, depth)) {
            return false;
        }

        SkipSpace();
        if (Eof()) {
            return Fail("unterminated");
        }
        if (Peek() == ',') {
            ++cursor_;
            continue;
        }
        if (Peek() == '}') {
            ++cursor_;
            return true;
        }
        return Fail("expected_comma");
    }
}

bool JsonParser::ReadValue(std::size_t parent, const std::string& name, int depth) {
    SkipSpace();
    if (Eof()) {
        return Fail("unterminated");
    }

    ConfigValue value;
    const char c = Peek();

    if (c == '{') {
        // 深度在读的时候就被限制住，序列化那一侧才敢用递归。
        if (depth + 1 > ConfigTree::kMaxDepth) {
            return Fail("too_deep");
        }
        ++cursor_;
        value.kind = ValueKind::Object;
        const std::size_t node = tree_.AddMember(parent, name, value);
        return ReadObject(node, depth + 1);
    }

    if (c == '"') {
        value.kind = ValueKind::String;
        if (!ReadString(value.text)) {
            return false;
        }
        tree_.AddMember(parent, name, value);
        return true;
    }

    if (c == 't') {
        if (!MatchWord("true")) {
            return Fail("bad_token");
        }
        value.kind = ValueKind::Boolean;
        value.boolean = true;
        tree_.AddMember(parent, name, value);
        return true;
    }

    if (c == 'f') {
        if (!MatchWord("false")) {
            return Fail("bad_token");
        }
        value.kind = ValueKind::Boolean;
        value.boolean = false;
        tree_.AddMember(parent, name, value);
        return true;
    }

    if (c == 'n') {
        if (!MatchWord("null")) {
            return Fail("bad_token");
        }
        value.kind = ValueKind::Null;
        tree_.AddMember(parent, name, value);
        return true;
    }

    if (c == '-' || IsDigit(c)) {
        return ReadNumber(parent, name);
    }

    return Fail("bad_token");
}

bool JsonParser::ReadNumber(std::size_t parent, const std::string& name) {
    const std::size_t start = cursor_;
    bool negative = false;
    if (Peek() == '-') {
        negative = true;
        ++cursor_;
    }

    const std::size_t digitsStart = cursor_;
    unsigned long long accumulator = 0;
    bool overflow = false;
    while (!Eof() && IsDigit(Peek())) {
        if (accumulator > (0xFFFFFFFFFFFFFFFFull - 9u) / 10u) {
            overflow = true;
        }
        accumulator = accumulator * 10u + static_cast<unsigned>(Peek() - '0');
        ++cursor_;
    }

    if (cursor_ == digitsStart) {
        cursor_ = start;
        return Fail("bad_number");
    }
    if (cursor_ - digitsStart > 1 && text_[digitsStart] == '0') {
        // "0123" 不是 JSON；1.x 的解析器也不认。
        cursor_ = start;
        return Fail("bad_number");
    }
    if (overflow || accumulator > static_cast<unsigned long long>(0x7FFFFFFFFFFFFFFFull)) {
        cursor_ = start;
        return Fail("number_overflow");
    }

    bool fractional = false;
    if (!Eof() && Peek() == '.') {
        fractional = true;
        ++cursor_;
        const std::size_t fractionStart = cursor_;
        while (!Eof() && IsDigit(Peek())) {
            ++cursor_;
        }
        if (cursor_ == fractionStart) {
            cursor_ = start;
            return Fail("bad_number");
        }
    }
    if (!Eof() && (Peek() == 'e' || Peek() == 'E')) {
        fractional = true;
        ++cursor_;
        if (!Eof() && (Peek() == '+' || Peek() == '-')) {
            ++cursor_;
        }
        const std::size_t exponentStart = cursor_;
        while (!Eof() && IsDigit(Peek())) {
            ++cursor_;
        }
        if (cursor_ == exponentStart) {
            cursor_ = start;
            return Fail("bad_number");
        }
    }

    ConfigValue value;
    value.kind = ValueKind::Number;
    value.number = negative ? -static_cast<std::int64_t>(accumulator)
                            : static_cast<std::int64_t>(accumulator);
    value.numberFractional = fractional;
    tree_.AddMember(parent, name, value);
    return true;
}

bool JsonParser::ReadString(std::string& out) {
    out.clear();
    if (Eof() || Peek() != '"') {
        return Fail("bad_key_or_string");
    }
    ++cursor_;

    while (true) {
        if (Eof()) {
            return Fail("unterminated");
        }
        const char c = Peek();

        if (c == '"') {
            ++cursor_;
            return true;
        }
        if (c == '\\') {
            ++cursor_;
            if (!ReadEscape(out)) {
                return false;
            }
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20u) {
            // 裸控制字符必须转义（字符串里的换行会打断这一行判断，所以只在此处报错）。
            return Fail("bad_string");
        }
        // 非 ASCII 字节按原样搬运：文件本身是 UTF-8，core 只把它当字节串。
        out.push_back(c);
        ++cursor_;
    }
}

bool JsonParser::ReadEscape(std::string& out) {
    if (Eof()) {
        return Fail("unterminated");
    }
    const char c = Peek();
    ++cursor_;

    switch (c) {
    case '"': out.push_back('"'); return true;
    case '\\': out.push_back('\\'); return true;
    case '/': out.push_back('/'); return true;
    case 'b': out.push_back('\b'); return true;
    case 'f': out.push_back('\f'); return true;
    case 'n': out.push_back('\n'); return true;
    case 'r': out.push_back('\r'); return true;
    case 't': out.push_back('\t'); return true;
    case 'u': break;
    default: return Fail("bad_escape");
    }

    auto readUnit = [this](std::uint32_t& unit) {
        if (text_.size() - cursor_ < 4) {
            return false;
        }
        std::uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t digit = HexValue(text_[cursor_ + i]);
            if (digit > 0xFu) {
                return false;
            }
            value = value * 16u + digit;
        }
        cursor_ += 4;
        unit = value;
        return true;
    };

    std::uint32_t unit = 0;
    if (!readUnit(unit)) {
        return Fail("bad_unicode_escape");
    }

    if (unit >= 0xD800u && unit <= 0xDBFFu) {
        // 代理对必须成对出现，且第二个必须是低代理：1.x 写出的是转义后的中文，
        // 单个孤立代理只可能来自手工编辑，读到一半崩掉不如整个文件退回默认值。
        if (Eof() || Peek() != '\\' || text_.size() - cursor_ < 6 || text_[cursor_ + 1] != 'u') {
            return Fail("bad_unicode_escape");
        }
        cursor_ += 2;
        std::uint32_t low = 0;
        if (!readUnit(low) || low < 0xDC00u || low > 0xDFFFu) {
            return Fail("bad_unicode_escape");
        }
        AppendUtf8(out, 0x10000u + ((unit - 0xD800u) << 10) + (low - 0xDC00u));
        return true;
    }

    if (unit >= 0xDC00u && unit <= 0xDFFFu) {
        return Fail("bad_unicode_escape");
    }

    AppendUtf8(out, unit);
    return true;
}

void AppendJsonString(std::string_view text, std::string& out) {
    out.push_back('"');
    for (const char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (byte < 0x20u) {
                static const char kHex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kHex[byte >> 4]);
                out.push_back(kHex[byte & 0xFu]);
            } else {
                out.push_back(c);
            }
        }
    }
    out.push_back('"');
}

std::string Padding(int indent) {
    return std::string(static_cast<std::size_t>(indent) * 2u, ' ');
}

void AppendValue(const ConfigTree& tree, std::size_t node, int indent, std::string& out) {
    const ConfigValue& value = tree.At(node);
    switch (value.kind) {
    case ValueKind::Null: out += "null"; break;
    case ValueKind::Boolean: out += value.boolean ? "true" : "false"; break;
    case ValueKind::Number: out += std::to_string(value.number); break;
    case ValueKind::String: AppendJsonString(value.text, out); break;
    case ValueKind::Object: {
        if (value.memberNames.empty()) {
            out += "{}";
            break;
        }
        out += "{\n";
        for (std::size_t i = 0; i < value.memberNames.size(); ++i) {
            out += Padding(indent + 1);
            AppendJsonString(value.memberNames[i], out);
            out += ": ";
            AppendValue(tree, value.memberValues[i], indent + 1, out);
            out += (i + 1 == value.memberNames.size()) ? "\n" : ",\n";
        }
        out += Padding(indent);
        out += "}";
        break;
    }
    }
}

// ---- 手工搭一棵树用的四个小动作 ----

std::size_t AddText(ConfigTree& tree, std::size_t parent, const char* name, std::string_view value) {
    ConfigValue v;
    v.kind = ValueKind::String;
    v.text = std::string(value);
    return tree.AddMember(parent, name, v);
}

std::size_t AddBoolean(ConfigTree& tree, std::size_t parent, const char* name, bool value) {
    ConfigValue v;
    v.kind = ValueKind::Boolean;
    v.boolean = value;
    return tree.AddMember(parent, name, v);
}

std::size_t AddInteger(ConfigTree& tree, std::size_t parent, const char* name, std::int64_t value) {
    ConfigValue v;
    v.kind = ValueKind::Number;
    v.number = value;
    return tree.AddMember(parent, name, v);
}

std::size_t AddObject(ConfigTree& tree, std::size_t parent, const char* name) {
    ConfigValue v;
    v.kind = ValueKind::Object;
    return tree.AddMember(parent, name, v);
}

// ---- 枚举表 ----

struct EnumEntry {
    std::int32_t value;
    const char* name;
};

bool EnumFromName(const EnumEntry* table, std::size_t count, std::string_view name, std::int32_t& out) {
    const std::string lowered = Lower(TrimSpaces(name));
    for (std::size_t i = 0; i < count; ++i) {
        if (lowered == Lower(table[i].name)) {
            out = table[i].value;
            return true;
        }
    }
    return false;
}

bool EnumFromValue(const EnumEntry* table, std::size_t count, std::int32_t candidate, std::int32_t& out) {
    for (std::size_t i = 0; i < count; ++i) {
        if (table[i].value == candidate) {
            out = candidate;
            return true;
        }
    }
    return false;
}

const char* EnumToName(const EnumEntry* table, std::size_t count, std::int32_t value) {
    for (std::size_t i = 0; i < count; ++i) {
        if (table[i].value == value) {
            return table[i].name;
        }
    }
    return "";
}

#define HALOCAP_ENUM_TABLE(name, ...) \
    const EnumEntry name[] = { __VA_ARGS__ }

HALOCAP_ENUM_TABLE(kScreenshotFormatEntries, { 0, "Png" }, { 1, "Jpeg" });
HALOCAP_ENUM_TABLE(kMenuSizeEntries, { 0, "Small" }, { 1, "Medium" }, { 2, "Large" });
HALOCAP_ENUM_TABLE(kOcrEngineEntries, { 0, "Native" }, { 1, "Model" });
HALOCAP_ENUM_TABLE(kResolutionEntries, { 0, "Original" }, { 1, "P2160" }, { 2, "P1440" },
                   { 3, "P1080" }, { 4, "P720" });
HALOCAP_ENUM_TABLE(kFrameRateEntries, { 30, "Thirty" }, { 60, "Sixty" });
HALOCAP_ENUM_TABLE(kQualityEntries, { 0, "High" }, { 1, "Balanced" }, { 2, "Compact" });
HALOCAP_ENUM_TABLE(kCountdownEntries, { 0, "Off" }, { 3, "ThreeSeconds" }, { 5, "FiveSeconds" });
HALOCAP_ENUM_TABLE(kRangeEntries, { 0, "Region" }, { 1, "Window" });
HALOCAP_ENUM_TABLE(kAudioBitrateEntries, { 128000, "K128" }, { 192000, "K192" });

#undef HALOCAP_ENUM_TABLE

template <std::size_t N>
constexpr std::size_t EntryCount(const EnumEntry (&)[N]) {
    return N;
}

// 这份 schema 认得的键。热键既可能是 2.0 的文本也可能是 1.x 的对象，两种都算认得，
// 所以它们列在这里、命中之后不再往下钻（否则 hotkey.key 会被当成不认识的键报出来）。
const char* const kKnownLeafPaths[] = {
    "schemaVersion",   "hotkey",            "screenshotHotkey", "ocrHotkey",     "recordHotkey",
    "pinHotkey",       "screenshotFormat",  "menuSize",         "recordingDirectory",
    "convertFilePathsToImages", "pinGlowEnabled", "autoStart",
    "ocr.engine",      "ocr.autoCopy",      "ocr.pinOcrEnabled", "ocr.autoStart",
    "ocr.model",       "ocr.modelVariant",
    "recording.resolution", "recording.frameRate", "recording.quality", "recording.recordCursor",
    "recording.countdown",  "recording.range",     "recording.systemAudio", "recording.microphone",
    "recording.audioBitrate", "recording.autoStart",
};

// 1.x 把只读的计算属性也写进了文件（framesPerSecond、countdownSeconds、audioBitrateValue、
// wantsAudio 四个）。它自己读回时因为没有 setter 而忽略，2.0 同样忽略——但这不是「不认识的键」，
// 分开报才不至于让每一次导入都带着一串假警报。这四个是本机真文件里数出来的，不是照模型猜的。
const char* const kDerivedLegacyPaths[] = {
    "recording.framesPerSecond",
    "recording.countdownSeconds",
    "recording.audioBitrateValue",
    "recording.wantsAudio",
};

bool IsIn(const char* const* list, std::size_t count, std::string_view path) {
    for (std::size_t i = 0; i < count; ++i) {
        if (path == list[i]) {
            return true;
        }
    }
    return false;
}

void CollectUnrecognized(const ConfigTree& tree, std::size_t node, const std::string& prefix,
                         SettingsReadReport& report) {
    const ConfigValue& value = tree.At(node);
    if (value.kind != ValueKind::Object) {
        return;
    }

    for (std::size_t i = 0; i < value.memberNames.size(); ++i) {
        const std::string path = prefix.empty() ? value.memberNames[i] : prefix + "." + value.memberNames[i];
        const std::size_t child = value.memberValues[i];

        if (IsIn(kKnownLeafPaths, std::size(kKnownLeafPaths), path)) {
            continue;
        }
        if (IsIn(kDerivedLegacyPaths, std::size(kDerivedLegacyPaths), path)) {
            report.ignoredKeys.push_back(path);
            continue;
        }

        const ConfigValue& childValue = tree.At(child);
        if (childValue.kind == ValueKind::Object && !childValue.memberNames.empty()) {
            // 没认出来的对象继续往下钻，报出来的就是真正多余的叶子。
            CollectUnrecognized(tree, child, path, report);
            continue;
        }
        report.droppedKeys.push_back(path);
    }
}

// ---- 1.x 的两字段热键 ----

// WPF 的 ModifierKeys：None=0、Alt=1、Control=2、Shift=4、Windows=8。
// 数值与 2.0 的不同（我们 Alt 是 1、Control 是 2），所以必须逐项翻译，不能 cast。
bool ModifierFromWpfName(std::string_view name, std::uint32_t& bit) {
    const std::string lowered = Lower(TrimSpaces(name));
    if (lowered.empty() || lowered == "none") {
        bit = 0;
        return true;
    }
    if (lowered == "alt") { bit = kModAlt; return true; }
    if (lowered == "control") { bit = kModControl; return true; }
    if (lowered == "shift") { bit = kModShift; return true; }
    if (lowered == "windows") { bit = kModWin; return true; }
    return false;
}

bool ModifiersFromWpfText(std::string_view text, std::uint32_t& out) {
    out = 0;
    std::string_view remainder = text;

    while (true) {
        const auto comma = remainder.find(',');
        const std::string_view piece = comma == std::string_view::npos
                                           ? remainder
                                           : remainder.substr(0, comma);
        std::uint32_t bit = 0;
        if (!ModifierFromWpfName(piece, bit)) {
            return false;
        }
        out |= bit;

        if (comma == std::string_view::npos) {
            return true;
        }
        remainder = remainder.substr(comma + 1);
    }
}

bool ModifiersFromWpfInt(std::int64_t raw, std::uint32_t& out) {
    if (raw < 0 || raw > 15) {
        return false;
    }
    out = 0;
    if ((raw & 1) != 0) out |= kModAlt;
    if ((raw & 2) != 0) out |= kModControl;
    if ((raw & 4) != 0) out |= kModShift;
    if ((raw & 8) != 0) out |= kModWin;
    return true;
}

// WPF 的 Key 成员名与 2.0 的符号名大部分重合（A、F1、Insert、PageUp…），
// 剩下的按别名表翻译。小键盘数字与 CapsLock 在 2.0 的符号表里没有位置，
// 认不出来就走修复路径（回默认值 + 报出来），不硬编一个相近的键。
bool KeyFromLegacyName(std::string_view name, KeySymbol& out) {
    if (KeySymbolFromName(name, out)) {
        return true;
    }

    const std::string lowered = Lower(TrimSpaces(name));

    if (lowered.size() == 2 && lowered[0] == 'd' && IsDigit(lowered[1])) {
        out = static_cast<KeySymbol>(static_cast<std::int32_t>(KeySymbol::Num0) + (lowered[1] - '0'));
        return true;
    }

    struct Alias {
        const char* name;
        KeySymbol key;
    };
    static constexpr Alias kAliases[] = {
        { "escape", KeySymbol::Escape },
        { "return", KeySymbol::Enter },
        { "back", KeySymbol::Backspace },
        { "snapshot", KeySymbol::PrintScreen },
        { "printscreen", KeySymbol::PrintScreen },
    };
    for (const auto& alias : kAliases) {
        if (lowered == alias.name) {
            out = alias.key;
            return true;
        }
    }
    return false;
}

bool ReadLegacyHotkey(const ConfigTree& tree, const ConfigValue& node, HotkeySpec& out) {
    const ConfigValue* key = nullptr;
    const ConfigValue* modifiers = nullptr;

    for (std::size_t i = 0; i < node.memberNames.size(); ++i) {
        if (node.memberNames[i] == "key") {
            key = &tree.At(node.memberValues[i]);
        } else if (node.memberNames[i] == "modifiers") {
            modifiers = &tree.At(node.memberValues[i]);
        }
    }

    if (key == nullptr || key->kind != ValueKind::String || !KeyFromLegacyName(key->text, out.key)) {
        return false;
    }

    out.modifiers = 0;
    if (modifiers == nullptr) {
        return true;
    }
    if (modifiers->kind == ValueKind::String) {
        return ModifiersFromWpfText(modifiers->text, out.modifiers);
    }
    if (modifiers->kind == ValueKind::Number && !modifiers->numberFractional) {
        return ModifiersFromWpfInt(modifiers->number, out.modifiers);
    }
    return false;
}

// ---- 逐键读取 ----

void MarkRepaired(const char* path, SettingsReadReport& report) {
    report.repairedKeys.emplace_back(path);
}

void ReadBoolField(const ConfigTree& tree, const char* path, bool& field, SettingsReadReport& report) {
    const ConfigValue* value = tree.Find(path);
    if (value == nullptr) {
        return;
    }
    if (value->kind == ValueKind::Boolean) {
        field = value->boolean;
        return;
    }
    // 1.x 的设置页写过 "true"/"false" 字符串吗？没有，但手工编辑的人会用 1/0。
    if (value->kind == ValueKind::Number && !value->numberFractional &&
        (value->number == 0 || value->number == 1)) {
        field = value->number == 1;
        return;
    }
    MarkRepaired(path, report);
}

void ReadStringField(const ConfigTree& tree, const char* path, std::string& field,
                     SettingsReadReport& report) {
    const ConfigValue* value = tree.Find(path);
    if (value == nullptr) {
        return;
    }
    if (value->kind == ValueKind::String) {
        field = value->text;
        return;
    }
    MarkRepaired(path, report);
}

template <typename Enum>
void ReadEnumField(const ConfigTree& tree, const char* path, const EnumEntry* table,
                   std::size_t count, Enum& field, SettingsReadReport& report) {
    const ConfigValue* value = tree.Find(path);
    if (value == nullptr) {
        return;
    }

    std::int32_t raw = 0;
    if (value->kind == ValueKind::String) {
        if (!EnumFromName(table, count, value->text, raw)) {
            MarkRepaired(path, report);
            return;
        }
    } else if (value->kind == ValueKind::Number && !value->numberFractional &&
               value->number >= 0 && value->number <= 0x7FFFFFFFll) {
        // 1.x 的序列化器允许整数形式的枚举，早期文件里出现过。
        if (!EnumFromValue(table, count, static_cast<std::int32_t>(value->number), raw)) {
            MarkRepaired(path, report);
            return;
        }
    } else {
        MarkRepaired(path, report);
        return;
    }

    field = static_cast<Enum>(raw);
}

void ReadHotkeyField(const ConfigTree& tree, const char* path, HotkeySpec& field,
                     SettingsReadReport& report) {
    const ConfigValue* value = tree.Find(path);
    if (value == nullptr) {
        return;
    }

    HotkeySpec parsed;
    if (value->kind == ValueKind::String) {
        if (!ParseHotkeyText(value->text, parsed)) {
            MarkRepaired(path, report);
            return;
        }
    } else if (value->kind == ValueKind::Object) {
        if (!ReadLegacyHotkey(tree, *value, parsed)) {
            MarkRepaired(path, report);
            return;
        }
        report.migratedKeys.emplace_back(path);
    } else {
        MarkRepaired(path, report);
        return;
    }

    if (!CheckHotkey(parsed).valid) {
        MarkRepaired(path, report);
        return;
    }
    field = parsed;
}

std::int32_t TargetHeight(RecordingResolution resolution) {
    switch (resolution) {
    case RecordingResolution::P2160: return 2160;
    case RecordingResolution::P1440: return 1440;
    case RecordingResolution::P1080: return 1080;
    case RecordingResolution::P720: return 720;
    default: return 0;
    }
}

int MakeEven(int value) { return value - (value & 1); }

int ClampInt(int value, int low, int high) {
    // 1.x 用的是 Math.Clamp，min > max 会抛；这里没有可抛的东西，返回 high 表示
    // 「这条尺寸不成立」，调用方（录制起点）据此拒绝开录。
    if (low > high) {
        return high;
    }
    return std::min(std::max(value, low), high);
}

int BitsPerPixelPermille(RecordingQuality quality) {
    switch (quality) {
    case RecordingQuality::High: return 200;
    case RecordingQuality::Compact: return 55;
    default: return 110;
    }
}

}  // namespace

// ---- ConfigTree ----

ConfigTree::ConfigTree() { ResetRoot(); }

void ConfigTree::ResetRoot() {
    values_.clear();
    ConfigValue root;
    root.kind = ValueKind::Object;
    values_.push_back(std::move(root));
}

std::size_t ConfigTree::AddMember(std::size_t parent, std::string_view name, const ConfigValue& value) {
    ConfigValue copy = value;
    const std::size_t index = values_.size();
    values_.push_back(std::move(copy));

    ConfigValue& parentNode = values_[parent];
    parentNode.memberNames.emplace_back(name);
    parentNode.memberValues.push_back(index);
    return index;
}

const ConfigValue* ConfigTree::FindMember(std::size_t node, std::string_view name) const {
    const ConfigValue& value = values_[node];
    const ConfigValue* found = nullptr;
    for (std::size_t i = 0; i < value.memberNames.size(); ++i) {
        if (value.memberNames[i] == name) {
            // 重复键取最后一个：与 1.x 的反序列化一致（后写的覆盖先写的）。
            found = &values_[value.memberValues[i]];
        }
    }
    return found;
}

const ConfigValue* ConfigTree::Find(std::string_view dottedPath) const {
    std::size_t node = kRoot;
    std::string_view remainder = dottedPath;

    while (!remainder.empty()) {
        const auto dot = remainder.find('.');
        const std::string_view segment =
            dot == std::string_view::npos ? remainder : remainder.substr(0, dot);
        remainder = dot == std::string_view::npos ? std::string_view{} : remainder.substr(dot + 1);

        const ConfigValue* member = FindMember(node, segment);
        if (member == nullptr) {
            return nullptr;
        }

        if (remainder.empty()) {
            return member;
        }
        const std::size_t index = static_cast<std::size_t>(member - values_.data());
        if (member->kind != ValueKind::Object) {
            return nullptr;
        }
        node = index;
    }
    return nullptr;
}

bool ConfigTree::Parse(std::string_view utf8) {
    // 先解析到一棵临时树里：出错时既不留半棵树，也不会把刚记下的错误码清掉。
    ConfigTree scratch;
    JsonParser parser(utf8, scratch);

    errorKind_.clear();
    errorOffset_ = 0;

    if (!parser.Run()) {
        errorKind_ = parser.ErrorKind();
        errorOffset_ = parser.ErrorOffset();
        ResetRoot();
        return false;
    }

    values_ = std::move(scratch.values_);
    return true;
}

std::string ConfigTree::Serialize() const {
    std::string out;
    out.reserve(512);
    AppendValue(*this, kRoot, 0, out);
    return out;
}

// ---- 热键合法性 ----

bool IsSafeSingleKey(KeySymbol key) {
    const std::int32_t code = static_cast<std::int32_t>(key);
    if (code >= static_cast<std::int32_t>(KeySymbol::F1) &&
        code <= static_cast<std::int32_t>(KeySymbol::F24)) {
        return true;
    }
    if (code >= static_cast<std::int32_t>(KeySymbol::A) &&
        code <= static_cast<std::int32_t>(KeySymbol::Z)) {
        return true;
    }
    if (code >= static_cast<std::int32_t>(KeySymbol::Num0) &&
        code <= static_cast<std::int32_t>(KeySymbol::Num9)) {
        return true;
    }
    switch (key) {
    case KeySymbol::Insert:
    case KeySymbol::Home:
    case KeySymbol::End:
    case KeySymbol::PageUp:
    case KeySymbol::PageDown:
    case KeySymbol::PrintScreen:
        return true;
    default:
        return false;
    }
}

HotkeyCheck CheckHotkey(const HotkeySpec& spec) {
    if (spec.key == KeySymbol::None) {
        return { false, "none" };
    }
    // 1.x 在这里还挡了一道「键本身不是修饰键」。2.0 的符号表里没有左 Ctrl 这类符号，
    // 所以那条规则由类型系统保证；但修饰位仍然要查——多出来的位没人定义过。
    if ((spec.modifiers & ~(kModAlt | kModControl | kModShift | kModWin | kModNoRepeat)) != 0) {
        return { false, "bad_modifier" };
    }
    if (spec.modifiers == 0 && !IsSafeSingleKey(spec.key)) {
        return { false, "unsafe_single_key" };
    }
    return { true, "" };
}

const HotkeySpec* HotkeySlot(const AppSettings& settings, int slot) {
    switch (slot) {
    case 0: return &settings.radialHotkey;
    case 1: return &settings.screenshotHotkey;
    case 2: return &settings.ocrHotkey;
    case 3: return &settings.recordHotkey;
    case 4: return &settings.pinHotkey;
    default: return nullptr;
    }
}

HotkeySpec* HotkeySlot(AppSettings& settings, int slot) {
    return const_cast<HotkeySpec*>(
        HotkeySlot(static_cast<const AppSettings&>(settings), slot));
}

int FindHotkeyConflict(const AppSettings& settings, int slot) {
    const HotkeySpec* candidate = HotkeySlot(settings, slot);
    if (candidate == nullptr || candidate->key == KeySymbol::None) {
        return -1;
    }
    const std::uint32_t modifiers = candidate->modifiers & kStateModifiers;
    for (int other = 0; other < kHotkeySlotCount; ++other) {
        if (other == slot) {
            continue;
        }
        const HotkeySpec* spec = HotkeySlot(settings, other);
        if (spec->key == candidate->key && (spec->modifiers & kStateModifiers) == modifiers) {
            return other;
        }
    }
    return -1;
}

// ---- 读取与写出 ----

void ParseSettings(std::string_view utf8, AppSettings& settings, SettingsReadReport& report) {
    settings = AppSettings{};
    report = SettingsReadReport{};

    ConfigTree tree;
    if (!tree.Parse(utf8)) {
        report.parseError = tree.ErrorKind();
        report.parseErrorOffset = tree.ErrorOffset();
        return;
    }

    if (const ConfigValue* version = tree.Find("schemaVersion")) {
        if (version->kind == ValueKind::Number && !version->numberFractional &&
            version->number >= 0 && version->number <= 0x7FFFFFFFll) {
            report.schemaVersion = static_cast<std::int32_t>(version->number);
        } else {
            MarkRepaired("schemaVersion", report);
        }
    }

    ReadHotkeyField(tree, "hotkey", settings.radialHotkey, report);
    ReadHotkeyField(tree, "screenshotHotkey", settings.screenshotHotkey, report);
    ReadHotkeyField(tree, "ocrHotkey", settings.ocrHotkey, report);
    ReadHotkeyField(tree, "recordHotkey", settings.recordHotkey, report);
    ReadHotkeyField(tree, "pinHotkey", settings.pinHotkey, report);

    ReadEnumField(tree, "screenshotFormat", kScreenshotFormatEntries, EntryCount(kScreenshotFormatEntries),
                  settings.screenshotFormat, report);
    ReadEnumField(tree, "menuSize", kMenuSizeEntries, EntryCount(kMenuSizeEntries), settings.menuSize, report);
    ReadStringField(tree, "recordingDirectory", settings.recordingDirectory, report);
    ReadBoolField(tree, "convertFilePathsToImages", settings.convertFilePathsToImages, report);
    ReadBoolField(tree, "pinGlowEnabled", settings.pinGlowEnabled, report);
    ReadBoolField(tree, "autoStart", settings.autoStart, report);

    ReadEnumField(tree, "ocr.engine", kOcrEngineEntries, EntryCount(kOcrEngineEntries), settings.ocr.engine,
                  report);
    ReadBoolField(tree, "ocr.autoCopy", settings.ocr.autoCopy, report);
    ReadBoolField(tree, "ocr.pinOcrEnabled", settings.ocr.pinOcrEnabled, report);
    ReadBoolField(tree, "ocr.autoStart", settings.ocr.autoStart, report);
    ReadStringField(tree, "ocr.model", settings.ocr.model, report);
    // 2.2.0 写的是「极速/高精度」二选一；同名的模型目录还在就沿用，避免升级后回到内置。
    if (settings.ocr.model.empty()) {
        const ConfigValue* legacy = tree.Find("ocr.modelVariant");
        if (legacy != nullptr && legacy->kind == ValueKind::String &&
            legacy->text == "ppocrv6-small") {
            settings.ocr.model = "ppocrv6-small";
            report.migratedKeys.emplace_back("ocr.modelVariant");
        }
    }

    ReadEnumField(tree, "recording.resolution", kResolutionEntries, EntryCount(kResolutionEntries),
                  settings.recording.resolution, report);
    ReadEnumField(tree, "recording.frameRate", kFrameRateEntries, EntryCount(kFrameRateEntries),
                  settings.recording.frameRate, report);
    ReadEnumField(tree, "recording.quality", kQualityEntries, EntryCount(kQualityEntries),
                  settings.recording.quality, report);
    ReadBoolField(tree, "recording.recordCursor", settings.recording.recordCursor, report);
    ReadEnumField(tree, "recording.countdown", kCountdownEntries, EntryCount(kCountdownEntries),
                  settings.recording.countdown, report);
    ReadEnumField(tree, "recording.range", kRangeEntries, EntryCount(kRangeEntries), settings.recording.range,
                  report);
    ReadBoolField(tree, "recording.systemAudio", settings.recording.systemAudio, report);
    ReadBoolField(tree, "recording.microphone", settings.recording.microphone, report);
    ReadEnumField(tree, "recording.audioBitrate", kAudioBitrateEntries, EntryCount(kAudioBitrateEntries),
                  settings.recording.audioBitrate, report);
    ReadBoolField(tree, "recording.autoStart", settings.recording.autoStart, report);

    CollectUnrecognized(tree, ConfigTree::kRoot, "", report);
}

std::string SerializeSettings(const AppSettings& settings) {
    ConfigTree tree;
    const std::size_t root = ConfigTree::kRoot;

    // 键序与 1.x 的属性声明顺序一致，枚举名直接用对外的 *Name 函数——
    // 读侧和写侧共用同一张表，才不会有一天写出一个自己读不回来的名字。
    AddInteger(tree, root, "schemaVersion", kSettingsSchemaVersion);
    AddText(tree, root, "hotkey", FormatHotkeyText(settings.radialHotkey));
    AddText(tree, root, "screenshotHotkey", FormatHotkeyText(settings.screenshotHotkey));
    AddText(tree, root, "ocrHotkey", FormatHotkeyText(settings.ocrHotkey));
    AddText(tree, root, "recordHotkey", FormatHotkeyText(settings.recordHotkey));
    AddText(tree, root, "pinHotkey", FormatHotkeyText(settings.pinHotkey));
    AddText(tree, root, "screenshotFormat", ScreenshotFormatName(settings.screenshotFormat));
    AddText(tree, root, "menuSize", MenuSizeName(settings.menuSize));
    AddText(tree, root, "recordingDirectory", settings.recordingDirectory);
    AddBoolean(tree, root, "convertFilePathsToImages", settings.convertFilePathsToImages);
    AddBoolean(tree, root, "pinGlowEnabled", settings.pinGlowEnabled);
    AddBoolean(tree, root, "autoStart", settings.autoStart);

    const std::size_t ocr = AddObject(tree, root, "ocr");
    AddText(tree, ocr, "engine", OcrEngineName(settings.ocr.engine));
    AddBoolean(tree, ocr, "autoCopy", settings.ocr.autoCopy);
    AddBoolean(tree, ocr, "pinOcrEnabled", settings.ocr.pinOcrEnabled);
    AddBoolean(tree, ocr, "autoStart", settings.ocr.autoStart);
    AddText(tree, ocr, "model", settings.ocr.model);

    const std::size_t recording = AddObject(tree, root, "recording");
    AddText(tree, recording, "resolution", ResolutionName(settings.recording.resolution));
    AddText(tree, recording, "frameRate", FrameRateName(settings.recording.frameRate));
    AddText(tree, recording, "quality", QualityName(settings.recording.quality));
    AddBoolean(tree, recording, "recordCursor", settings.recording.recordCursor);
    AddText(tree, recording, "countdown", CountdownName(settings.recording.countdown));
    AddText(tree, recording, "range", RangeName(settings.recording.range));
    AddBoolean(tree, recording, "systemAudio", settings.recording.systemAudio);
    AddBoolean(tree, recording, "microphone", settings.recording.microphone);
    AddText(tree, recording, "audioBitrate", AudioBitrateName(settings.recording.audioBitrate));
    AddBoolean(tree, recording, "autoStart", settings.recording.autoStart);

    return tree.Serialize();
}

// ---- 录制参数落到像素与码率 ----

int RecordingSettings::FramesPerSecond() const {
    return static_cast<int>(frameRate);
}

int RecordingSettings::CountdownSeconds() const {
    return static_cast<int>(countdown);
}

std::int64_t RecordingSettings::AudioBitrateBitsPerSec() const {
    return static_cast<std::int64_t>(audioBitrate);
}

SizedOutput ResolveOutputSize(const RecordingSettings& settings, int sourceWidthPx, int sourceHeightPx) {
    if (sourceWidthPx <= 0 || sourceHeightPx <= 0) {
        return {};
    }

    const int targetHeight = TargetHeight(settings.resolution);
    if (targetHeight <= 0 || sourceHeightPx <= targetHeight) {
        return { MakeEven(sourceWidthPx), MakeEven(sourceHeightPx) };
    }

    // 1.x 用 Math.Round(double)，那是「到偶数」的四舍六入；这里用整数长除复现同一条规则，
    // 免得两份实现恰好在一半的位置上给出不同尺寸。
    const long long numerator = static_cast<long long>(sourceWidthPx) * targetHeight;
    const long long denominator = sourceHeightPx;
    long long quotient = numerator / denominator;
    const long long remainder = numerator % denominator;
    if (remainder * 2 > denominator ||
        (remainder * 2 == denominator && (quotient % 2) != 0)) {
        quotient += 1;
    }

    SizedOutput out;
    // 宽还要再取一次偶：比值恰好落在奇数整数上时（6×864 → P720 给出 5）1.x 会把奇数宽度
    // 交给编码器，而 2.0 的编码 seam 在 OpenVideo 就拒收奇数（NV12 的 4:2:0 划分不允许）。
    // 这是两版唯一故意不同的地方，取偶向下偏 1 像素，换录制能不能开始。
    out.widthPx = ClampInt(MakeEven(static_cast<int>(quotient)), 2, MakeEven(sourceWidthPx));
    out.heightPx = ClampInt(MakeEven(targetHeight), 2, MakeEven(sourceHeightPx));
    return out;
}

std::int64_t ComputeVideoBitrateBitsPerSec(const RecordingSettings& settings, int widthPx, int heightPx) {
    constexpr std::int64_t kMinBitrate = 1'000'000;
    constexpr std::int64_t kMaxBitrate = 45'000'000;

    if (widthPx <= 0 || heightPx <= 0) {
        return kMinBitrate;
    }

    const std::int64_t estimated = static_cast<std::int64_t>(widthPx) * heightPx *
                                   settings.FramesPerSecond() * BitsPerPixelPermille(settings.quality) / 1000;
    return std::min(std::max(estimated, kMinBitrate), kMaxBitrate);
}

// ---- 显示名 ----

const char* ScreenshotFormatName(ScreenshotFormat value) {
    return EnumToName(kScreenshotFormatEntries, EntryCount(kScreenshotFormatEntries), static_cast<std::int32_t>(value));
}
const char* MenuSizeName(MenuSize value) {
    return EnumToName(kMenuSizeEntries, EntryCount(kMenuSizeEntries), static_cast<std::int32_t>(value));
}
const char* OcrEngineName(OcrEngine value) {
    return EnumToName(kOcrEngineEntries, EntryCount(kOcrEngineEntries), static_cast<std::int32_t>(value));
}
const char* ResolutionName(RecordingResolution value) {
    return EnumToName(kResolutionEntries, EntryCount(kResolutionEntries), static_cast<std::int32_t>(value));
}
const char* FrameRateName(RecordingFrameRate value) {
    return EnumToName(kFrameRateEntries, EntryCount(kFrameRateEntries), static_cast<std::int32_t>(value));
}
const char* QualityName(RecordingQuality value) {
    return EnumToName(kQualityEntries, EntryCount(kQualityEntries), static_cast<std::int32_t>(value));
}
const char* CountdownName(RecordingCountdown value) {
    return EnumToName(kCountdownEntries, EntryCount(kCountdownEntries), static_cast<std::int32_t>(value));
}
const char* RangeName(RecordingRange value) {
    return EnumToName(kRangeEntries, EntryCount(kRangeEntries), static_cast<std::int32_t>(value));
}
const char* AudioBitrateName(RecordingAudioBitrate value) {
    return EnumToName(kAudioBitrateEntries, EntryCount(kAudioBitrateEntries), static_cast<std::int32_t>(value));
}

}
