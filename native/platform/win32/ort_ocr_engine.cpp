#include "ort_ocr_engine.h"

#include "ocr.h"

#include <windows.h>

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace halocap::platform::win32 {

namespace {

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return wide;
}

std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

bool ReadDictionary(const std::string& path, std::vector<std::string>* dictionary,
                    std::string* error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        *error = "dictionary not found: " + path;
        return false;
    }

    std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    if (content.size() >= 3 && static_cast<std::uint8_t>(content[0]) == 0xEF &&
        static_cast<std::uint8_t>(content[1]) == 0xBB && static_cast<std::uint8_t>(content[2]) == 0xBF) {
        content.erase(0, 3);
    }

    std::string line;
    for (const char character : content) {
        if (character == '\n') {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                dictionary->push_back(line);
            }
            line.clear();
        } else {
            line.push_back(character);
        }
    }
    if (!line.empty()) {
        dictionary->push_back(line);
    }

    if (dictionary->empty()) {
        *error = "dictionary is empty";
        return false;
    }
    return true;
}

// 在 rec 输入上做 180° 旋转：检测出的竖排/倒置文本先转正再识别（1.x 的 cls 分支）。
void Rotate180(std::vector<float>* data, int width, int height) {
    const std::size_t plane = static_cast<std::size_t>(width) * height;
    for (int channel = 0; channel < 3; ++channel) {
        float* base = data->data() + static_cast<std::size_t>(channel) * plane;
        std::reverse(base, base + plane);
    }
}

}  // namespace

struct OrtOcrEngine::Impl {
    Ort::Env environment{ ORT_LOGGING_LEVEL_ERROR, "halocap-ocr" };
    std::unique_ptr<Ort::Session> det;
    std::unique_ptr<Ort::Session> rec;
    std::unique_ptr<Ort::Session> cls;

    std::string detInput;
    std::string detOutput;
    std::string recInput;
    std::string recOutput;
    std::string clsInput;
    std::string clsOutput;

    std::vector<std::string> dictionary;
    int threads = 2;

    bool CreateSession(const std::string& path, std::unique_ptr<Ort::Session>* session,
                       std::string* inputName, std::string* outputName, std::string* error) {
        try {
            Ort::SessionOptions options;
            options.SetIntraOpNumThreads(threads);
            options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
            options.DisableMemPattern();

            const std::wstring widePath = Utf8ToWide(path);
            auto created = std::make_unique<Ort::Session>(environment, widePath.c_str(), options);

            Ort::AllocatorWithDefaultOptions allocator;
            const Ort::AllocatedStringPtr input = created->GetInputNameAllocated(0, allocator);
            const Ort::AllocatedStringPtr output = created->GetOutputNameAllocated(0, allocator);
            *inputName = input.get();
            *outputName = output.get();
            *session = std::move(created);
            return true;
        } catch (const Ort::Exception& exception) {
            *error = std::string("ort: ") + exception.what();
            return false;
        }
    }

    std::vector<float> Run(Ort::Session& session, const std::string& inputName,
                           const std::string& outputName, const std::vector<float>& data,
                           std::array<std::int64_t, 4> shape, std::vector<std::int64_t>* outShape,
                           std::string* error) {
        std::vector<float> result;
        try {
            Ort::MemoryInfo memory =
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value tensor = Ort::Value::CreateTensor<float>(
                memory, const_cast<float*>(data.data()), data.size(), shape.data(), shape.size());

            const char* inputNames[] = { inputName.c_str() };
            const char* outputNames[] = { outputName.c_str() };
            auto outputs = session.Run(Ort::RunOptions{ nullptr }, inputNames, &tensor, 1,
                                       outputNames, 1);
            if (outputs.empty()) {
                *error = "ort: no output";
                return result;
            }

            const auto info = outputs[0].GetTensorTypeAndShapeInfo();
            *outShape = info.GetShape();
            const float* values = outputs[0].GetTensorData<float>();
            const std::size_t count = info.GetElementCount();
            result.assign(values, values + count);
        } catch (const Ort::Exception& exception) {
            *error = std::string("ort: ") + exception.what();
        }
        return result;
    }
};

