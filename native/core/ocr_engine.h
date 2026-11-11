#pragma once

#include "ocr.h"

#include <string>

namespace halocap::core {

struct OcrEngineConfig {
    // 模型目录：det/rec/cls 三个 onnx 与字典都在这里。
    std::string modelDirectory;
    // 分类模型不在模型目录里时可单独指定（用户只放了 det/rec/字典时用内置的那份）。
    std::string clsDirectory;
    std::string detModel = "ppocrv6_tiny_det.onnx";
    std::string recModel = "ppocrv6_tiny_rec.onnx";
    std::string clsModel = "ppocrv5_textline_cls.onnx";
    std::string dictionary = "ppocrv6_tiny_dict.txt";
    int threads = 2;
};

// OCR 引擎 seam。前后处理在 core（ocr.h），这里只负责「模型进、结果出」。
// 会话按需创建：app 空闲时不持有引擎，OCR 之后可以整体释放，满足「空闲 ≤ 80 MB」。
class IOcrEngine {
public:
    virtual ~IOcrEngine() = default;

    virtual bool Initialize(const OcrEngineConfig& config, std::string* error) = 0;
    virtual bool Ready() const = 0;
    virtual const char* Name() const = 0;

    virtual ocr::OcrResult Recognize(const ocr::OcrImage& image, const ocr::OcrOptions& options,
                                     std::string* error) = 0;
};

}
