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

}
