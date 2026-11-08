#pragma once

#include "clipboard.h"

#include <cstdint>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

// 剪贴板读取与贴图素材渲染（1.x ClipboardPinService 的平台半边）。
// 文本/HTML/文件路径交给 core 决策；这里负责 DIB/PNG 解码、文件图片加载与卡片绘制。

struct ClipboardImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;   // 顶朝下
};

struct ClipboardReadResult {
    std::uint32_t sequence = 0;
    std::vector<std::string> filePaths;   // UTF-8
    bool hasImage = false;
    ClipboardImage image;
    bool hasHtml = false;
    std::string html;                     // UTF-8
    bool hasText = false;
    std::string text;                     // UTF-8
};

bool ReadClipboard(ClipboardReadResult* result);

// 读一张本地图片（PNG/JPEG/BMP…），拒绝超过 maxBytes 的图。
bool LoadImageFile(const std::string& utf8Path, std::int64_t maxBytes, ClipboardImage* image);

bool DecodeImageBytes(const std::uint8_t* bytes, std::size_t size, std::int64_t maxBytes,
                      ClipboardImage* image);

// 贴图卡片：白底文本卡与颜色卡（尺寸规则在 core::clipboard::TextCardBounds）。
bool RenderTextCard(const std::string& utf8Text, double scale, ClipboardImage* image);
bool RenderColorCard(core::clipboard::Color color, double scale, ClipboardImage* image);

// 自检用：把文本放进剪贴板。
bool WriteUnicodeTextToClipboard(const std::string& utf8Text);

}
