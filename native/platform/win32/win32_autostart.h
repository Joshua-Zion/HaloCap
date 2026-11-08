#pragma once

#include <string>

namespace halocap::platform::win32 {

// 开机自启：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 下的 "HaloCap"。
// 只动当前用户的键，不需要管理员权限；exe 路径带引号，避免空格被拆开。
bool SetAutoStart(bool enabled, const std::wstring& executablePath, std::string* error);
bool IsAutoStartEnabled(const std::wstring& executablePath);

}
