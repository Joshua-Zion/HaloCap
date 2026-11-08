#pragma once

#include "ocr_engine.h"

#include <memory>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

// ONNX Runtime 的 PP-OCR 引擎：det（DBNet）+ cls（可选）+ rec（CRNN/CTC）。
// 模型来自 1.x 的 Assets/OcrModel（与 1.x 同一份权重），前后处理在 core/ocr。
class OrtOcrEngine final : public core::IOcrEngine {
public:
    OrtOcrEngine();
    ~OrtOcrEngine() override;

    OrtOcrEngine(const OrtOcrEngine&) = delete;
    OrtOcrEngine& operator=(const OrtOcrEngine&) = delete;

    bool Initialize(const core::OcrEngineConfig& config, std::string* error) override;
    bool Ready() const override { return ready_; }
    const char* Name() const override { return "ppocrv6-tiny"; }

    core::ocr::OcrResult Recognize(const core::ocr::OcrImage& image,
                                   const core::ocr::OcrOptions& options,
                                   std::string* error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
    std::string lastError_;
};

}
