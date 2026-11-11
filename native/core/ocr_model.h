#pragma once

#include <cstdint>

namespace halocap::core {

// OCR 模型变体：同一个 PP-OCRv6 系列的不同档位。
// Tiny 随程序内置；Small 由用户在设置里下载（更高精度、字典也更大）。
enum class OcrModelVariant : std::int32_t {
    Tiny = 0,
    Small = 1,
};

constexpr int kOcrModelVariantCount = 2;

struct OcrModelInfo {
    OcrModelVariant variant;
    const char* id;       // 稳定短名（设置文件里也用它）
    const char* title;    // 「极速」「高精度」
    const char* detail;   // 一行说明
    bool bundled;         // true = 随程序提供，不需要下载
};

const OcrModelInfo* OcrModelCatalog();
const OcrModelInfo& OcrModelInfoFor(OcrModelVariant variant);

// 设置里存的是下标（0/1）；越界退回 Tiny。
OcrModelVariant OcrModelVariantFromIndex(std::int32_t index);
std::int32_t OcrModelVariantIndex(OcrModelVariant variant);

// 下载源（托管在本仓 GitHub 的 models 发布里）与落盘文件名。
// 落盘名沿用引擎的默认名，这样 OCR worker 不需要知道变体、只认目录即可。
struct OcrModelDownload {
    const char* detUrl;
    const char* recUrl;
    const char* dictUrl;
    long long detBytes;
    long long recBytes;
    long long dictBytes;
    long long TotalBytes() const { return detBytes + recBytes + dictBytes; }
};

OcrModelDownload OcrModelDownloadFor(OcrModelVariant variant);

// 变体在模型目录里必须存在的文件（相对名）。Tiny 用 1.x 的内置名；
// Small 下载后同样按这些名字落盘（cls 从内置目录复制）。
inline constexpr const char* kOcrModelDetName = "ppocrv6_tiny_det.onnx";
inline constexpr const char* kOcrModelRecName = "ppocrv6_tiny_rec.onnx";
inline constexpr const char* kOcrModelClsName = "ppocrv5_textline_cls.onnx";
inline constexpr const char* kOcrModelDictName = "ppocrv6_tiny_dict.txt";

}
