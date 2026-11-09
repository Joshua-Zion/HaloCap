#pragma once

#include "display_list.h"

#include <string_view>
#include <vector>

namespace halocap::core {

// WPF 的 path 微语言子集：可选的 F0/F1 前缀，然后 M/L/H/V/C/S/Q/T/Z（大小写都支持，
// 小写是相对坐标）。解析结果直接喂给 PathShape；遇到不认识的命令就跳过该命令。
//
// 1.x 的工具栏图标就存在 App.xaml 的 StreamGeometry 里，这样能原样搬过来，
// 不用照着轮廓重新画一遍。
bool ParsePathData(std::string_view data, std::vector<PathCommand>* commands, bool* evenOdd);

}
