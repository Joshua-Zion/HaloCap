// 设置 seam 的第一套测试：1.x 的 settings.json 能被读成什么、2.0 写出去的文件能不能读回来、
// 以及逐键修复与报告到底报了什么。这里不碰磁盘——磁盘那一半在 tools/store_probe 里。
#include "settings.h"

#include "encoder.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace core = halocap::core;

using core::AppSettings;
using core::ConfigTree;
using core::HotkeyCheck;
using core::HotkeySpec;
using core::KeySymbol;
using core::SettingsReadReport;

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

void CheckText(std::string_view actual, std::string_view expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%.*s\" want \"%.*s\"\n", what, static_cast<int>(actual.size()),
                    actual.data(), static_cast<int>(expected.size()), expected.data());
    }
}

void CheckList(const std::vector<std::string>& actual, std::initializer_list<const char*> expected,
               const char* what) {
    ++checks;
    bool ok = actual.size() == expected.size();
    if (ok) {
        std::size_t index = 0;
        for (const char* want : expected) {
            if (actual[index] != want) {
                ok = false;
                break;
            }
            ++index;
        }
    }
    if (!ok) {
        ++failures;
        std::printf("FAIL %s: [", what);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            std::printf("%s%s", i == 0 ? "" : ", ", actual[i].c_str());
        }
        std::printf("] wanted [");
        bool first = true;
        for (const char* want : expected) {
            std::printf("%s%s", first ? "" : ", ", want);
            first = false;
        }
        std::printf("]\n");
    }
}

void CheckEmpty(const std::vector<std::string>& actual, const char* what) {
    ++checks;
    if (!actual.empty()) {
        ++failures;
        std::printf("FAIL %s: expected empty, got %zu entries\n", what, actual.size());
    }
}

struct Parsed {
    AppSettings settings;
    SettingsReadReport report;
};

Parsed Parse(std::string_view text) {
    Parsed parsed;
    core::ParseSettings(text, parsed.settings, parsed.report);
    return parsed;
}

void CheckHotkey(const HotkeySpec& actual, KeySymbol key, std::uint32_t modifiers, const char* what) {
    ++checks;
    if (actual.key != key || actual.modifiers != modifiers) {
        ++failures;
        std::printf("FAIL %s: got key=%s mods=0x%X want key=%s mods=0x%X\n", what,
                    core::KeySymbolName(actual.key), actual.modifiers, core::KeySymbolName(key), modifiers);
    }
}

// 1.x 真正写在磁盘上的形状（本机 %APPDATA%\HaloCap\settings.json 的字段顺序与值）。
// recording 里那四个只读计算属性 1.x 也会写出来，读回时被它自己忽略。
constexpr const char* kLegacyDocument = R"({
  "hotkey": {
    "modifiers": "None",
    "key": "F6"
  },
  "screenshotHotkey": {
    "modifiers": "None",
    "key": "F1"
  },
  "ocrHotkey": {
    "modifiers": "None",
    "key": "F7"
  },
  "recordHotkey": {
    "modifiers": "None",
    "key": "F8"
  },
  "pinHotkey": {
    "modifiers": "None",
    "key": "F3"
  },
  "screenshotFormat": "Png",
  "menuSize": "Small",
  "recordingDirectory": "",
  "convertFilePathsToImages": true,
  "pinGlowEnabled": true,
  "ocr": {
    "engine": "Model",
    "autoCopy": false,
    "pinOcrEnabled": true
  },
  "recording": {
    "resolution": "Original",
    "frameRate": "Thirty",
    "quality": "Balanced",
    "recordCursor": true,
    "countdown": "ThreeSeconds",
    "range": "Region",
    "systemAudio": true,
    "microphone": false,
    "audioBitrate": "K192",
    "framesPerSecond": 30,
    "countdownSeconds": 3,
    "audioBitrateValue": 192000,
    "wantsAudio": true
  }
})";

std::string NestedDocument(int objects) {
    std::string document = R"({"b":1})";
    for (int i = 1; i < objects; ++i) {
        document = "{\"a\":" + document + "}";
    }
    return document;
}

void TestConfigTreeBasics() {
    ConfigTree tree;
    CheckEqual(static_cast<long long>(tree.At(ConfigTree::kRoot).kind),
               static_cast<long long>(core::ValueKind::Object), "empty tree has an object root");
    CheckTrue(tree.At(ConfigTree::kRoot).memberNames.empty(), "empty tree has no members");
    CheckText(tree.Serialize(), "{}", "empty tree serializes to {}");

    CheckTrue(tree.Parse(R"({"a":1,"b":true,"c":"x","d":null})"), "flat object parses");
    CheckEqual(tree.Find("a")->number, 1, "integer member");
    CheckTrue(tree.Find("b")->boolean, "boolean member");
    CheckText(tree.Find("c")->text, "x", "string member");
    CheckEqual(static_cast<long long>(tree.Find("d")->kind),
               static_cast<long long>(core::ValueKind::Null), "null member");
    CheckTrue(tree.Find("missing") == nullptr, "missing member is null");

    // 重复键取最后一个：与 1.x 的反序列化一致（后写的覆盖先写的）。
    CheckTrue(tree.Parse(R"({"a":1,"a":2})"), "duplicate keys still parse");
    CheckEqual(tree.Find("a")->number, 2, "duplicate key takes the last value");

    CheckTrue(tree.Parse(R"({"a":{"b":{"c":7}}})"), "nested objects parse");
    CheckEqual(tree.Find("a.b.c")->number, 7, "dotted path finds a nested leaf");
    CheckTrue(tree.Find("a.b") != nullptr, "dotted path finds a nested object");
    CheckTrue(tree.Find("a.b.missing") == nullptr, "dotted path through a missing link is null");
    CheckTrue(tree.Find("a.x.y") == nullptr, "dotted path through a leaf is null");

    // 带 BOM 的文件是人手编辑出来的常见结果。
    CheckTrue(tree.Parse("\xEF\xBB\xBF{}"), "UTF-8 BOM is accepted");
}

