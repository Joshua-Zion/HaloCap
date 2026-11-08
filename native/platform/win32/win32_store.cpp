#include "win32_store.h"

#include <windows.h>

#include <ShlObj.h>

#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <system_error>

namespace halocap::platform::win32 {

namespace {

// 设置文件是给人读的几百字节；给它一个上限，是防止「把整个磁盘的路径填进来」这种输入
// 让进程在这里直接分配掉几 GB。
constexpr std::size_t kMaxSettingsBytes = 4u * 1024u * 1024u;

constexpr long long kUnixEpochInFileTime = 116444736000000000LL;

std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                           nullptr, 0);
    if (length <= 0) {
        return {};
    }
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
    if (length <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), length,
                        nullptr, nullptr);
    return utf8;
}

// 已知文件夹（漫游/本地 AppData）。SHGetKnownFolderPath 会尊重组策略与 OneDrive 重定向，
// 比直接拼 %APPDATA% 环境变量正确——1.x 用的是 Environment.SpecialFolder，等价于前者。
std::wstring KnownFolder(REFKNOWNFOLDERID id, const wchar_t* fallbackVariable) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw != nullptr) {
        std::wstring folder(raw);
        CoTaskMemFree(raw);
        return folder;
    }

    const DWORD needed = GetEnvironmentVariableW(fallbackVariable, nullptr, 0);
    if (needed > 0) {
        std::wstring value(needed, L'\0');
        const DWORD written = GetEnvironmentVariableW(fallbackVariable, value.data(), needed);
        value.resize(written);
        return value;
    }
    return {};
}

std::wstring ParentDirectory(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }
    return path.substr(0, slash);
}

bool EnsureDirectory(const std::wstring& path, unsigned long* errorOut = nullptr) {
    if (path.empty()) {
        if (errorOut != nullptr) {
            *errorOut = ERROR_PATH_NOT_FOUND;
        }
        return false;
    }
    // 用标准库递归建：它处理驱动器根、UNC 共享前缀与已存在这三类边界，
    // 自己按反斜杠切片会在 `\\server\share` 上切出 `\\` 这种非法前缀。
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path(path), error);
    if (error) {
        if (errorOut != nullptr) {
            *errorOut = static_cast<unsigned long>(error.value());
        }
        return false;
    }
    std::error_code checkError;
    const bool isDirectory = std::filesystem::is_directory(std::filesystem::path(path), checkError);
    if (!isDirectory && errorOut != nullptr) {
        *errorOut = static_cast<unsigned long>(checkError.value());
    }
    return isDirectory;
}

bool WriteAllBytes(const std::wstring& path, std::string_view bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::size_t written = 0;
    bool ok = true;
    while (written < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - written, 1u << 20));
        DWORD done = 0;
        if (!WriteFile(file, bytes.data() + written, chunk, &done, nullptr) || done == 0) {
            ok = false;
            break;
        }
        written += done;
    }

    // 刷盘失败同样算失败：原子替换的前提是临时文件的内容真的落地了。
    if (ok && !FlushFileBuffers(file)) {
        ok = false;
    }

    CloseHandle(file);
    return ok;
}

std::int64_t FileTimeToUnixSeconds(const FILETIME& time) {
    const long long ticks = (static_cast<long long>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    return (ticks - kUnixEpochInFileTime) / 10000000LL;
}

const char* MapError(unsigned long code) {
    switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return "denied";
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return "missing";
    default:
        return "io";
    }
}

}  // namespace

Win32Store::Win32Store() {
    // 两个根：设置与日志放漫游目录（跟 1.x 同一类），模型放本地目录（几十 MB，不该参与同步）。
    const std::wstring roaming = KnownFolder(FOLDERID_RoamingAppData, L"APPDATA");
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData, L"LOCALAPPDATA");

    const std::wstring appRoot = roaming.empty() ? std::wstring{} : roaming + L"\\HaloCap2";
    const std::wstring modelRoot = local.empty() ? appRoot : local + L"\\HaloCap2";
    Initialize(appRoot, modelRoot);
}

Win32Store::Win32Store(std::wstring rootOverride) {
    // 探针：设置、日志、模型全在这个根下面，不会碰到任何真实的用户目录。
    Initialize(rootOverride, rootOverride);
}

void Win32Store::Initialize(const std::wstring& appRoot, const std::wstring& modelRoot) {
    settingsPath_ = appRoot.empty() ? std::wstring{} : appRoot + L"\\settings.json";
    logDirectory_ = appRoot.empty() ? std::wstring{} : appRoot + L"\\logs";
    modelDirectory_ = modelRoot.empty()
                          ? std::wstring{}
                          : modelRoot + L"\\models\\" + Utf8ToWide(core::kOcrModelVersion);

    settingsPathUtf8_ = WideToUtf8(settingsPath_);
    logDirectoryUtf8_ = WideToUtf8(logDirectory_);
    modelDirectoryUtf8_ = WideToUtf8(modelDirectory_);

    errorKind_ = "none";
    lastError_ = 0;
}

void Win32Store::SetError(const char* kind, unsigned long code) {
    errorKind_ = kind;
    lastError_ = code;
}

