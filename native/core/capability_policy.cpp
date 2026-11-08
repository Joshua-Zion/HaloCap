#include "capability.h"

#include <algorithm>
#include <cstdio>

namespace halocap::core {

namespace {

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

}  // namespace

// ---- 系统下限 ----

bool MeetsWindowsFloor(int build) { return build >= kMinimumWindowsBuild; }

bool SupportsCaptureToggle(int build) { return build >= kCaptureToggleBuild; }

std::string DescribeWindowsFloor(int build) {
    if (MeetsWindowsFloor(build)) {
        return {};
    }

    if (build == 0) {
        return "无法确认 Windows 版本，HaloCap 需要 Windows 10 1903 或更高版本；录屏可能无法工作。";
    }

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "HaloCap 需要 Windows 10 1903 或更高版本（当前 build %d）。"
                  "截图、文字识别和贴图仍可使用，录屏可能无法工作。",
                  build);
    return buffer;
}

std::string DescribeCursorFallback(int build) {
    if (SupportsCaptureToggle(build)) {
        return {};
    }

    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "当前 Windows 版本（build %d）不支持关闭录制光标，画面里会始终带上光标；"
                  "升级到 Windows 10 2004 以上后此设置才会生效。",
                  build);
    return buffer;
}

// ---- 特性开关 ----

FeatureFlags EvaluateFeatures(const PlatformFacts& facts) {
    FeatureFlags flags;
    // 区域与显示器录制走桌面复制，Win8 就有；真正的门槛是「我们敢支持的版本」。
    flags.recording = MeetsWindowsFloor(facts.osBuild);
    flags.windowRecording = flags.recording && facts.graphicsCaptureSupported;
    flags.cursorToggle = SupportsCaptureToggle(facts.osBuild);
    flags.captureBorderToggle = flags.cursorToggle;
    flags.needsIdleTrim = ShouldTrimAfterIdle(facts.privateMemoryBytes);
    return flags;
}

// ---- 日志文件名 ----

std::string LogFileName(std::string_view day, int part) {
    if (day.empty() || part < 0 || part >= kLogMaxPartsPerDay) {
        return {};
    }

    std::string name = "app-";
    name += day;
    if (part > 0) {
        // 第二份起带序号，且序号是「第几份」而不是「下标 + 1 之后再加一」：part=1 叫 -2。
        name += '-';
        name += std::to_string(part + 1);
    }
    name += ".log";
    return name;
}

bool ParseLogFileName(std::string_view name, LogNameParts& out) {
    constexpr std::string_view kPrefix = "app-";
    constexpr std::string_view kSuffix = ".log";

    if (name.size() < kPrefix.size() + 8 + kSuffix.size() ||
        name.substr(0, kPrefix.size()) != kPrefix ||
        name.substr(name.size() - kSuffix.size()) != kSuffix) {
        return false;
    }

    std::string_view middle = name.substr(kPrefix.size(), name.size() - kPrefix.size() - kSuffix.size());

    // 日期必须是八位数字：日志目录里可能还有人手放的别的文件，不该被当成日志。
    if (middle.size() < 8) {
        return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (!IsDigit(middle[i])) {
            return false;
        }
    }

    LogNameParts parsed;
    parsed.day = std::string(middle.substr(0, 8));

    const std::string_view tail = middle.substr(8);
    if (tail.empty()) {
        parsed.part = 0;
    } else {
        if (tail.front() != '-' || tail.size() < 2) {
            return false;
        }
        int number = 0;
        for (std::size_t i = 1; i < tail.size(); ++i) {
            if (!IsDigit(tail[i])) {
                return false;
            }
            number = number * 10 + (tail[i] - '0');
        }
        // 1.x 的序号从 2 开始（第一份不带序号）；1 与 7 以上都不可能由我们写出来。
        if (number < 2 || number > kLogMaxPartsPerDay) {
            return false;
        }
        parsed.part = number - 1;
    }

    out = parsed;
    return true;
}

LogTarget DecideLogTarget(std::string_view day, const std::vector<LogPartSize>& todayFiles) {
    std::int64_t sizes[kLogMaxPartsPerDay] = {};
    for (const LogPartSize& file : todayFiles) {
        if (file.part >= 0 && file.part < kLogMaxPartsPerDay) {
            sizes[file.part] = file.sizeBytes;
        }
    }

    for (int part = 0; part < kLogMaxPartsPerDay; ++part) {
        if (sizes[part] < kLogMaxFileBytes) {
            return { LogFileName(day, part), part, false };
        }
    }

    return { std::string{}, kLogMaxPartsPerDay, true };
}

std::vector<std::string> SelectExpiredLogs(const std::vector<LogFileEntry>& files,
                                           std::int64_t nowUnixSeconds, int retainDays) {
    // 与 1.x 一致：保留天数至少按 1 天算，写 0 或负数不能变成「什么都删」。
    const std::int64_t days = std::max(1, retainDays);
    const std::int64_t cutoff = nowUnixSeconds - days * 86400;

    std::vector<std::string> expired;
    expired.reserve(files.size());
    for (const LogFileEntry& file : files) {
        if (file.writtenUnixSeconds < cutoff) {
            expired.push_back(file.name);
        }
    }
    return expired;
}

// ---- 磁盘下限 ----

std::int64_t RecordingFreeSpaceFloorBytes(std::int64_t bitrateBitsPerSec) {
    const std::int64_t oneMinute = std::max<std::int64_t>(0, bitrateBitsPerSec) / 8 * 60;
    return std::max(kRecordingMinFreeBytes, oneMinute);
}

DiskSpaceVerdict EvaluateDiskSpace(std::int64_t availableBytes, std::int64_t bitrateBitsPerSec) {
    DiskSpaceVerdict verdict;
    verdict.requiredBytes = RecordingFreeSpaceFloorBytes(bitrateBitsPerSec);
    verdict.availableBytes = availableBytes;
    // 读不出可用空间时 available 是 -1：1.x 在这种情况下只记日志、照常开录，
    // 因为「盘不可读」和「盘满了」不是一回事，不能把前者当成后者拦下来。
    verdict.enough = availableBytes < 0 || availableBytes >= verdict.requiredBytes;
    return verdict;
}

// ---- 空闲回收 ----

bool ShouldTrimAfterIdle(std::int64_t privateMemoryBytes) {
    return privateMemoryBytes >= kIdleTrimThresholdBytes;
}

bool WithinIdleBudget(std::int64_t privateMemoryBytes) {
    return privateMemoryBytes <= kIdleMemoryBudgetBytes;
}

}