void TestConfigTreeErrors() {
    // 嵌套过深那一条要单独建字符串：把临时 std::string 的 c_str() 塞进静态数组会悬空。
    const std::string tooDeep = NestedDocument(17);
    const std::string deepestPath = [] {
        std::string path;
        for (int i = 1; i < 16; ++i) {
            path += "a.";
        }
        return path + "b";
    }();

    struct Case {
        const char* text;
        const char* kind;
        const char* what;
    };
    const Case kCases[] = {
        { "", "empty", "empty input" },
        { "   \n\t ", "empty", "whitespace-only input" },
        { "[]", "not_object", "array root" },
        { "null", "not_object", "null root" },
        { "{", "unterminated", "unterminated object" },
        { "{}x", "trailing", "trailing garbage" },
        { R"({"a":})", "bad_token", "missing value" },
        { R"({"a" 1})", "expected_colon", "missing colon" },
        { R"({"a":1 "b":2})", "expected_comma", "missing comma" },
        { R"({"a":0123})", "bad_number", "leading zero" },
        { R"({"a":-})", "bad_number", "minus with no digits" },
        { R"({"a":1.})", "bad_number", "decimal point with no fraction" },
        { R"({"a":1e})", "bad_number", "exponent with no digits" },
        { R"({"a":99999999999999999999999})", "number_overflow", "number overflow" },
        { R"({"a":"\q"})", "bad_escape", "unknown escape" },
        { R"({"a":"\ud83d"})", "bad_unicode_escape", "lone high surrogate" },
        { R"({"a":"\ude00"})", "bad_unicode_escape", "lone low surrogate" },
        { R"({"a":"\ud83dx"})", "bad_unicode_escape", "surrogate pair cut short" },
        { "{\"a\":\"\x01\"}", "bad_string", "raw control character in a string" },
        { tooDeep.c_str(), "too_deep", "17 nested objects" },
    };

    for (const auto& testCase : kCases) {
        ConfigTree tree;
        const bool ok = tree.Parse(testCase.text);
        ++checks;
        if (ok) {
            ++failures;
            std::printf("FAIL %s: expected a parse error, got none\n", testCase.what);
            continue;
        }
        if (tree.ErrorKind() != testCase.kind) {
            ++failures;
            std::printf("FAIL %s: got error \"%s\" want \"%s\"\n", testCase.what,
                        tree.ErrorKind().c_str(), testCase.kind);
        }
    }

    // 16 层正好在限制之内，且失败之后树要退回到一个干净的空根。
    ConfigTree deep;
    CheckTrue(deep.Parse(NestedDocument(16)), "16 nested objects are accepted");
    CheckEqual(deep.Find(deepestPath)->number, 1, "deepest leaf is reachable");

    ConfigTree broken;
    CheckTrue(!broken.Parse("{\"a\":1,"), "broken document is rejected");
    CheckTrue(broken.Find("a") == nullptr, "failed parse leaves no half tree");
    CheckText(broken.Serialize(), "{}", "failed parse resets to an empty root");
}

void TestStringEscapes() {
    ConfigTree tree;
    CheckTrue(tree.Parse(R"({"a":"\ud83d\ude00"})"), "surrogate pair parses");
    CheckEqual(static_cast<long long>(tree.Find("a")->text.size()), 4, "surrogate pair is 4 UTF-8 bytes");

    CheckTrue(tree.Parse(R"({"a":"\u4e2d\u6587"})"), "CJK escapes parse");
    // 1.x 的序列化器把中文写成 \uXXXX，读回来必须还是同样的字节。
    CheckText(tree.Find("a")->text, "\xE4\xB8\xAD\xE6\x96\x87", "CJK escapes decode to UTF-8");

    CheckTrue(tree.Parse(R"({"a":"line1\nline2\t\"q\"\\"})"), "common escapes parse");
    CheckText(tree.Find("a")->text, "line1\nline2\t\"q\"\\", "common escapes decode");

    // 非 ASCII 原样搬运：文件本身是 UTF-8，core 只把它当字节串。
    CheckTrue(tree.Parse("{\"a\":\"\xE4\xB8\xAD\xE6\x96\x87\"}"), "raw UTF-8 parses");
    CheckText(tree.Find("a")->text, "\xE4\xB8\xAD\xE6\x96\x87", "raw UTF-8 is preserved");

    CheckTrue(tree.Parse(R"({"a":-42})"), "negative number parses");
    CheckEqual(tree.Find("a")->number, -42, "negative number value");
    CheckTrue(tree.Parse(R"({"a":1.5})"), "fractional number parses");
    CheckTrue(tree.Find("a")->numberFractional, "fractional flag is set");
    CheckTrue(tree.Parse(R"({"a":1e3})"), "exponent number parses");
    CheckTrue(tree.Find("a")->numberFractional, "exponent marks the number fractional");
}

