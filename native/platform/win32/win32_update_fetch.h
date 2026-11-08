#pragma once

#include <string>

namespace halocap::platform::win32 {

// 更新清单的网络通道：WinHTTP GET，UTF-8 返回体，限制 256 KB。
// 失败时 error 给出可读原因（"open" / "connect" / "status:404" / "read" / ...）。
bool HttpGetUtf8(const std::wstring& url, std::string* body, std::string* error);

}
