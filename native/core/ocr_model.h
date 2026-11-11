#pragma once

#include <string>
#include <vector>

namespace halocap::core {

// 模型目录里的一套 PP-OCR ONNX 文件。检测、识别与字典必须有，方向分类模型可选
// （没有时用内置的那一份）。文件名不要求固定，按关键字识别，方便直接放入
// 官方导出的模型或其他 PP-OCR 系列模型。
struct OcrModelFiles {
    std::string det;
    std::string rec;
    std::string dict;
    std::string cls;

    bool Complete() const {
        return !det.empty() && !rec.empty() && !dict.empty();
    }
};

// 从目录里的文件名挑出一套模型。纯逻辑，不访问文件系统，便于单测。
// 规则（不区分大小写）：*.onnx 里含 det / rec / cls（或 textline）分别作为
// 检测 / 识别 / 分类模型；*.txt 里含 dict 的作为字典（没有则取第一个 txt）。
OcrModelFiles DetectOcrModelFiles(const std::vector<std::string>& fileNames);

// 官方托管的可下载模型。下载到 %LOCALAPPDATA%\HaloCap2\models\<folder>\。
struct OcrModelDownload {
    const char* id;
    const char* folder;
    const char* title;
    const char* detUrl;
    const char* recUrl;
    const char* dictUrl;
    long long detBytes;
    long long recBytes;
    long long dictBytes;

    long long TotalBytes() const { return detBytes + recBytes + dictBytes; }
};

const OcrModelDownload& OcrModelSmallDownload();

// 随包内置的模型目录名（models\<folder>）。
inline constexpr const char* kOcrModelBuiltInFolder = "ppocrv6-tiny-1";
inline constexpr const char* kOcrModelSmallFolder = "ppocrv6-small";
inline constexpr const char* kOcrModelClsName = "ppocrv5_textline_cls.onnx";

}
