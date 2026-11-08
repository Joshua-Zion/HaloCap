#include "win32_autostart.h"

#include <windows.h>

namespace halocap::platform::win32 {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"HaloCap";

}  // namespace

bool SetAutoStart(bool enabled, const std::wstring& executablePath, std::string* error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS) {
        if (error != nullptr) {
            *error = "RegCreateKeyEx failed";
        }
        return false;
    }

    bool ok = false;
    if (enabled) {
        const std::wstring quoted = L"\"" + executablePath + L"\"";
        ok = RegSetValueExW(key, kValueName, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(quoted.c_str()),
                            static_cast<DWORD>((quoted.size() + 1) * sizeof(wchar_t))) ==
             ERROR_SUCCESS;
    } else {
        const LSTATUS status = RegDeleteValueW(key, kValueName);
        ok = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);

    if (!ok && error != nullptr) {
        *error = enabled ? "RegSetValueEx failed" : "RegDeleteValue failed";
    }
    return ok;
}

bool IsAutoStartEnabled(const std::wstring& executablePath) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
        return false;
    }

    wchar_t buffer[1024] = {};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS status = RegQueryValueExW(key, kValueName, nullptr, &type,
                                            reinterpret_cast<BYTE*>(buffer), &size);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS || type != REG_SZ) {
        return false;
    }

    const std::wstring expected = L"\"" + executablePath + L"\"";
    return expected == buffer;
}

}
