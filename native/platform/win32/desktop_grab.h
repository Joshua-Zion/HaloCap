#pragma once

#include <cstdint>
#include <vector>

namespace halocap::platform::win32 {

// 桌面快照：BGRA 位图（行距 = width*4，不透明）。
//
// 走 GDI BitBlt 而不是桌面复制，是因为本机（Win11 26300 + 混合显卡）的
// DuplicateOutput 回读出来是全黑的——同样的桌面 GDI 抓得到内容。截图不需要
// 逐帧增量，一次 BitBlt 足够；录制的那条路仍然用桌面复制（S2 的编码链）。
struct DesktopImage {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

// 虚拟桌面在屏幕坐标里的矩形（多屏时左上角可能是负的）。
struct DesktopBounds {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
};

// 要叠进抓屏结果的一块「分层窗口」：托盘菜单、光圈都是 WS_EX_LAYERED，用
// UpdateLayeredWindow 呈现，内容由 DWM 在屏幕 DC 之外合成——BitBlt(CAPTUREBLT)
// 从屏幕 DC 根本抓不到它们。所以抓屏后把这类窗口自己已经渲染好的位图（预乘
// alpha）用 AlphaBlend 补叠上去，这样把菜单/光圈框进截图才能真正截到内容。
struct LayeredOverlay {
    int screenX = 0;  // 窗口左上角在虚拟桌面坐标里的位置
    int screenY = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> premultipliedBgra;  // 预乘 alpha，行距 width*4
};

DesktopBounds VirtualDesktopBounds();

bool GrabPrimaryDesktop(DesktopImage& image);

// 多屏一次抓完整块虚拟桌面（1.x 的截图容器就是这么算的）。
// overlays 非空时，抓完屏幕再把这些分层窗口的位图叠上去。
bool GrabVirtualDesktop(DesktopImage& image,
                        const std::vector<LayeredOverlay>* overlays = nullptr);

}
