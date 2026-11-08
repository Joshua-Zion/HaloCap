#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core::clipboard {

// 剪贴板贴图（1.x 的 ClipboardPinService 搬到 core 的纯逻辑部分）：
// 颜色识别、CF_HTML 取可见文本、快照优先级、文件贴图的批次/粘贴序列规则。
// 位图解码、文件读取与卡片渲染在平台侧（需要 WIC/GDI），这里只处理字符串与决策。

struct Color {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;

    bool operator==(const Color& other) const {
        return r == other.r && g == other.g && b == other.b && a == other.a;
    }
};

// CSS 风格：#rgb / #rgba / #rrggbb / #rrggbbaa（alpha 在末位），或
// rgb/rgba 前缀 + 三个分量（0..255 整数或 0..1 小数，逗号/空白/中文逗号分隔）。
bool ParseColor(std::string_view text, Color* color);

// CF_HTML → 可见文本：优先 <!--StartFragment--> 标记，其次 StartFragment/EndFragment
// 的 UTF-8 字节偏移；去掉脚本/样式/head/iframe/object 与注释，块级标签转换行，
// 单元格转换行内制表符，解 HTML 实体。
std::string HtmlToText(std::string_view html);

// 一次剪贴板读取的原始形状（平台填）。优先级：文件 > 位图 > HTML > 纯文本。
struct SnapshotInput {
    std::uint32_t sequence = 0;
    std::vector<std::string> filePaths;
    bool hasImage = false;
    bool hasHtml = false;
    std::string html;
    bool hasText = false;
    std::string text;
};

struct Snapshot {
    std::uint32_t sequence = 0;
    std::vector<std::string> filePaths;
    bool hasImage = false;
    std::string text;

    bool HasContent() const;
};

Snapshot NormalizeSnapshot(const SnapshotInput& input);

enum class PinKind {
    Image,
    Color,
    Text,
    FileImage,
    FilePath,
};

struct PinPlan {
    PinKind kind = PinKind::Text;
    std::string text;          // 颜色/文本/文件路径（FileImage 时为源路径）
    bool hasImage = false;     // Image / FileImage
    std::uint32_t sequence = 0;
};

struct PinOptions {
    bool convertFilePathsToImages = true;
    int maxFileImagePins = 16;
};

// 只按字符串判断「像不像本机绝对路径」：`C:\...` / `C:/...`。UNC、相对路径、
// 网络盘与重解析点由平台在读取时再拒（这里不碰文件系统）。
bool LooksLikeLocalAbsolutePath(std::string_view utf8Path);

class PinPlanner {
public:
    // 文件贴图：对每个候选调用 loadImage；成功且未超上限 → FileImage，其余回退到
    // 一条 FilePath（多路径用 CRLF 连接）。粘贴序列被消耗时整批回退。
    std::vector<PinPlan> PlanFiles(const Snapshot& snapshot, const PinOptions& options,
                                   const std::function<bool(const std::string&)>& loadImage) const;

    // 规划非文件快照：位图 → Image；可解析颜色 → Color；否则 Text。
    std::vector<PinPlan> PlanContent(const Snapshot& snapshot) const;

    // 只有真正的 FileImage 粘贴会消耗当前序列；其它 kind 不消耗（1.x 的行为）。
    void MarkPasted(const PinPlan& plan);

    bool FileImagePasteConsumed(std::uint32_t sequence) const;

private:
    std::uint32_t pastedSequence_ = 0;
    bool hasPastedSequence_ = false;
};

// 渲染卡片的尺寸规则（与 1.x 的 RenderText 同口径，平台按这个开画布）。
struct TextCardMetrics {
    int width = 160;
    int height = 56;
};

TextCardMetrics TextCardBounds(std::string_view text, double scale);

}