void TestDefaults() {
    const AppSettings settings;

    CheckHotkey(settings.radialHotkey, KeySymbol::D, core::kModControl | core::kModAlt, "radial default");
    CheckHotkey(settings.screenshotHotkey, KeySymbol::A, core::kModControl | core::kModAlt,
                "screenshot default");
    CheckHotkey(settings.ocrHotkey, KeySymbol::O, core::kModControl | core::kModAlt, "ocr default");
    CheckHotkey(settings.recordHotkey, KeySymbol::R, core::kModControl | core::kModAlt, "record default");
    CheckHotkey(settings.pinHotkey, KeySymbol::P, core::kModControl | core::kModAlt, "pin default");

    CheckTrue(settings.screenshotFormat == core::ScreenshotFormat::Png, "screenshot format default");
    CheckTrue(settings.menuSize == core::MenuSize::Medium, "menu size default");
    CheckTrue(settings.recordingDirectory.empty(), "recording directory default is empty");
    CheckTrue(settings.convertFilePathsToImages, "convert paths default");
    CheckTrue(settings.pinGlowEnabled, "pin glow default");

    CheckTrue(settings.ocr.engine == core::OcrEngine::Native, "ocr engine default");
    CheckTrue(!settings.ocr.autoCopy, "ocr autoCopy default");
    CheckTrue(settings.ocr.pinOcrEnabled, "ocr pinOcr default");

    const core::RecordingSettings& recording = settings.recording;
    CheckTrue(recording.resolution == core::RecordingResolution::Original, "resolution default");
    CheckTrue(recording.frameRate == core::RecordingFrameRate::Thirty, "frame rate default");
    CheckTrue(recording.quality == core::RecordingQuality::Balanced, "quality default");
    CheckTrue(recording.recordCursor, "record cursor default");
    CheckTrue(recording.countdown == core::RecordingCountdown::ThreeSeconds, "countdown default");
    CheckTrue(recording.range == core::RecordingRange::Region, "range default");
    CheckTrue(recording.systemAudio, "system audio default");
    CheckTrue(!recording.microphone, "microphone default");
    CheckTrue(recording.audioBitrate == core::RecordingAudioBitrate::K192, "audio bitrate default");

    CheckEqual(recording.FramesPerSecond(), 30, "derived frames per second");
    CheckEqual(recording.CountdownSeconds(), 3, "derived countdown seconds");
    CheckEqual(recording.AudioBitrateBitsPerSec(), 192000, "derived audio bitrate");
    CheckTrue(recording.WantsAudio(), "wants audio with system audio on");
}

void TestEmptyAndBrokenDocuments() {
    Parsed empty = Parse("");
    CheckText(empty.report.parseError, "empty", "empty file reports empty");
    CheckTrue(empty.report.UsedDefaults(), "empty file uses defaults");
    CheckHotkey(empty.settings.radialHotkey, KeySymbol::D, core::kModControl | core::kModAlt,
                "empty file keeps the default hotkey");

    Parsed broken = Parse("{\"menuSize\":");
    CheckText(broken.report.parseError, "unterminated", "broken file reports its error kind");
    CheckTrue(broken.settings.menuSize == core::MenuSize::Medium, "broken file keeps defaults");

    // 空对象是一份合法文档：没有错误报告，值全是默认值。
    Parsed object = Parse("{}");
    CheckTrue(object.report.parseError.empty(), "empty object has no parse error");
    CheckEqual(object.report.schemaVersion, 0, "empty object has no schema version");
    CheckEmpty(object.report.repairedKeys, "empty object repairs nothing");
    CheckEmpty(object.report.migratedKeys, "empty object migrates nothing");
    CheckEmpty(object.report.ignoredKeys, "empty object ignores nothing");
    CheckEmpty(object.report.droppedKeys, "empty object drops nothing");
}

void TestLegacyDocument() {
    Parsed parsed = Parse(kLegacyDocument);

    CheckTrue(parsed.report.parseError.empty(), "1.x document parses");
    CheckEqual(parsed.report.schemaVersion, 0, "1.x document has no schemaVersion");

    CheckHotkey(parsed.settings.radialHotkey, KeySymbol::F6, 0, "legacy radial hotkey");
    CheckHotkey(parsed.settings.screenshotHotkey, KeySymbol::F1, 0, "legacy screenshot hotkey");
    CheckHotkey(parsed.settings.ocrHotkey, KeySymbol::F7, 0, "legacy ocr hotkey");
    CheckHotkey(parsed.settings.recordHotkey, KeySymbol::F8, 0, "legacy record hotkey");
    CheckHotkey(parsed.settings.pinHotkey, KeySymbol::F3, 0, "legacy pin hotkey");

    CheckTrue(parsed.settings.screenshotFormat == core::ScreenshotFormat::Png, "legacy format");
    CheckTrue(parsed.settings.menuSize == core::MenuSize::Small, "legacy menu size");
    CheckTrue(parsed.settings.recordingDirectory.empty(), "legacy recording directory");
    CheckTrue(parsed.settings.ocr.engine == core::OcrEngine::Model, "legacy ocr engine");
    CheckTrue(parsed.settings.recording.quality == core::RecordingQuality::Balanced, "legacy quality");
    CheckTrue(parsed.settings.recording.countdown == core::RecordingCountdown::ThreeSeconds,
              "legacy countdown");
    CheckTrue(parsed.settings.recording.audioBitrate == core::RecordingAudioBitrate::K192,
              "legacy audio bitrate");

    CheckList(parsed.report.migratedKeys,
              { "hotkey", "screenshotHotkey", "ocrHotkey", "recordHotkey", "pinHotkey" },
              "legacy hotkeys are reported as migrated");
    // 1.x 把四个只读计算属性也写进了文件；它们不是「不认识的键」，但也不是需要读的键。
    CheckList(parsed.report.ignoredKeys,
              { "recording.framesPerSecond", "recording.countdownSeconds",
                "recording.audioBitrateValue", "recording.wantsAudio" },
              "legacy derived fields are reported as ignored");
    CheckEmpty(parsed.report.repairedKeys, "legacy document repairs nothing");
    CheckEmpty(parsed.report.droppedKeys, "legacy document drops nothing");
}

