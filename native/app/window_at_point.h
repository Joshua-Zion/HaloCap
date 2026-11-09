#pragma once

#include <windows.h>

namespace halocap::app {

// 光标下的顶层窗口。1.x 的 WindowProbe 与 2.0 的窗口拾取、单击选窗口共用这一份判定：
//   - 跳过本进程的窗口（ignore 还会临时禁用，避免 topmost 的覆盖层永远命中自己）
//   - 跳过桌面/Shell、不可见、被 DWM 遮住（cloaked）、WS_EX_TRANSPARENT
//   - 跳过工具提示/输入法/阴影这类系统窗口
//   - 跳过可见矩形小于 minSize 的区域（图标、勾选框）
// 返回 nullptr 表示这块位置没有可选的窗口。
HWND WindowAtPoint(POINT screen, HWND ignore, int minSize = 32);

// 窗口的可见矩形：优先 DWM 扩展边框（不含 Win10 之后的隐形缩放边），退回 GetWindowRect。
bool WindowVisibleRect(HWND window, RECT* rect);

}
