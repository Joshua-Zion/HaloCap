#pragma once

#include "store.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::platform::win32 {

// IStore 的 Win32 实现。目录约定（与 1.x 完全分开，绝不写它的目录）：
//
//   %APPDATA%\HaloCap2\settings.json          设置（漫游，1.x 也是在漫游目录）
//   %APPDATA%\HaloCap2\logs\app-YYYYMMDD.log  日志
//   %LOCALAPPDATA%\HaloCap2\models\<version>\    模型（本地，不参与漫游同步）
//
// 原子替换用「临时文件 + MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)」：1.x 用的是
// File.Move(overwrite: true)，同一条语义，但刷盘这一步由调用方显式要求。
class Win32Store final : public core::IStore {
public:
    Win32Store();

    // 探针用：换一个根目录，三个路径都相对它算（模型目录也在这下面）。
    // 这条重载存在的原因是要能测「原子替换」而不去动用户真实的设置。
    explicit Win32Store(std::wstring rootOverride);

    bool LoadSettings(std::string& text, bool& exists) override;
    bool SaveSettings(std::string_view text) override;

    bool AppendLog(std::string_view name, std::string_view line) override;
    std::vector<core::LogFileEntry> ListLogs() override;
    bool DeleteLog(std::string_view name) override;

    const char* SettingsPath() const override { return settingsPathUtf8_.c_str(); }
    const char* LogDirectory() const override { return logDirectoryUtf8_.c_str(); }
    const char* ModelDirectory() const override { return modelDirectoryUtf8_.c_str(); }

    const char* LastErrorKind() const override { return errorKind_; }
    std::int32_t LastErrorCode() const override { return static_cast<std::int32_t>(lastError_); }

private:
    // 传入的已经是「HaloCap2 根」：设置/日志在 appRoot，模型在 modelRoot。
    void Initialize(const std::wstring& appRoot, const std::wstring& modelRoot);

    void SetError(const char* kind, unsigned long code);

private:
    std::wstring settingsPath_;
    std::wstring logDirectory_;
    std::wstring modelDirectory_;

    std::string settingsPathUtf8_;
    std::string logDirectoryUtf8_;
    std::string modelDirectoryUtf8_;

    const char* errorKind_ = "none";
    unsigned long lastError_ = 0;
};

}