void TestRoundTrip() {
    AppSettings custom;
    custom.radialHotkey = { KeySymbol::F13, 0 };
    custom.screenshotHotkey = { KeySymbol::K, core::kModControl | core::kModShift };
    custom.ocrHotkey = { KeySymbol::O, core::kModShift };
    custom.recordHotkey = { KeySymbol::R, core::kModAlt | core::kModWin };
    custom.pinHotkey = { KeySymbol::PrintScreen, 0 };
    custom.screenshotFormat = core::ScreenshotFormat::Jpeg;
    custom.menuSize = core::MenuSize::Large;
    custom.recordingDirectory = "D:\\Captures\\截图";
    custom.convertFilePathsToImages = false;
    custom.pinGlowEnabled = false;
    custom.ocr.engine = core::OcrEngine::Model;
    custom.ocr.autoCopy = true;
    custom.ocr.pinOcrEnabled = false;
    custom.recording.resolution = core::RecordingResolution::P1440;
    custom.recording.frameRate = core::RecordingFrameRate::Sixty;
    custom.recording.quality = core::RecordingQuality::High;
    custom.recording.recordCursor = false;
    custom.recording.countdown = core::RecordingCountdown::Off;
    custom.recording.range = core::RecordingRange::Window;
    custom.recording.systemAudio = false;
    custom.recording.microphone = true;
    custom.recording.audioBitrate = core::RecordingAudioBitrate::K128;

    const std::string text = core::SerializeSettings(custom);

    // 2.0 写出的是规范文本热键，不是 1.x 的两字段对象。
    CheckTrue(text.find("\"hotkey\": \"F13\"") != std::string::npos, "bare key is written as text");
    CheckTrue(text.find("\"screenshotHotkey\": \"Ctrl+Shift+K\"") != std::string::npos,
              "modifiers are written in canonical order");
    CheckTrue(text.find("\"schemaVersion\": 2") != std::string::npos, "2.0 writes schemaVersion 2");
    CheckTrue(text.find("\"menuSize\": \"Large\"") != std::string::npos, "enum is written by member name");
    CheckTrue(text.find("  \"hotkey\"") != std::string::npos, "two-space indentation");

    Parsed parsed = Parse(text);
    CheckTrue(parsed.report.parseError.empty(), "our own document parses");
    CheckEqual(parsed.report.schemaVersion, core::kSettingsSchemaVersion, "schema version round-trips");
    CheckTrue(parsed.settings == custom, "settings round-trip through text");
    CheckEmpty(parsed.report.repairedKeys, "round-trip repairs nothing");
    CheckEmpty(parsed.report.migratedKeys, "round-trip migrates nothing");
    CheckEmpty(parsed.report.ignoredKeys, "round-trip ignores nothing");
    CheckEmpty(parsed.report.droppedKeys, "round-trip drops nothing");

    // 默认值也要能整份走一圈。
    const AppSettings defaults;
    Parsed defaultRoundTrip = Parse(core::SerializeSettings(defaults));
    CheckTrue(defaultRoundTrip.settings == defaults, "default settings round-trip");
    CheckTrue(defaultRoundTrip.report.parseError.empty(), "default settings parse cleanly");
}

void TestEnumNames() {
    // 每个合法值都要有非空名字，并且名字能读回同一个值——读写共用一张表才不会自相矛盾。
    const core::ScreenshotFormat formats[] = { core::ScreenshotFormat::Png, core::ScreenshotFormat::Jpeg };
    for (const auto value : formats) {
        AppSettings settings;
        settings.screenshotFormat = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.screenshotFormat == value, "screenshot format name round-trips");
        CheckTrue(core::ScreenshotFormatName(value)[0] != '\0', "screenshot format has a name");
    }

    const core::MenuSize sizes[] = { core::MenuSize::Small, core::MenuSize::Medium, core::MenuSize::Large };
    for (const auto value : sizes) {
        AppSettings settings;
        settings.menuSize = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.menuSize == value, "menu size name round-trips");
        CheckTrue(core::MenuSizeName(value)[0] != '\0', "menu size has a name");
    }

    const core::OcrEngine engines[] = { core::OcrEngine::Native, core::OcrEngine::Model };
    for (const auto value : engines) {
        AppSettings settings;
        settings.ocr.engine = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.ocr.engine == value, "ocr engine name round-trips");
        CheckTrue(core::OcrEngineName(value)[0] != '\0', "ocr engine has a name");
    }

    const core::RecordingResolution resolutions[] = {
        core::RecordingResolution::Original, core::RecordingResolution::P2160,
        core::RecordingResolution::P1440, core::RecordingResolution::P1080,
        core::RecordingResolution::P720
    };
    for (const auto value : resolutions) {
        AppSettings settings;
        settings.recording.resolution = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.resolution == value, "resolution name round-trips");
        CheckTrue(core::ResolutionName(value)[0] != '\0', "resolution has a name");
    }

    const core::RecordingFrameRate frameRates[] = { core::RecordingFrameRate::Thirty,
                                                    core::RecordingFrameRate::Sixty };
    for (const auto value : frameRates) {
        AppSettings settings;
        settings.recording.frameRate = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.frameRate == value, "frame rate name round-trips");
        CheckTrue(core::FrameRateName(value)[0] != '\0', "frame rate has a name");
    }

    const core::RecordingQuality qualities[] = { core::RecordingQuality::High,
                                                 core::RecordingQuality::Balanced,
                                                 core::RecordingQuality::Compact };
    for (const auto value : qualities) {
        AppSettings settings;
        settings.recording.quality = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.quality == value, "quality name round-trips");
        CheckTrue(core::QualityName(value)[0] != '\0', "quality has a name");
    }

    const core::RecordingCountdown countdowns[] = { core::RecordingCountdown::Off,
                                                    core::RecordingCountdown::ThreeSeconds,
                                                    core::RecordingCountdown::FiveSeconds };
    for (const auto value : countdowns) {
        AppSettings settings;
        settings.recording.countdown = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.countdown == value, "countdown name round-trips");
        CheckTrue(core::CountdownName(value)[0] != '\0', "countdown has a name");
    }

    const core::RecordingRange ranges[] = { core::RecordingRange::Region, core::RecordingRange::Window };
    for (const auto value : ranges) {
        AppSettings settings;
        settings.recording.range = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.range == value, "range name round-trips");
        CheckTrue(core::RangeName(value)[0] != '\0', "range has a name");
    }

    const core::RecordingAudioBitrate bitrates[] = { core::RecordingAudioBitrate::K128,
                                                     core::RecordingAudioBitrate::K192 };
    for (const auto value : bitrates) {
        AppSettings settings;
        settings.recording.audioBitrate = value;
        Parsed parsed = Parse(core::SerializeSettings(settings));
        CheckTrue(parsed.settings.recording.audioBitrate == value, "audio bitrate name round-trips");
        CheckTrue(core::AudioBitrateName(value)[0] != '\0', "audio bitrate has a name");
    }
}

