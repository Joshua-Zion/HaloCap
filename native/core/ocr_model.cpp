#include "ocr_model.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace halocap::core {

namespace {

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string FirstMatch(const std::vector<std::string>& names, std::string_view suffix,
                       std::string_view keyword) {
    for (const std::string& name : names) {
        const std::string lower = Lower(name);
        if (EndsWith(lower, suffix) && lower.find(keyword) != std::string::npos) {
            return name;
        }
    }
    return {};
}

std::string FirstWithSuffix(const std::vector<std::string>& names, std::string_view suffix) {
    for (const std::string& name : names) {
        if (EndsWith(Lower(name), suffix)) {
            return name;
        }
    }
    return {};
}

}

OcrModelFiles DetectOcrModelFiles(const std::vector<std::string>& fileNames) {
    OcrModelFiles files;
    files.det = FirstMatch(fileNames, ".onnx", "det");
    files.rec = FirstMatch(fileNames, ".onnx", "rec");
    files.cls = FirstMatch(fileNames, ".onnx", "cls");
    if (files.cls.empty()) {
        files.cls = FirstMatch(fileNames, ".onnx", "textline");
    }
    files.dict = FirstMatch(fileNames, ".txt", "dict");
    if (files.dict.empty()) {
        files.dict = FirstWithSuffix(fileNames, ".txt");
    }
    return files;
}

const OcrModelDownload& OcrModelSmallDownload() {
    static const OcrModelDownload download{
        "ppocrv6-small",
        "ppocrv6-small",
        "PP-OCRv6 高精度",
        "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/PP-OCRv6_det_small.onnx",
        "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/PP-OCRv6_rec_small.onnx",
        "https://github.com/Joshua-Zion/HaloCap/releases/download/models-v1/ppocrv6_dict.txt",
        9929594,
        21234383,
        74947,
    };
    return download;
}

}