OrtOcrEngine::OrtOcrEngine() : impl_(std::make_unique<Impl>()) {}

OrtOcrEngine::~OrtOcrEngine() = default;

bool OrtOcrEngine::Initialize(const core::OcrEngineConfig& config, std::string* error) {
    std::string localError;
    if (error == nullptr) {
        error = &localError;
    }

    impl_->threads = std::max(1, config.threads);
    if (config.modelDirectory.empty()) {
        *error = "model directory is empty";
        return false;
    }

    if (!ReadDictionary(config.modelDirectory + "\\" + config.dictionary, &impl_->dictionary,
                        error)) {
        return false;
    }

    if (!impl_->CreateSession(config.modelDirectory + "\\" + config.detModel, &impl_->det,
                              &impl_->detInput, &impl_->detOutput, error)) {
        return false;
    }
    if (!impl_->CreateSession(config.modelDirectory + "\\" + config.recModel, &impl_->rec,
                              &impl_->recInput, &impl_->recOutput, error)) {
        return false;
    }
    if (!impl_->CreateSession(config.modelDirectory + "\\" + config.clsModel, &impl_->cls,
                              &impl_->clsInput, &impl_->clsOutput, error)) {
        return false;
    }

    ready_ = true;
    return true;
}

core::ocr::OcrResult OrtOcrEngine::Recognize(const core::ocr::OcrImage& image,
                                             const core::ocr::OcrOptions& options,
                                             std::string* error) {
    core::ocr::OcrResult result;
    std::string localError;

    if (!ready_) {
        if (error != nullptr) {
            *error = "engine not initialised";
        }
        return result;
    }
    if (image.bgra == nullptr || image.width <= 0 || image.height <= 0) {
        if (error != nullptr) {
            *error = "empty image";
        }
        return result;
    }

    const core::ocr::DetInput det = core::ocr::PrepareDetInput(image, options.detTargetSide);
    if (det.data.empty()) {
        if (error != nullptr) {
            *error = "empty det input";
        }
        return result;
    }

    std::vector<std::int64_t> detShape;
    std::vector<float> probability =
        impl_->Run(*impl_->det, impl_->detInput, impl_->detOutput, det.data,
                   { 1, 3, det.height, det.width }, &detShape, &localError);
    if (probability.empty() || detShape.size() != 4) {
        if (error != nullptr) {
            *error = localError.empty() ? "det failed" : localError;
        }
        return result;
    }

    const int outHeight = static_cast<int>(detShape[2]);
    const int outWidth = static_cast<int>(detShape[3]);

    // PP-OCR 的 det 导出通常带 sigmoid；不带时值域会越界，这里兜一层。
    float maximum = 0.0f;
    float minimum = 1.0f;
    for (const float value : probability) {
        maximum = std::max(maximum, value);
        minimum = std::min(minimum, value);
    }
    if (maximum > 1.001f || minimum < -0.001f) {
        for (float& value : probability) {
            value = 1.0f / (1.0f + std::exp(-value));
        }
    }

    if (std::getenv("HALOCAP_OCR_DEBUG") != nullptr) {
        float sum = 0.0f;
        int positive = 0;
        for (const float value : probability) {
            sum += value;
            positive += value >= options.detThreshold ? 1 : 0;
        }
        std::fprintf(stderr,
                     "det in=%dx%d out=%dx%d min=%.4f max=%.4f mean=%.4f above=%.2f:%d\n",
                     det.width, det.height, outWidth, outHeight, minimum, maximum,
                     sum / static_cast<float>(probability.size()), options.detThreshold,
                     positive);
    }

    core::ocr::DetInput mapped = det;
    mapped.scaleX = det.scaleX * static_cast<float>(outWidth) / static_cast<float>(det.width);
    mapped.scaleY = det.scaleY * static_cast<float>(outHeight) / static_cast<float>(det.height);

    const std::vector<core::Rect> boxes =
        core::ocr::ExtractTextBoxes(probability.data(), outWidth, outHeight, mapped, options);
    if (std::getenv("HALOCAP_OCR_DEBUG") != nullptr) {
        std::fprintf(stderr, "boxes=%zu\n", boxes.size());
    }

    std::vector<core::ocr::OcrLine> lines;
    lines.reserve(boxes.size());

    for (const core::Rect& box : boxes) {
        if (static_cast<int>(lines.size()) >= options.maxLines) {
            break;
        }

        core::ocr::RecInput rec = core::ocr::PrepareRecInput(image, box, options.recHeight);
        if (rec.data.empty() || rec.width < 2) {
            continue;
        }

        // 竖排/倒置：cls 判 1 时把输入转 180°。
        if (impl_->cls != nullptr) {
            std::vector<float> clsData(static_cast<std::size_t>(3) * 48 * 192, 0.0f);
            for (int channel = 0; channel < 3; ++channel) {
                for (int y = 0; y < 48; ++y) {
                    for (int x = 0; x < 192; ++x) {
                        const int sourceX = std::min(rec.width - 1, x * rec.width / 192);
                        clsData[static_cast<std::size_t>(channel) * 48 * 192 + y * 192 + x] =
                            rec.data[static_cast<std::size_t>(channel) * 48 * rec.width +
                                     static_cast<std::size_t>(y) * rec.width + sourceX];
                    }
                }
            }
            std::vector<std::int64_t> clsShape;
            std::vector<float> scores =
                impl_->Run(*impl_->cls, impl_->clsInput, impl_->clsOutput, clsData,
                           { 1, 3, 48, 192 }, &clsShape, &localError);
            if (!scores.empty() && clsShape.size() == 2 && clsShape[1] == 2) {
                if (scores[1] > scores[0] && scores[1] > 0.9f) {
                    Rotate180(&rec.data, rec.width, rec.height);
                }
            }
        }

        std::vector<std::int64_t> recShape;
        const std::vector<float> logits =
            impl_->Run(*impl_->rec, impl_->recInput, impl_->recOutput, rec.data,
                       { 1, 3, rec.height, rec.width }, &recShape, &localError);
        if (logits.empty() || recShape.size() != 3) {
            continue;
        }

        if (std::getenv("HALOCAP_OCR_DEBUG") != nullptr) {
            std::fprintf(stderr, "rec w=%d h=%d shape=[%lld,%lld,%lld]\n", rec.width, rec.height,
                         static_cast<long long>(recShape[0]), static_cast<long long>(recShape[1]),
                         static_cast<long long>(recShape[2]));
        }

        float confidence = 0.0f;
        const std::string text = core::ocr::DecodeCtc(
            logits.data(), static_cast<int>(recShape[1]), static_cast<int>(recShape[2]),
            impl_->dictionary, &confidence);

        if (std::getenv("HALOCAP_OCR_DEBUG") != nullptr) {
            int nonBlank = 0;
            for (int t = 0; t < static_cast<int>(recShape[1]); ++t) {
                const float* row = logits.data() + static_cast<std::size_t>(t) * recShape[2];
                int best = 0;
                for (int c = 1; c < static_cast<int>(recShape[2]); ++c) {
                    if (row[c] > row[best]) {
                        best = c;
                    }
                }
                nonBlank += best != 0 ? 1 : 0;
            }
            std::fprintf(stderr, "rec text='%s' conf=%.3f nonblank=%d/%lld\n", text.c_str(),
                         confidence, nonBlank, static_cast<long long>(recShape[1]));
        }

        if (text.empty() || confidence < options.minLineConfidence) {
            continue;
        }
        lines.push_back(core::ocr::OcrLine{ text, box, confidence });
    }

    result = core::ocr::BuildResult(std::move(lines));
    return result;
}

}
