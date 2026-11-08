#include "ocr_worker.h"

#include "ocr_engine.h"
#include "ort_ocr_engine.h"
#include "winrt_ocr_engine.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>

namespace halocap::app {

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

// 结果文本里不能出现分隔符：制表符/换行会被当成列边界。
std::string Sanitize(std::string text) {
    for (char& character : text) {
        if (character == '\t' || character == '\n' || character == '\r') {
            character = ' ';
        }
    }
    return text;
}

std::string ModelDirectoryFromWorker() {
    if (const char* environment = std::getenv("HALOCAP_OCR_MODELS")) {
        return environment;
    }

    wchar_t executable[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, executable, MAX_PATH) == 0) {
        return {};
    }
    const std::filesystem::path directory = std::filesystem::path(executable).parent_path();
    const std::filesystem::path candidates[] = {
        directory / "models" / "ppocrv6-tiny-1",
        directory / ".." / ".." / ".." / "src" / "HaloCap" / "Assets" / "OcrModel",
    };
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code error;
        if (std::filesystem::exists(candidate / "ppocrv6_tiny_det.onnx", error)) {
            return candidate.string();
        }
    }
    return {};
}

}

bool WriteOcrWorkerInput(const std::string& path, const std::uint8_t* bgra, int width,
                         int height) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream || bgra == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    const std::int32_t header[3] = { width, height, width * 4 };
    stream.write(reinterpret_cast<const char*>(header), sizeof(header));
    stream.write(reinterpret_cast<const char*>(bgra),
                 static_cast<std::streamsize>(width) * height * 4);
    return stream.good();
}

bool ReadOcrWorkerOutput(const std::string& path, OcrWorkerResult* result) {
    if (result == nullptr) {
        return false;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        result->error = "output not found";
        return false;
    }

    std::string status;
    std::getline(stream, status);
    if (status.rfind("OK", 0) != 0) {
        result->error = status.rfind("ERROR:", 0) == 0 ? status.substr(6) : "worker failed";
        return false;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty()) {
            continue;
        }
        std::istringstream row(line);
        float x = 0.0f;
        float y = 0.0f;
        float w = 0.0f;
        float h = 0.0f;
        float confidence = 0.0f;
        char separator = 0;
        if (!(row >> x >> separator >> y >> separator >> w >> separator >> h >> separator >>
              confidence >> separator)) {
            continue;
        }
        std::string text;
        std::getline(row, text);
        if (text.empty()) {
            continue;
        }
        result->lines.push_back(core::ocr::OcrLine{ text, core::Rect{ x, y, x + w, y + h },
                                                    confidence });
    }

    result->ok = true;
    return true;
}

int RunOcrWorker(const std::string& inputPath, const std::string& outputPath,
                 const std::string& engineName) {
    std::ifstream stream(inputPath, std::ios::binary);
    if (!stream) {
        std::ofstream(outputPath) << "ERROR:input missing\n";
        return 2;
    }

    std::int32_t header[3] = {};
    stream.read(reinterpret_cast<char*>(header), sizeof(header));
    const int width = header[0];
    const int height = header[1];
    const int stride = header[2];
    if (width <= 0 || height <= 0 || stride < width * 4) {
        std::ofstream(outputPath) << "ERROR:bad header\n";
        return 2;
    }

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height, 0);
    stream.read(reinterpret_cast<char*>(pixels.data()),
                static_cast<std::streamsize>(pixels.size()));
    if (!stream) {
        std::ofstream(outputPath) << "ERROR:short image\n";
        return 2;
    }

    std::unique_ptr<core::IOcrEngine> engine;
    std::string error;
    core::OcrEngineConfig config;
    config.modelDirectory = ModelDirectoryFromWorker();

    if (engineName == "winrt") {
        engine = std::make_unique<halocap::platform::win32::WinrtOcrEngine>();
    } else {
        engine = std::make_unique<halocap::platform::win32::OrtOcrEngine>();
    }

    if (!engine->Initialize(config, &error)) {
        std::ofstream(outputPath) << "ERROR:" << Sanitize(error) << "\n";
        return 3;
    }

    const core::ocr::OcrImage image{ pixels.data(), width, height, stride };
    const core::ocr::OcrResult result = engine->Recognize(image, core::ocr::OcrOptions{}, &error);
    if (result.lines.empty() && !error.empty()) {
        std::ofstream(outputPath) << "ERROR:" << Sanitize(error) << "\n";
        return 4;
    }

    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output << "OK\n";
    for (const core::ocr::OcrLine& line : result.lines) {
        output << line.box.left << '\t' << line.box.top << '\t' << line.box.Width() << '\t'
               << line.box.Height() << '\t' << line.confidence << '\t' << Sanitize(line.text)
               << '\n';
    }
    output.flush();
    return output.good() ? 0 : 5;
}

bool SpawnOcrWorker(const std::string& executablePath, const std::string& inputPath,
                    const std::string& outputPath, const std::string& engineName,
                    std::string* error) {
    std::wstring commandLine = L"\"" + Utf8ToWide(executablePath) + L"\" --ocr-worker \"" +
                               Utf8ToWide(inputPath) + L"\" \"" + Utf8ToWide(outputPath) +
                               L"\" " + Utf8ToWide(engineName);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    std::wstring mutableCommand = commandLine;
    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        *error = "CreateProcess failed";
        return false;
    }

    const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    if (waited != WAIT_OBJECT_0) {
        *error = "worker timeout";
        return false;
    }
    if (exitCode != 0) {
        *error = "worker exit " + std::to_string(exitCode);
        return false;
    }
    return true;
}

}
