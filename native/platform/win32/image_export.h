#pragma once

#include <cstdint>
#include <string>

namespace halocap::platform::win32 {

// 截图的两种出口：写 PNG 文件、拷进剪贴板（CF_DIB）。
// 输入是 BGRA、行距可以比 width*4 宽（回读出来的就是那样）。
//
// 编码走 WIC：PNG 是 1.x 的默认截图格式，而 WIC 是系统自带、不需要第三方库。
bool SaveBgraPng(const std::wstring& path, const std::uint8_t* bgra, int width, int height,
                 int stride);

// owner 可以是 nullptr：没有窗口时 OpenClipboard(NULL) 一样可用。
bool CopyBgraToClipboard(void* owner, const std::uint8_t* bgra, int width, int height, int stride);

}
