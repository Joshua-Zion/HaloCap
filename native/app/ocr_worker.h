#pragma once

#include "ocr.h"

#include <cstdint>
#include <string>
#include <vector>

namespace halocap::app {

// OCR worker：把模型推理放进一次性的子进程，主进程不加载 ORT。
// 协议是「一个临时 raw 文件进、一个文本结果出」：
//   in:  int32 width, int32 height, int32 stride, 然后 width*height*4 的 BGRA
//   out: 第一行 OK 或 ERROR:<原因>，之后每行 x\ty\tw\th\tconf\ttext
struct OcrWorkerResult {
    bool ok = false;
    std::string error;
    std::vector<core::ocr::OcrLine> lines;
};

bool WriteOcrWorkerInput(const std::string& path, const std::uint8_t* bgra, int width, int height);
bool ReadOcrWorkerOutput(const std::string& path, OcrWorkerResult* result);

// 子进程入口（--ocr-worker in out engine）。返回进程退出码。
int RunOcrWorker(const std::string& inputPath, const std::string& outputPath,
                 const std::string& engineName);

// 主进程侧：起子进程并等它结束。超时返回 false。
bool SpawnOcrWorker(const std::string& executablePath, const std::string& inputPath,
                    const std::string& outputPath, const std::string& engineName,
                    std::string* error);

}
