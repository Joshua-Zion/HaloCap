#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace halocap::platform::win32 {

// 更新清单的网络通道：WinHTTP GET，UTF-8 返回体，限制 256 KB。
// 失败时 error 给出可读原因（"open" / "connect" / "status:404" / "read" / ...）。
bool HttpGetUtf8(const std::wstring& url, std::string* body, std::string* error);

// 下载到文件（流式，不整包进内存）。progress 会按块回调 (已收字节, 总字节)；
// 总字节未知时为 0。失败时删除半截文件并返回 false。
bool HttpDownloadToFile(
    const std::wstring& url, const std::wstring& path,
    const std::function<void(std::int64_t received, std::int64_t total)>& progress,
    std::string* error);

}
