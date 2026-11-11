#include "ocr_model.h"

#include <algorithm>

namespace halocap::core {

namespace {

const OcrModelInfo kCatalog[kOcrModelVariantCount] = {
    { OcrModelVariant::Tiny, "ppocrv6-tiny", "极速", "PP-OCRv6 tiny · 随程序内置，速度快、占用低", true },
    { OcrModelVariant::Small, "ppocrv6-small", "高精度",
      "PP-OCRv6 small · 更强的小字/混排识别，需要下载（约 31 MB）", false },
};

constexpr const char* kModelsBase =
    "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/";

// Small 的三个下载文件（det/rec/字典）。cls 与内置模型共用，下载后会从内置目录复制。
const OcrModelDownload kSmallDownload{
    "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/PP-OCRv6_det_small.onnx",
    "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/PP-OCRv6_rec_small.onnx",
    "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/ppocrv6_dict.txt",
    9929594,
    21234383,
    74947,
};

}  // namespace

const OcrModelInfo* OcrModelCatalog() {
    return kCatalog;
}

const OcrModelInfo& OcrModelInfoFor(OcrModelVariant variant) {
    const std::int32_t index = OcrModelVariantIndex(variant);
    return kCatalog[std::clamp(index, 0, kOcrModelVariantCount - 1)];
}

OcrModelVariant OcrModelVariantFromIndex(std::int32_t index) {
    if (index == static_cast<std::int32_t>(OcrModelVariant::Small)) {
        return OcrModelVariant::Small;
    }
    return OcrModelVariant::Tiny;
}

std::int32_t OcrModelVariantIndex(OcrModelVariant variant) {
    return static_cast<std::int32_t>(variant);
}

OcrModelDownload OcrModelDownloadFor(OcrModelVariant variant) {
    if (variant == OcrModelVariant::Small) {
        return kSmallDownload;
    }
    return OcrModelDownload{};
}

}