void TestRepairs() {
    // 值非法 → 逐键回默认值并登记，整份文件不丢（与 1.x 的「一个键坏掉就整份退回默认」不同）。
    Parsed enumName = Parse(R"({"screenshotFormat":"Bmp","menuSize":"Huge","ocr":{"engine":"Tesseract"}})");
    CheckList(enumName.report.repairedKeys, { "screenshotFormat", "menuSize", "ocr.engine" },
              "bad enum names are repaired in read order");
    CheckTrue(enumName.settings.screenshotFormat == core::ScreenshotFormat::Png, "format falls back");
    CheckTrue(enumName.settings.menuSize == core::MenuSize::Medium, "menu size falls back");
    CheckTrue(enumName.settings.ocr.engine == core::OcrEngine::Native, "ocr engine falls back");

    // 1.x 的序列化器也允许整数形式的枚举，早期文件里出现过。
    Parsed enumNumber = Parse(R"({"screenshotFormat":1,"menuSize":2,"recording":{"frameRate":60}})");
    CheckTrue(enumNumber.report.repairedKeys.empty(), "integer enums are accepted");
    CheckTrue(enumNumber.settings.screenshotFormat == core::ScreenshotFormat::Jpeg, "integer format");
    CheckTrue(enumNumber.settings.menuSize == core::MenuSize::Large, "integer menu size");
    CheckTrue(enumNumber.settings.recording.frameRate == core::RecordingFrameRate::Sixty,
              "integer frame rate");

    Parsed badNumber = Parse(R"({"menuSize":1.5})");
    CheckList(badNumber.report.repairedKeys, { "menuSize" }, "fractional enum is repaired");
    CheckTrue(badNumber.settings.menuSize == core::MenuSize::Medium, "fractional enum stays default");

    Parsed badBool = Parse(R"({"convertFilePathsToImages":"yes","pinGlowEnabled":2})");
    CheckList(badBool.report.repairedKeys, { "convertFilePathsToImages", "pinGlowEnabled" },
              "non-boolean booleans are repaired");
    CheckTrue(badBool.settings.convertFilePathsToImages, "bad boolean stays default");
    CheckTrue(badBool.settings.pinGlowEnabled, "bad boolean stays default (second)");

    // 手工编辑的人会用 1/0 写布尔；这是宽容而不是猜。
    Parsed numericBool = Parse(R"({"convertFilePathsToImages":0,"pinGlowEnabled":1})");
    CheckTrue(numericBool.report.repairedKeys.empty(), "numeric booleans are accepted");
    CheckTrue(!numericBool.settings.convertFilePathsToImages, "0 means false");
    CheckTrue(numericBool.settings.pinGlowEnabled, "1 means true");

    Parsed badString = Parse(R"({"recordingDirectory":5})");
    CheckList(badString.report.repairedKeys, { "recordingDirectory" }, "non-string path is repaired");
    CheckTrue(badString.settings.recordingDirectory.empty(), "bad path stays empty");

    Parsed badVersion = Parse(R"({"schemaVersion":"2"})");
    CheckList(badVersion.report.repairedKeys, { "schemaVersion" }, "non-integer schemaVersion is repaired");
    CheckEqual(badVersion.report.schemaVersion, 0, "repaired schemaVersion reads as unset");
}

