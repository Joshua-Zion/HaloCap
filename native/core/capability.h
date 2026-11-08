#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 8 个 seam 里的第八个（`IStore` + `ICapability`）。这个头是 ICapability 的 core 半边：
// 平台只汇报事实（系统 build、WGC 支不支持、私有内存多少、某个路径还有多少空间），
// 「低于下限该怎么办」「日志留几份」「磁盘剩多少才允许开录」全在这里定。
//
// 这一条分工不是洁癖：1.x 把这些数字散在 WindowsCompatibility、Log、RecordingService、
// App.xaml.cs 四处，其中日志与磁盘两条还有各自独立的小测试。搬到 core 之后它们能在一处
// 被单测钉住，平台侧只剩「读一个数」。
//
// 所有数值都是 1.x 的原值，不是重新拍的：

// ---- 系统版本下限（1.x Diagnostics/WindowsCompatibility.cs） ----
//
// 声明支持 Windows 10 1903（build 18362）起。WGC 的光标/无边框开关随 2004（build 19041）
// 加入，所以那一档叫「切换开关」，不是第二个下限。
constexpr int kMinimumWindowsBuild = 18362;
constexpr int kCaptureToggleBuild = 19041;

bool MeetsWindowsFloor(int build);
bool SupportsCaptureToggle(int build);

// 低于下限时给用户的说明；满足要求时返回空串。build 为 0 表示读不出系统版本，
// 那句话与「版本太旧」不同——不能把它当成一个具体的旧版本号去说。
std::string DescribeWindowsFloor(int build);

// 「设置里要求隐藏光标，但系统不支持」时的说明；支持时返回空串。
std::string DescribeCursorFallback(int build);

// ---- 平台事实与由此得出的特性开关 ----

struct PlatformFacts {
    int osBuild = 0;

    // Windows.Graphics.Capture 可用（窗口录制需要它；区域/显示器录制走桌面复制，不需要）。
    bool graphicsCaptureSupported = false;

    // 进程当前私有内存，供空闲回收判断。
    std::int64_t privateMemoryBytes = 0;
};

struct FeatureFlags {
    bool recording = false;         // 过支持下限即可录屏（桌面复制从 Win8 就有）
    bool windowRecording = false;   // 窗口录制还要求 WGC
    bool cursorToggle = false;      // 19041+ 才能关掉录进去的光标
    bool captureBorderToggle = false;
    bool needsIdleTrim = false;     // 私有内存已过 220 MB 的线
};

FeatureFlags EvaluateFeatures(const PlatformFacts& facts);

// ICapability 的接口本身。Probe 只读一次（系统 build 不会变，内存会变，所以 Probe
// 是「现在拍一张」，调用方决定多久拍一次）。
class ICapability {
public:
    virtual ~ICapability() = default;

    virtual PlatformFacts Probe() = 0;

    // 该路径所在卷的可用字节；路径不存在时退到它的卷根再问一次；都读不到返回 -1。
    // 用 UTF-8 传路径，平台侧负责转宽字符。
    virtual std::int64_t AvailableBytes(const char* utf8Path) = 0;
};

// ---- 日志规则（1.x Diagnostics/Log.cs 的原值） ----
//
// 1.x 的老毛病：日志只按天新建、从不清理也不限长，一次异常刷屏就能把磁盘写满。
// 这几条规则就是当时的补丁，现在要能在没有文件系统的情况下被验证。

constexpr std::int64_t kLogMaxFileBytes = 8LL * 1024 * 1024;
constexpr int kLogMaxPartsPerDay = 6;
constexpr int kLogRetainDays = 14;

struct LogFileEntry {
    std::string name;
    std::int64_t sizeBytes = 0;
    std::int64_t writtenUnixSeconds = 0;
};

struct LogNameParts {
    std::string day;   // 八位日期，"20261108"
    int part = 0;      // 0 = app-YYYYMMDD.log，1..5 = app-YYYYMMDD-2.log … -6.log
};

// 名字的解析与生成放在 core：平台只列出目录里有什么，不猜哪一份是今天的。
bool ParseLogFileName(std::string_view name, LogNameParts& out);
std::string LogFileName(std::string_view day, int part);

struct LogPartSize {
    int part = 0;
    std::int64_t sizeBytes = 0;
};

// 本日下一次该往哪一份写。六份都写满时 name 为空、atCapacity 为真（1.x 此时往
// app-YYYYMMDD-last.log 写一条「已达上限」，之后就不再写）。
struct LogTarget {
    std::string name;
    int part = 0;
    bool atCapacity = false;
};

LogTarget DecideLogTarget(std::string_view day, const std::vector<LogPartSize>& todayFiles);

// 最后写入时间早于 now - retainDays（至少留 1 天）的那些日志文件名。
std::vector<std::string> SelectExpiredLogs(const std::vector<LogFileEntry>& files,
                                           std::int64_t nowUnixSeconds, int retainDays);

// ---- 录制前的磁盘下限（1.x RecordingService.EnsureFreeSpace） ----
//
// 下限是 max(200 MB, 一分钟的码流)：小码率也要留出足够余量，大码率则按一分钟算。
constexpr std::int64_t kRecordingMinFreeBytes = 200'000'000;

std::int64_t RecordingFreeSpaceFloorBytes(std::int64_t bitrateBitsPerSec);

struct DiskSpaceVerdict {
    bool enough = true;
    std::int64_t requiredBytes = 0;
    std::int64_t availableBytes = 0;
};

DiskSpaceVerdict EvaluateDiskSpace(std::int64_t availableBytes, std::int64_t bitrateBitsPerSec);

// ---- 空闲回收（1.x App.xaml.cs） ----
//
// 1.x 的教训有两条：过线才动手（空闲时本来就稳定在 250 MB，无条件回收只是空转），
// 以及在两次操作之间留 1.2 秒再动手（刚截完图就回收会把还在用的位图抽走）。
constexpr std::int64_t kIdleTrimThresholdBytes = 220LL * 1024 * 1024;
constexpr std::int64_t kIdleTrimDelayMs = 1200;

bool ShouldTrimAfterIdle(std::int64_t privateMemoryBytes);

// §1.3 的空闲内存目标，也是 S1 的退出条件：托盘、设置、日志与覆盖层都起来之后，
// 私有内存不该过 80 MB（此时还没有 OCR 与编码器）。
//
// 与上面那条 220 MB 不是一回事：220 MB 是「过线才回收」，80 MB 是「本来就不该过线」。
// 这条阈值放在 core，是因为它要被 app 的自检当成可执行的断言，而不只是文档里的一句话。
constexpr std::int64_t kIdleMemoryBudgetBytes = 80LL * 1024 * 1024;

bool WithinIdleBudget(std::int64_t privateMemoryBytes);

}
