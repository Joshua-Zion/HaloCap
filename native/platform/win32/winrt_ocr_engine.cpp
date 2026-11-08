#include "winrt_ocr_engine.h"

#include <windows.h>

#include <objbase.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace halocap::platform::win32 {

namespace {

namespace Ocr = winrt::Windows::Media::Ocr;
namespace Imaging = winrt::Windows::Graphics::Imaging;
namespace Streams = winrt::Windows::Storage::Streams;
using winrt::Windows::Foundation::Rect;
using winrt::Windows::Globalization::Language;

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

}  // namespace

struct WinrtOcrEngine::Impl {
    Ocr::OcrEngine engine{ nullptr };
    std::wstring language;
};

WinrtOcrEngine::WinrtOcrEngine() : impl_(std::make_unique<Impl>()) {}

WinrtOcrEngine::~WinrtOcrEngine() = default;

bool WinrtOcrEngine::Initialize(const core::OcrEngineConfig&, std::string* error) {
    std::string localError;
    if (error == nullptr) {
        error = &localError;
    }

    // 整个进程共用一个 MTA：初始化一次、不再 uninit。引擎对象属于这个 apartment，
    // 之前「初始化线程退出就 uninit」会让对象失效，下一次识别直接 AV。
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (const winrt::hresult_error& exception) {
        // RPC_E_CHANGED_MODE：调用方已经是 STA，WinRT 仍可用，继续尝试。
        if (exception.code() != RPC_E_CHANGED_MODE) {
            *error = WideToUtf8(exception.message().c_str());
            return false;
        }
    }

    try {
        Ocr::OcrEngine engine = Ocr::OcrEngine::TryCreateFromUserProfileLanguages();
        if (engine == nullptr) {
            engine = Ocr::OcrEngine::TryCreateFromLanguage(Language(L"en-US"));
        }
        if (engine == nullptr) {
            *error = "no OCR language pack installed";
            return false;
        }
        impl_->engine = engine;
        impl_->language = std::wstring(engine.RecognizerLanguage().LanguageTag());
        ready_ = true;
        return true;
    } catch (const winrt::hresult_error& exception) {
        *error = WideToUtf8(exception.message().c_str());
        return false;
    }
}

core::ocr::OcrResult WinrtOcrEngine::Recognize(const core::ocr::OcrImage& image,
                                               const core::ocr::OcrOptions&,
                                               std::string* error) {
    core::ocr::OcrResult result;
    std::string localError;
    if (error == nullptr) {
        error = &localError;
    }
    if (!ready_ || image.bgra == nullptr || image.width <= 0 || image.height <= 0) {
        *error = "engine not ready";
        return result;
    }

    try {
        Imaging::SoftwareBitmap bitmap(Imaging::BitmapPixelFormat::Bgra8, image.width, image.height,
                                       Imaging::BitmapAlphaMode::Ignore);

        const std::size_t bytes =
            static_cast<std::size_t>(image.stride) * static_cast<std::size_t>(image.height);
        Streams::DataWriter writer;
        writer.WriteBytes(
            winrt::array_view<const std::uint8_t>(image.bgra, image.bgra + bytes));
        bitmap.CopyFromBuffer(writer.DetachBuffer());

        const Ocr::OcrResult recognized = impl_->engine.RecognizeAsync(bitmap).get();

        std::vector<core::ocr::OcrLine> lines;
        for (const Ocr::OcrLine& line : recognized.Lines()) {
            const std::string text = WideToUtf8(line.Text().c_str());
            if (text.empty()) {
                continue;
            }

            // 行没有直接的矩形，取所有 word 的并集。
            Rect bounds{};
            bool first = true;
            for (const Ocr::OcrWord& word : line.Words()) {
                const Rect wordBounds = word.BoundingRect();
                if (first) {
                    bounds = wordBounds;
                    first = false;
                } else {
                    const float left = std::min(bounds.X, wordBounds.X);
                    const float top = std::min(bounds.Y, wordBounds.Y);
                    const float right =
                        std::max(bounds.X + bounds.Width, wordBounds.X + wordBounds.Width);
                    const float bottom =
                        std::max(bounds.Y + bounds.Height, wordBounds.Y + wordBounds.Height);
                    bounds = Rect{ left, top, right - left, bottom - top };
                }
            }
            if (first) {
                continue;
            }

            lines.push_back(core::ocr::OcrLine{
                text,
                core::Rect{ bounds.X, bounds.Y, bounds.X + bounds.Width,
                            bounds.Y + bounds.Height },
                1.0f });
        }

        return core::ocr::BuildResult(std::move(lines));
    } catch (const winrt::hresult_error& exception) {
        *error = WideToUtf8(exception.message().c_str());
        return result;
    }
}

}
