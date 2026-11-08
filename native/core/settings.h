#pragma once

#include "input_host.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 8 个 seam 里 IStore 的 core 半边：设置文件的内容。
//
// 平台侧（win32_store）只管「一段 UTF-8 文本从一个位置原子地换进另一个位置」，
// 形状、键名、默认值、修复规则全在这里。这条分工决定了 1.x 的 settings.json 能被 2.0
// 直接读，也决定了以后换平台不用重写一遍设置。
//
// JSON 只支持 1.x 真的写得出来的那一子集：对象、字符串、整数、布尔、null。
// 没有数组——1.x 的设置模型里没有一个数组字段，为了「以后可能有」实现一遍数组，
// 代价是解析器、序列化器和测试各多一条从没跑过的分支。

// ---- 设置文档（一棵扁平的树） ----
//
// 节点放在一个 vector 里、用下标互相引用，而不是递归持有的值：解析器因此不需要递归
// 持有子对象，深度也在读的时候就被卡住。手工编辑过的配置文件不该能让进程栈溢出。
enum class ValueKind : std::uint8_t {
    Null = 0,
    Boolean = 1,
    Number = 2,
    String = 3,
    Object = 4,
};

struct ConfigValue {
    ValueKind kind = ValueKind::Null;
    bool boolean = false;
    std::int64_t number = 0;

    // 数字带小数或指数时置真：3.5 是语法正确、类型不对的输入，要能跟 3 区分开。
    bool numberFractional = false;

    std::string text;

    // 仅对象使用，按下标指向同一棵树里的节点；顺序就是文件里的顺序。
    std::vector<std::string> memberNames;
    std::vector<std::size_t> memberValues;
};

class ConfigTree {
public:
    // 根节点恒为对象，构造好放在 0 号位，所以空文档也有一棵合法的树。
    static constexpr std::size_t kRoot = 0;

    // 解析阶段允许的最大嵌套层数（根算第 1 层）。当前 schema 最深 3 层。
    static constexpr int kMaxDepth = 16;

    ConfigTree();

    std::size_t AddMember(std::size_t parent, std::string_view name, const ConfigValue& value);

    // 点分路径，比如 "recording.frameRate"。重名成员取最后一个（与 1.x 的反序列化一致）。
    const ConfigValue* Find(std::string_view dottedPath) const;
    const ConfigValue* FindMember(std::size_t node, std::string_view name) const;

    const ConfigValue& At(std::size_t node) const { return values_[node]; }

    // 读失败时整份文档作废（调用方退回默认值），所以错误只要「够说清」就行：
    // kind 是稳定的短词，offset 指向出问题的字节，供 store_probe 打印。
    bool Parse(std::string_view utf8);
    const std::string& ErrorKind() const { return errorKind_; }
    std::size_t ErrorOffset() const { return errorOffset_; }

    // 两个空格缩进、键序与插入序一致；非 ASCII 原样写出（1.x 会转义成 \uXXXX，
    // 读侧两种都吃，所以这只是输出风格的差异）。
    std::string Serialize() const;

private:
    // 只留一个合法的空根，错误码不动。
    void ResetRoot();

    std::vector<ConfigValue> values_;
    std::string errorKind_;
    std::size_t errorOffset_ = 0;
};

// ---- 设置模型：键名、默认值与 1.x 逐项对齐 ----
//
// 磁盘上的名字用的是 1.x 序列化器写出来的枚举成员名（Png / ThreeSeconds / K192），
// 不是我们自己的拼法——同一个用户目录里住过两份配置，读不懂就是「设置全丢了」。

enum class ScreenshotFormat : std::int32_t { Png = 0, Jpeg = 1 };
enum class MenuSize : std::int32_t { Small = 0, Medium = 1, Large = 2 };
enum class OcrEngine : std::int32_t { Native = 0, Model = 1 };
enum class RecordingResolution : std::int32_t { Original = 0, P2160 = 1, P1440 = 2, P1080 = 3, P720 = 4 };
enum class RecordingFrameRate : std::int32_t { Thirty = 30, Sixty = 60 };
enum class RecordingQuality : std::int32_t { High = 0, Balanced = 1, Compact = 2 };
enum class RecordingCountdown : std::int32_t { Off = 0, ThreeSeconds = 3, FiveSeconds = 5 };
enum class RecordingRange : std::int32_t { Region = 0, Window = 1 };
enum class RecordingAudioBitrate : std::int32_t { K128 = 128000, K192 = 192000 };

struct OcrSettings {
    OcrEngine engine = OcrEngine::Native;
    bool autoCopy = false;
    bool pinOcrEnabled = true;
};

struct RecordingSettings {
    RecordingResolution resolution = RecordingResolution::Original;
    RecordingFrameRate frameRate = RecordingFrameRate::Thirty;
    RecordingQuality quality = RecordingQuality::Balanced;
    bool recordCursor = true;
    RecordingCountdown countdown = RecordingCountdown::ThreeSeconds;
    RecordingRange range = RecordingRange::Region;
    bool systemAudio = true;
    bool microphone = false;
    RecordingAudioBitrate audioBitrate = RecordingAudioBitrate::K192;

    int FramesPerSecond() const;
    int CountdownSeconds() const;
    std::int64_t AudioBitrateBitsPerSec() const;
    bool WantsAudio() const { return systemAudio || microphone; }

    bool operator==(const RecordingSettings& other) const {
        return resolution == other.resolution && frameRate == other.frameRate &&
               quality == other.quality && recordCursor == other.recordCursor &&
               countdown == other.countdown && range == other.range &&
               systemAudio == other.systemAudio && microphone == other.microphone &&
               audioBitrate == other.audioBitrate;
    }
};

