#pragma once

#include <cstdint>
#include <vector>

namespace halocap::platform::win32 {

// 桌面快照：主显示器的 BGRA 位图（行距 = width*4，不透明）。
//
// 走 GDI BitBlt 而不是桌面复制，是因为本机（Win11 26300 + 混合显卡）的
// DuplicateOutput 回读出来是全黑的——同样的桌面 GDI 抓得到内容。截图不需要
// 逐帧增量，一次 BitBlt 足够；录制的那条路仍然用桌面复制（S2 的编码链）。
struct DesktopImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

bool GrabPrimaryDesktop(DesktopImage& image);

}