void TestHotkeyParsing() {
    // 2.0 的规范文本：自己写出去的必须能读回来。
    Parsed text = Parse(R"({"hotkey":"Ctrl+Alt+K"})");
    CheckHotkey(text.settings.radialHotkey, KeySymbol::K, core::kModControl | core::kModAlt,
                "text hotkey parses");
    CheckEmpty(text.report.migratedKeys, "text hotkey is not a migration");

    // 1.x 的显示文本里带空格（"Ctrl + Alt + K"），宽容接受。
    Parsed spaced = Parse(R"({"hotkey":"Ctrl + Alt + K"})");
    CheckHotkey(spaced.settings.radialHotkey, KeySymbol::K, core::kModControl | core::kModAlt,
                "spaced text hotkey parses");

    // 单键热键：功能键、字母、主键盘数字、翻页类可以裸用。
    Parsed bare = Parse(R"({"hotkey":"F13"})");
    CheckHotkey(bare.settings.radialHotkey, KeySymbol::F13, 0, "bare function key is valid");
    CheckEmpty(bare.report.repairedKeys, "bare function key is not repaired");

    // 空格裸用会被系统独占日常输入，必须拒。
    Parsed unsafe = Parse(R"({"hotkey":"Space"})");
    CheckList(unsafe.report.repairedKeys, { "hotkey" }, "bare space is repaired");
    CheckHotkey(unsafe.settings.radialHotkey, KeySymbol::D, core::kModControl | core::kModAlt,
                "unsafe bare key keeps the default");

    Parsed unknown = Parse(R"({"hotkey":"LeftCtrl+F6"})");
    CheckList(unknown.report.repairedKeys, { "hotkey" }, "modifier key as a symbol is repaired");

    // 1.x 的两字段对象：修饰键是 flags 的字符串形式或整数。
    Parsed legacy = Parse(R"({"hotkey":{"modifiers":"Control, Alt","key":"K"}})");
    CheckHotkey(legacy.settings.radialHotkey, KeySymbol::K, core::kModControl | core::kModAlt,
                "legacy modifier names parse");
    CheckList(legacy.report.migratedKeys, { "hotkey" }, "legacy hotkey is reported as migrated");

    Parsed legacyInt = Parse(R"({"hotkey":{"modifiers":3,"key":"K"}})");
    CheckHotkey(legacyInt.settings.radialHotkey, KeySymbol::K, core::kModControl | core::kModAlt,
                "legacy integer modifiers 3 = Alt|Control");

    Parsed noModifiers = Parse(R"({"hotkey":{"key":"F1"}})");
    CheckHotkey(noModifiers.settings.radialHotkey, KeySymbol::F1, 0, "missing modifiers mean none");

    // WPF 的 Key 成员名与 2.0 的符号名不总相同，别名表要能翻。
    Parsed snapshot = Parse(R"({"hotkey":{"key":"Snapshot","modifiers":"None"}})");
    CheckHotkey(snapshot.settings.radialHotkey, KeySymbol::PrintScreen, 0, "Snapshot aliases PrintScreen");

    Parsed back = Parse(R"({"hotkey":{"key":"Back","modifiers":"Alt"}})");
    CheckHotkey(back.settings.radialHotkey, KeySymbol::Backspace, core::kModAlt,
                "Back aliases Backspace");

    // 小键盘数字与 CapsLock 在 2.0 的符号表里没有位置：认不出来就回默认，不硬编相近的键。
    Parsed numpad = Parse(R"({"hotkey":{"key":"NumPad0","modifiers":"None"}})");
    CheckList(numpad.report.repairedKeys, { "hotkey" }, "NumPad0 cannot migrate");
    CheckHotkey(numpad.settings.radialHotkey, KeySymbol::D, core::kModControl | core::kModAlt,
                "unmigratable key keeps the default");

    Parsed badModifiers = Parse(R"({"hotkey":{"key":"F6","modifiers":99}})");
    CheckList(badModifiers.report.repairedKeys, { "hotkey" }, "out-of-range modifiers are repaired");

    Parsed missingKey = Parse(R"({"hotkey":{"modifiers":"Alt"}})");
    CheckList(missingKey.report.repairedKeys, { "hotkey" }, "hotkey object without a key is repaired");

    Parsed noKey = Parse(R"({"hotkey":{"key":"None","modifiers":"None"}})");
    CheckList(noKey.report.repairedKeys, { "hotkey" }, "KeySymbol::None is not a hotkey");
}

void TestUnrecognizedKeys() {
    Parsed dropped = Parse(R"({"menuSize":"Medium","mystery":1,"ocr":{"engine":"Model","threshold":5}})");
    CheckList(dropped.report.droppedKeys, { "mystery", "ocr.threshold" },
              "unknown leaves are reported as dropped");
    CheckEmpty(dropped.report.repairedKeys, "dropping an unknown key is not a repair");

    // 空对象没有叶子可报，报这一层就够——否则每次导入都会多出一条没有信息量的行。
    Parsed emptyObject = Parse(R"({"mystery":{}})");
    CheckList(emptyObject.report.droppedKeys, { "mystery" }, "empty unknown object is one report");

    Parsed nestedObject = Parse(R"({"mystery":{"deep":1}})");
    CheckList(nestedObject.report.droppedKeys, { "mystery.deep" }, "unknown object is drilled into");

    Parsed ignored = Parse(R"({"recording":{"framesPerSecond":30,"countdownSeconds":3,
                                     "audioBitrateValue":192000,"wantsAudio":true}})");
    CheckList(ignored.report.ignoredKeys,
              { "recording.framesPerSecond", "recording.countdownSeconds",
                "recording.audioBitrateValue", "recording.wantsAudio" },
              "derived fields are ignored, not dropped");
    CheckEmpty(ignored.report.droppedKeys, "derived fields are not dropped");
}

