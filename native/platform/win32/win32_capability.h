#pragma once

#include "capability.h"

#include <cstdint>

namespace halocap::platform::win32 {

// ICapability 的 Win32 实现：读系统 build、问 WGC 支不支持、量自己的私有内存、
// 查某个路径所在卷还剩多少空间。判断全在 core::capability.h 里，这里只负责把事实读出来。
class Win32Capability final : public core::ICapability {
public:
    core::PlatformFacts Probe() override;

    std::int64_t AvailableBytes(const char* utf8Path) override;
};

// 当前进程是否以管理员权限运行（TokenElevation）。诊断与启动提权判断都用它。
bool IsProcessElevated();

// 前台窗口所属进程是否提权。提权窗口在前台时，普通权限进程的全局热键会被 UIPI 挡住，
// 诊断页据此给出「开启管理员模式」的提示。读不到前台窗口时按「未提权」处理。
bool IsForegroundElevated();

}