bool Win32Store::LoadSettings(std::string& text, bool& exists) {
    text.clear();
    exists = false;

    if (settingsPath_.empty()) {
        SetError("invalid", ERROR_PATH_NOT_FOUND);
        return false;
    }

    HANDLE file = CreateFileW(settingsPath_.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const unsigned long error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            // 首次启动不是错误。
            SetError("missing", error);
            return true;
        }
        SetError(MapError(error), error);
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<unsigned long long>(size.QuadPart) > kMaxSettingsBytes) {
        const unsigned long error = GetLastError();
        CloseHandle(file);
        SetError("io", error);
        return false;
    }

    text.resize(static_cast<std::size_t>(size.QuadPart));
    std::size_t read = 0;
    bool ok = true;
    while (read < text.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(text.size() - read, 1u << 20));
        DWORD done = 0;
        if (!ReadFile(file, text.data() + read, chunk, &done, nullptr) || done == 0) {
            ok = false;
            break;
        }
        read += done;
    }

    const unsigned long error = ok ? 0ul : GetLastError();
    CloseHandle(file);

    if (!ok) {
        text.clear();
        SetError("io", error);
        return false;
    }

    exists = true;
    SetError("ok", 0);
    return true;
}

bool Win32Store::SaveSettings(std::string_view text) {
    if (settingsPath_.empty()) {
        SetError("invalid", ERROR_PATH_NOT_FOUND);
        return false;
    }

    const std::wstring parent = ParentDirectory(settingsPath_);
    unsigned long error = 0;
    if (!EnsureDirectory(parent, &error)) {
        SetError(MapError(error), error);
        return false;
    }

    const std::wstring temporary = settingsPath_ + L".tmp";
    if (!WriteAllBytes(temporary, text)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetError(MapError(error), error);
        return false;
    }

    // 整体换上去，而不是「先删后写」：进程在任何一刻被杀掉，磁盘上要么是旧版本，要么是新版本。
    if (!MoveFileExW(temporary.c_str(), settingsPath_.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = GetLastError();
        DeleteFileW(temporary.c_str());
        SetError(MapError(error), error);
        return false;
    }

    SetError("ok", 0);
    return true;
}

bool Win32Store::AppendLog(std::string_view name, std::string_view line) {
    if (name.empty() || name.find('\\') != std::string_view::npos ||
        name.find('/') != std::string_view::npos || name.find(':') != std::string_view::npos) {
        SetError("invalid", ERROR_INVALID_NAME);
        return false;
    }

    unsigned long error = 0;
    if (!EnsureDirectory(logDirectory_, &error)) {
        SetError(MapError(error), error);
        return false;
    }
    const std::wstring path = logDirectory_ + L"\\" + Utf8ToWide(name);
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = GetLastError();
        SetError(MapError(error), error);
        return false;
    }

    bool ok = true;
    std::size_t written = 0;
    while (written < line.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(line.size() - written, 1u << 20));
        DWORD done = 0;
        if (!WriteFile(file, line.data() + written, chunk, &done, nullptr) || done == 0) {
            ok = false;
            break;
        }
        written += done;
    }

    const unsigned long writeError = ok ? 0ul : GetLastError();
    CloseHandle(file);

    SetError(ok ? "ok" : MapError(writeError), writeError);
    return ok;
}

std::vector<core::LogFileEntry> Win32Store::ListLogs() {    std::vector<core::LogFileEntry> entries;
    if (logDirectory_.empty()) {
        SetError("invalid", ERROR_PATH_NOT_FOUND);
        return entries;
    }

    WIN32_FIND_DATAW data{};
    const std::wstring pattern = logDirectory_ + L"\\*.log";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        const unsigned long error = GetLastError();
        // 日志目录还没建出来是正常状态：一条日志都没有，不是错误。
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            SetError("missing", error);
        } else {
            SetError(MapError(error), error);
        }
        return entries;
    }

    do {
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }

        core::LogFileEntry entry;
        entry.name = WideToUtf8(data.cFileName);
        entry.sizeBytes = (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) |
                          static_cast<std::int64_t>(data.nFileSizeLow);
        entry.writtenUnixSeconds = FileTimeToUnixSeconds(data.ftLastWriteTime);
        entries.push_back(std::move(entry));
    } while (FindNextFileW(find, &data) != 0);

    FindClose(find);
    SetError("ok", 0);
    return entries;
}

bool Win32Store::DeleteLog(std::string_view name) {
    if (name.empty() || name.find('\\') != std::string_view::npos ||
        name.find('/') != std::string_view::npos || name.find(':') != std::string_view::npos ||
        name == ".." || name == ".") {
        SetError("invalid", ERROR_INVALID_NAME);
        return false;
    }

    const std::wstring path = logDirectory_ + L"\\" + Utf8ToWide(name);
    if (!DeleteFileW(path.c_str())) {
        const unsigned long error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            // 已经被别人删掉了：清理的目标已经达成。
            SetError("missing", error);
            return true;
        }
        SetError(MapError(error), error);
        return false;
    }

    SetError("ok", 0);
    return true;
}

}