void TestSingleKeyRules() {
    CheckTrue(core::IsSafeSingleKey(KeySymbol::F1), "F1 is a safe single key");
    CheckTrue(core::IsSafeSingleKey(KeySymbol::F24), "F24 is a safe single key");
    CheckTrue(core::IsSafeSingleKey(KeySymbol::A), "A is a safe single key");
    CheckTrue(core::IsSafeSingleKey(KeySymbol::Num5), "Num5 is a safe single key");
    CheckTrue(core::IsSafeSingleKey(KeySymbol::Insert), "Insert is a safe single key");
    CheckTrue(core::IsSafeSingleKey(KeySymbol::PrintScreen), "PrintScreen is a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::Space), "Space is not a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::Enter), "Enter is not a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::Escape), "Escape is not a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::Delete), "Delete is not a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::Up), "Up is not a safe single key");
    CheckTrue(!core::IsSafeSingleKey(KeySymbol::None), "None is not a key");

    HotkeyCheck none = core::CheckHotkey({ KeySymbol::None, 0 });
    CheckTrue(!none.valid, "empty hotkey is invalid");
    CheckText(none.reason, "none", "empty hotkey reason");

    HotkeyCheck unsafe = core::CheckHotkey({ KeySymbol::Space, 0 });
    CheckTrue(!unsafe.valid, "bare space is invalid");
    CheckText(unsafe.reason, "unsafe_single_key", "bare space reason");

    HotkeyCheck badBit = core::CheckHotkey({ KeySymbol::K, 0x100u });
    CheckTrue(!badBit.valid, "undefined modifier bit is invalid");
    CheckText(badBit.reason, "bad_modifier", "undefined modifier reason");

    CheckTrue(core::CheckHotkey({ KeySymbol::Space, core::kModControl }).valid,
              "modified space is valid");
    CheckTrue(core::CheckHotkey({ KeySymbol::F13, 0 }).valid, "bare F13 is valid");
    // NoRepeat 只影响注册，不参与「裸键」判断——它自己就是唯一的修饰位，所以不受单键限制。
    CheckTrue(core::CheckHotkey({ KeySymbol::F13, core::kModNoRepeat }).valid,
              "NoRepeat alone is accepted");
}

void TestHotkeySlotsAndConflicts() {
    AppSettings settings;
    CheckEqual(core::kHotkeySlotCount, 5, "five hotkey slots");
    CheckTrue(core::HotkeySlot(settings, 0) == &settings.radialHotkey, "slot 0 is radial");
    CheckTrue(core::HotkeySlot(settings, 1) == &settings.screenshotHotkey, "slot 1 is screenshot");
    CheckTrue(core::HotkeySlot(settings, 2) == &settings.ocrHotkey, "slot 2 is ocr");
    CheckTrue(core::HotkeySlot(settings, 3) == &settings.recordHotkey, "slot 3 is record");
    CheckTrue(core::HotkeySlot(settings, 4) == &settings.pinHotkey, "slot 4 is pin");
    CheckTrue(core::HotkeySlot(settings, -1) == nullptr, "capture idle slot is out of range");
    CheckTrue(core::HotkeySlot(settings, 5) == nullptr, "slot past the end is out of range");

    const AppSettings& readOnly = settings;
    CheckTrue(core::HotkeySlot(readOnly, 2) == &settings.ocrHotkey, "const overload picks the same slot");
    CheckTrue(core::HotkeySlot(readOnly, -1) == nullptr, "const overload rejects out of range");

    *core::HotkeySlot(settings, 2) = HotkeySpec{ KeySymbol::K, core::kModControl };
    CheckTrue(settings.ocrHotkey == HotkeySpec(KeySymbol::K, core::kModControl),
              "mutable slot writes through");

    // 默认五把互不相同。
    AppSettings defaults;
    for (int slot = 0; slot < core::kHotkeySlotCount; ++slot) {
        CheckEqual(core::FindHotkeyConflict(defaults, slot), -1, "default hotkeys do not collide");
    }

    // 把截图改成与光圈同一把：两个方向都要报出对方。
    defaults.screenshotHotkey = defaults.radialHotkey;
    CheckEqual(core::FindHotkeyConflict(defaults, 0), 1, "radial reports screenshot");
    CheckEqual(core::FindHotkeyConflict(defaults, 1), 0, "screenshot reports radial");
    CheckEqual(core::FindHotkeyConflict(defaults, 2), -1, "unrelated slot stays clean");

    // 第一个冲突按槽位序返回，不是按「离得最近」。
    defaults.ocrHotkey = defaults.radialHotkey;
    CheckEqual(core::FindHotkeyConflict(defaults, 0), 1, "lowest conflicting slot wins");

    // 同键不同修饰键不算冲突。
    AppSettings modifiers;
    modifiers.radialHotkey = HotkeySpec{ KeySymbol::D, core::kModControl | core::kModAlt };
    modifiers.screenshotHotkey = HotkeySpec{ KeySymbol::D, core::kModControl };
    CheckEqual(core::FindHotkeyConflict(modifiers, 0), -1, "extra modifier is a different hotkey");
    CheckEqual(core::FindHotkeyConflict(modifiers, 1), -1, "missing modifier is a different hotkey");

    // 只差 NoRepeat 仍然是同一把物理键：注册第二次必然失败，所以要报冲突。
    AppSettings noRepeat;
    noRepeat.radialHotkey = HotkeySpec{ KeySymbol::F13, 0 };
    noRepeat.pinHotkey = HotkeySpec{ KeySymbol::F13, core::kModNoRepeat };
    CheckEqual(core::FindHotkeyConflict(noRepeat, 0), 4, "NoRepeat does not hide a collision");
    CheckEqual(core::FindHotkeyConflict(noRepeat, 4), 0, "NoRepeat collision is symmetric");

    // 未设置的槽位不参与比对，否则两把空热键会互相报冲突。
    AppSettings empty;
    empty.radialHotkey = HotkeySpec{};
    empty.screenshotHotkey = HotkeySpec{};
    CheckEqual(core::FindHotkeyConflict(empty, 0), -1, "unset hotkey cannot collide");
    CheckEqual(core::FindHotkeyConflict(empty, 1), -1, "unset hotkey cannot be collided with");
    CheckEqual(core::FindHotkeyConflict(empty, -1), -1, "out of range slot has no conflict");
    CheckEqual(core::FindHotkeyConflict(empty, 9), -1, "past the end slot has no conflict");
}

