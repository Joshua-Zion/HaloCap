#pragma once

#include "ocr_engine.h"

#include <memory>
#include <string>

namespace halocap::platform::win32 {

// Windows.Media.Ocr 的引擎：系统自带，不需要模型，作为 PP-OCR 之外的第二引擎。
// 识别在一条 MTA 线程上跑（主线程可能是 STA，直接 .get() 会卡住消息泵）。
class WinrtOcrEngine final : public core::IOcrEngine {
public:
    WinrtOcrEngine();
    ~WinrtOcrEngine() override;

    WinrtOcrEngine(const WinrtOcrEngine&) = delete;
    WinrtOcrEngine& operator=(const WinrtOcrEngine&) = delete;

    bool Initialize(const core::OcrEngineConfig& config, std::string* error) override;
    bool Ready() const override { return ready_; }
    const char* Name() const override { return "windows-media-ocr"; }

    core::ocr::OcrResult Recognize(const core::ocr::OcrImage& image,
                                   const core::ocr::OcrOptions& options,
                                   std::string* error) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool ready_ = false;
};

}