struct AppSettings {
    // 圆盘呼出键。1.x 存的是 {modifiers, key}，2.0 存规范文本，读侧两种都认。
    HotkeySpec radialHotkey{ KeySymbol::D, kModControl | kModAlt };
    HotkeySpec screenshotHotkey{ KeySymbol::A, kModControl | kModAlt };
    HotkeySpec ocrHotkey{ KeySymbol::O, kModControl | kModAlt };
    HotkeySpec recordHotkey{ KeySymbol::R, kModControl | kModAlt };
    HotkeySpec pinHotkey{ KeySymbol::P, kModControl | kModAlt };

    ScreenshotFormat screenshotFormat = ScreenshotFormat::Png;
    MenuSize menuSize = MenuSize::Medium;

    // 空串 = 没指定，由平台侧决定落在哪里（1.x 同义）。
    std::string recordingDirectory;

    bool convertFilePathsToImages = true;
    bool pinGlowEnabled = true;
    bool autoStart = false;

    OcrSettings ocr;
    RecordingSettings recording;

    bool operator==(const AppSettings& other) const {
        return radialHotkey == other.radialHotkey && screenshotHotkey == other.screenshotHotkey &&
               ocrHotkey == other.ocrHotkey && recordHotkey == other.recordHotkey &&
               pinHotkey == other.pinHotkey && screenshotFormat == other.screenshotFormat &&
               menuSize == other.menuSize && recordingDirectory == other.recordingDirectory &&
               convertFilePathsToImages == other.convertFilePathsToImages &&
               pinGlowEnabled == other.pinGlowEnabled && autoStart == other.autoStart && ocr.engine == other.ocr.engine &&
               ocr.autoCopy == other.ocr.autoCopy && ocr.pinOcrEnabled == other.ocr.pinOcrEnabled &&
               recording == other.recording;
    }
};

// 2.0 写出的文件带的版本字段；读的时候只拿它做诊断，形状由值本身决定（见文件头注释）。
constexpr std::int32_t kSettingsSchemaVersion = 2;

// 文本 <-> 模型。ParseSettings 先把 settings 换成默认值再按文件修，所以传进来的
// 旧值不会影响结果；整份文件读不进去时 settings 就停在默认值、report.parseError 非空
// （与 1.x「读不懂就用默认值、不崩」一致）。
struct SettingsReadReport {
    std::string parseError;              // 非空 = 整份文件没读进去
    std::size_t parseErrorOffset = 0;
    std::int32_t schemaVersion = 0;      // 0 = 文件里没写，也就是 1.x 写的文件

    std::vector<std::string> repairedKeys;   // 值非法，已换成默认值（1.x 在这种情况丢掉整个文件）
    std::vector<std::string> migratedKeys;   // 1.x 的两字段热键已折成文本
    std::vector<std::string> ignoredKeys;    // 1.x 写出的派生字段，读回时本来也用不到
    std::vector<std::string> droppedKeys;    // 这份 schema 里没有的键

    bool UsedDefaults() const { return !parseError.empty(); }
};

void ParseSettings(std::string_view utf8, AppSettings& settings, SettingsReadReport& report);
std::string SerializeSettings(const AppSettings& settings);

// ---- 热键合法性（1.x 的 HotkeyDefinition.IsValid，逐条对齐） ----
//
// 裸键会被系统独占，所以只放开功能键、字母、数字与翻页类按键：空格、回车、方向键
// 这种日常输入的键不许当裸键用。2.0 的符号表里没有「左 Ctrl」这类修饰键，
// 1.x 那条 !IsModifierKey 因此变成了结构上不可能出错——但迁移路径上仍然要拦，
// 因为 1.x 的文件里真的可能有。
struct HotkeyCheck {
    bool valid = false;
    const char* reason = "";   // "" / "none" / "bad_modifier" / "unsafe_single_key"
};

bool IsSafeSingleKey(KeySymbol key);
HotkeyCheck CheckHotkey(const HotkeySpec& spec);

// ---- 录制参数如何落到像素与码率（1.x 的 RecordingSettings，逐条对齐） ----

struct SizedOutput {
    int widthPx = 0;
    int heightPx = 0;
};

// 按目标高度等比下采样，不放大；输出宽高保持偶数（编码器要求，见 §5.1 的编码 seam）。
// 退化输入（源边小于 2）会得到 0，调用方按「不能录」处理。
SizedOutput ResolveOutputSize(const RecordingSettings& settings, int sourceWidthPx, int sourceHeightPx);

// 每像素比特数按画质档位取：High 0.20 / Balanced 0.11 / Compact 0.055，
// 结果夹在 [1 Mbit, 45 Mbit]。用千分比做整数乘法，与 1.x 的浮点结果逐个对齐过。
std::int64_t ComputeVideoBitrateBitsPerSec(const RecordingSettings& settings, int widthPx, int heightPx);

// 设置页与诊断页要显示的名字，与 1.x 的显示文本同源。
const char* ScreenshotFormatName(ScreenshotFormat value);
const char* MenuSizeName(MenuSize value);
const char* OcrEngineName(OcrEngine value);
const char* ResolutionName(RecordingResolution value);
const char* FrameRateName(RecordingFrameRate value);
const char* QualityName(RecordingQuality value);
const char* CountdownName(RecordingCountdown value);
const char* RangeName(RecordingRange value);
const char* AudioBitrateName(RecordingAudioBitrate value);

}