void TestResolveOutputSize() {
    core::RecordingSettings original;
    CheckEqual(core::ResolveOutputSize(original, 2560, 1440).widthPx, 2560, "original keeps width");
    CheckEqual(core::ResolveOutputSize(original, 2560, 1440).heightPx, 1440, "original keeps height");
    CheckEqual(core::ResolveOutputSize(original, 2559, 1439).widthPx, 2558, "original rounds width down");
    CheckEqual(core::ResolveOutputSize(original, 2559, 1439).heightPx, 1438, "original rounds height down");
    CheckEqual(core::ResolveOutputSize(original, 0, 1440).widthPx, 0, "zero source width is degenerate");
    CheckEqual(core::ResolveOutputSize(original, 2560, -1).heightPx, 0, "negative source height is degenerate");

    core::RecordingSettings p720;
    p720.resolution = core::RecordingResolution::P720;
    CheckEqual(core::ResolveOutputSize(p720, 3840, 2160).widthPx, 1280, "4K to 720p width");
    CheckEqual(core::ResolveOutputSize(p720, 3840, 2160).heightPx, 720, "4K to 720p height");
    CheckEqual(core::ResolveOutputSize(p720, 2560, 1440).widthPx, 1280, "1440p to 720p width");
    CheckEqual(core::ResolveOutputSize(p720, 1280, 720).widthPx, 1280, "720p source is not upscaled");
    CheckEqual(core::ResolveOutputSize(p720, 1280, 719).heightPx, 718, "a shorter source shrinks to even");

    // 3×864 → 720p：3*720/864 = 2.5，Math.Round 的「到偶数」给出 2，不是 3。
    CheckEqual(core::ResolveOutputSize(p720, 3, 864).widthPx, 2, "half-even rounds 2.5 down to 2");
    CheckEqual(core::ResolveOutputSize(p720, 3, 864).heightPx, 720, "clamped height stays even");
    // 6×864 → 720p：6*720/864 正好是 5，一步取整没有半数可依；编码器拒收奇数宽度，
    // 所以这里必须再取一次偶（5 → 4）。1.x 在这一格会给 5，是两版唯一故意不同的地方。
    CheckEqual(core::ResolveOutputSize(p720, 6, 864).widthPx, 4, "odd exact quotient is made even");
    CheckEqual(core::ResolveOutputSize(p720, 5, 3).widthPx, 4, "tiny source keeps even width");
    CheckEqual(core::ResolveOutputSize(p720, 5, 3).heightPx, 2, "tiny source keeps even height");

    core::RecordingSettings p2160;
    p2160.resolution = core::RecordingResolution::P2160;
    CheckEqual(core::ResolveOutputSize(p2160, 1920, 1080).widthPx, 1920, "1080p is not upscaled to 2160p");
    CheckEqual(core::ResolveOutputSize(p2160, 1920, 1080).heightPx, 1080, "1080p height is untouched");
}

void TestBitrate() {
    core::RecordingSettings high;
    high.quality = core::RecordingQuality::High;
    core::RecordingSettings balanced;
    core::RecordingSettings compact;
    compact.quality = core::RecordingQuality::Compact;

    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 3840, 2160), 27371520,
               "4K30 balanced = 0.11 bpp");
    // 与编码 seam 的默认码率必须是同一条公式，否则两份实现会各自漂移。
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 3840, 2160),
               core::DefaultBitrateBitsPerSec(3840, 2160, 30), "matches the encoder default bitrate");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(high, 3840, 2160), 45000000,
               "4K30 high is clamped at the ceiling");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(compact, 3840, 2160), 13685760,
               "4K30 compact = 0.055 bpp");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 640, 360), 1000000,
               "tiny selection is clamped at the floor");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 0, 0), 1000000,
               "degenerate size uses the floor");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 1280, 720), 3041280,
               "720p30 balanced");

    balanced.frameRate = core::RecordingFrameRate::Sixty;
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 1280, 720), 6082560,
               "720p60 balanced is twice the 30fps value");
    CheckEqual(core::ComputeVideoBitrateBitsPerSec(balanced, 3840, 2160), 45000000,
               "4K60 balanced clamps at the ceiling");
}

void TestSettingsEquality() {
    AppSettings a;
    AppSettings b;
    CheckTrue(a == b, "default settings compare equal");

    b.menuSize = core::MenuSize::Large;
    CheckTrue(!(a == b), "a changed menu size breaks equality");

    b = a;
    b.recording.microphone = true;
    CheckTrue(!(a == b), "a changed recording field breaks equality");

    b = a;
    b.ocr.autoCopy = true;
    CheckTrue(!(a == b), "a changed ocr field breaks equality");

    b = a;
    b.recordingDirectory = "D:\\Captures";
    CheckTrue(!(a == b), "a changed path breaks equality");
}

}

int main() {
    TestConfigTreeBasics();
    TestConfigTreeErrors();
    TestStringEscapes();
    TestDefaults();
    TestEmptyAndBrokenDocuments();
    TestLegacyDocument();
    TestRoundTrip();
    TestEnumNames();
    TestRepairs();
    TestHotkeyParsing();
    TestUnrecognizedKeys();
    TestSingleKeyRules();
    TestHotkeySlotsAndConflicts();
    TestResolveOutputSize();
    TestBitrate();
    TestSettingsEquality();

    if (failures != 0) {
        std::printf("SETTINGS_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("SETTINGS_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
