// capability seam 的纯决策：系统下限、日志规则、磁盘下限、空闲回收线、特性开关。
// 这些都来自 1.x 的四个不同文件，现在集中在一处，且不碰文件系统也能验。
#include "capability.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s: expected true\n", what);
    }
}

void CheckText(const std::string& actual, const std::string& expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got \"%s\" want \"%s\"\n", what, actual.c_str(), expected.c_str());
    }
}

void CheckContains(const std::string& haystack, const std::string& needle, const char* what) {
    ++checks;
    if (haystack.find(needle) == std::string::npos) {
        ++failures;
        std::printf("FAIL %s: \"%s\" does not contain \"%s\"\n", what, haystack.c_str(),
                    needle.c_str());
    }
}

void CheckEmpty(const std::string& actual, const char* what) {
    ++checks;
    if (!actual.empty()) {
        ++failures;
        std::printf("FAIL %s: expected empty, got \"%s\"\n", what, actual.c_str());
    }
}

void CheckNames(const std::vector<std::string>& actual, std::initializer_list<const char*> expected,
                const char* what) {
    ++checks;
    bool ok = actual.size() == expected.size();
    if (ok) {
        std::size_t index = 0;
        for (const char* want : expected) {
            if (actual[index] != want) {
                ok = false;
                break;
            }
            ++index;
        }
    }
    if (!ok) {
        ++failures;
        std::printf("FAIL %s: got %zu entries\n", what, actual.size());
    }
}

void TestWindowsFloor() {
    CheckTrue(!core::MeetsWindowsFloor(0), "build 0 is below the floor");
    CheckTrue(!core::MeetsWindowsFloor(18361), "18361 is below the floor");
    CheckTrue(core::MeetsWindowsFloor(18362), "18362 is the floor itself");
    CheckTrue(core::MeetsWindowsFloor(22621), "newer builds meet the floor");

    CheckTrue(!core::SupportsCaptureToggle(19040), "19040 has no capture toggles");
    CheckTrue(core::SupportsCaptureToggle(19041), "19041 is the toggle floor");
    CheckTrue(core::SupportsCaptureToggle(22621), "newer builds have the toggles");

    CheckEmpty(core::DescribeWindowsFloor(18362), "no message when the floor is met");
    CheckEmpty(core::DescribeWindowsFloor(22621), "no message on a new build");
    CheckContains(core::DescribeWindowsFloor(0), "无法确认", "unknown build says so explicitly");
    CheckContains(core::DescribeWindowsFloor(0), "录屏可能无法工作", "unknown build still explains the risk");
    CheckContains(core::DescribeWindowsFloor(18361), "18361", "old build reports its number");
    CheckContains(core::DescribeWindowsFloor(18361), "1903", "old build names the required release");

    CheckEmpty(core::DescribeCursorFallback(19041), "no cursor fallback message when supported");
    CheckContains(core::DescribeCursorFallback(19040), "19040", "cursor fallback reports the build");
    CheckContains(core::DescribeCursorFallback(19040), "2004", "cursor fallback names the required release");
}

void TestFeatures() {
    core::PlatformFacts unknown;
    core::FeatureFlags unknownFlags = core::EvaluateFeatures(unknown);
    CheckTrue(!unknownFlags.recording, "unknown build blocks recording");
    CheckTrue(!unknownFlags.windowRecording, "unknown build blocks window recording");
    CheckTrue(!unknownFlags.cursorToggle, "unknown build blocks the cursor toggle");

    core::PlatformFacts floorOnly;
    floorOnly.osBuild = 18362;
    floorOnly.privateMemoryBytes = 100LL * 1024 * 1024;
    core::FeatureFlags floorFlags = core::EvaluateFeatures(floorOnly);
    CheckTrue(floorFlags.recording, "18362 allows recording");
    CheckTrue(!floorFlags.windowRecording, "18362 without WGC has no window recording");
    CheckTrue(!floorFlags.cursorToggle, "18362 has no cursor toggle");
    CheckTrue(!floorFlags.captureBorderToggle, "18362 has no border toggle");
    CheckTrue(!floorFlags.needsIdleTrim, "100 MB is under the trim line");

    core::PlatformFacts modern;
    modern.osBuild = 22621;
    modern.graphicsCaptureSupported = true;
    modern.privateMemoryBytes = 220LL * 1024 * 1024;
    core::FeatureFlags modernFlags = core::EvaluateFeatures(modern);
    CheckTrue(modernFlags.recording, "22621 allows recording");
    CheckTrue(modernFlags.windowRecording, "WGC allows window recording");
    CheckTrue(modernFlags.cursorToggle, "22621 allows the cursor toggle");
    CheckTrue(modernFlags.captureBorderToggle, "22621 allows the border toggle");
    CheckTrue(modernFlags.needsIdleTrim, "220 MB is on the trim line");

    modern.privateMemoryBytes = 220LL * 1024 * 1024 - 1;
    CheckTrue(!core::EvaluateFeatures(modern).needsIdleTrim, "just under the line does not trim");
}

void TestLogNames() {
    CheckText(core::LogFileName("20261108", 0), "app-20261108.log", "first log of the day");
    CheckText(core::LogFileName("20261108", 1), "app-20261108-2.log", "second log is -2, not -1");
    CheckText(core::LogFileName("20261108", 5), "app-20261108-6.log", "sixth log is -6");
    CheckEmpty(core::LogFileName("20261108", 6), "a seventh log has no name");
    CheckEmpty(core::LogFileName("20261108", -1), "negative part has no name");
    CheckEmpty(core::LogFileName("", 0), "empty day has no name");

    core::LogNameParts parts;
    CheckTrue(core::ParseLogFileName("app-20261108.log", parts), "first log parses");
    CheckText(parts.day, "20261108", "parsed day");
    CheckEqual(parts.part, 0, "parsed part 0");

    CheckTrue(core::ParseLogFileName("app-20261108-2.log", parts), "second log parses");
    CheckEqual(parts.part, 1, "parsed part 1");

    CheckTrue(core::ParseLogFileName("app-20261108-6.log", parts), "sixth log parses");
    CheckEqual(parts.part, 5, "parsed part 5");

    // 1.x 从不写 -1；-7 以上也不可能，别把别的东西算进今天的份数里。
    CheckTrue(!core::ParseLogFileName("app-20261108-1.log", parts), "-1 never exists");
    CheckTrue(!core::ParseLogFileName("app-20261108-7.log", parts), "-7 is out of range");
    CheckTrue(!core::ParseLogFileName("app-20261108-last.log", parts), "capacity notice is not a part");
    CheckTrue(!core::ParseLogFileName("app-2026-11-08.log", parts), "dashed date is not ours");
    CheckTrue(!core::ParseLogFileName("app-2026110.log", parts), "seven-digit day is not ours");
    CheckTrue(!core::ParseLogFileName("app-20261108.log.bak", parts), "backup suffix is not ours");
    CheckTrue(!core::ParseLogFileName("other.log", parts), "foreign file is not ours");
    CheckTrue(!core::ParseLogFileName("", parts), "empty name is not ours");
}

void TestLogTarget() {
    core::LogTarget fresh = core::DecideLogTarget("20261108", {});
    CheckText(fresh.name, "app-20261108.log", "no files means the first part");
    CheckTrue(!fresh.atCapacity, "no files is not capacity");

    core::LogTarget oneFull = core::DecideLogTarget("20261108", { { 0, core::kLogMaxFileBytes } });
    CheckText(oneFull.name, "app-20261108-2.log", "a full first part moves to the second");
    CheckEqual(oneFull.part, 1, "second part index");

    core::LogTarget justUnder =
        core::DecideLogTarget("20261108", { { 0, core::kLogMaxFileBytes - 1 } });
    CheckText(justUnder.name, "app-20261108.log", "one byte under the limit still appends");

    core::LogTarget gap = core::DecideLogTarget("20261108", { { 0, core::kLogMaxFileBytes },
                                                             { 2, 100 } });
    CheckText(gap.name, "app-20261108-2.log", "a missing middle part is chosen before a later one");

    std::vector<core::LogPartSize> allFull;
    for (int part = 0; part < core::kLogMaxPartsPerDay; ++part) {
        allFull.push_back({ part, core::kLogMaxFileBytes });
    }
    core::LogTarget full = core::DecideLogTarget("20261108", allFull);
    CheckTrue(full.atCapacity, "six full parts mean capacity");
    CheckEmpty(full.name, "capacity has no file name");

    // 越界的 part 不进统计。
    core::LogTarget outliers = core::DecideLogTarget("20261108", { { 7, core::kLogMaxFileBytes },
                                                                  { -1, 5 } });
    CheckText(outliers.name, "app-20261108.log", "out-of-range parts are ignored");
}

void TestLogPruning() {
    constexpr std::int64_t kNow = 1'800'000'000LL;
    constexpr std::int64_t kDay = 86400;

    const std::vector<core::LogFileEntry> files = {
        { "app-20200101.log", 100, kNow - 15 * kDay },
        { "app-20200102.log", 100, kNow - 14 * kDay },   // 正好在边界上：1.x 用 < 判，留下
        { "app-20200103.log", 100, kNow - 1 },
    };

    std::vector<std::string> expired = core::SelectExpiredLogs(files, kNow, core::kLogRetainDays);
    CheckNames(expired, { "app-20200101.log" }, "only logs older than the retention window expire");

    // 0 或负数按 1 天算：不能因为写错一个参数就把日志全删了。1 天的下限一收紧，
    // 14 天前与 15 天前的那两份都过期，只有 1 天内的那份留下。
    CheckNames(core::SelectExpiredLogs(files, kNow, 0),
               { "app-20200101.log", "app-20200102.log" }, "retain 0 is clamped to one day");
    CheckTrue(core::SelectExpiredLogs(files, kNow, 100).empty(), "a long window keeps everything");
    CheckTrue(core::SelectExpiredLogs({}, kNow, 1).empty(), "no files, nothing to prune");
}

void TestDiskFloor() {
    CheckEqual(core::RecordingFreeSpaceFloorBytes(0), 200'000'000, "no bitrate uses the 200 MB floor");
    CheckEqual(core::RecordingFreeSpaceFloorBytes(-1), 200'000'000, "negative bitrate is clamped");
    // 27,371,520 bit/s ÷ 8 × 60 = 205,286,400，比 200 MB 大，所以按一分钟的码流算。
    CheckEqual(core::RecordingFreeSpaceFloorBytes(27'371'520), 205'286'400,
               "4K30 balanced needs one minute of bitrate");
    CheckEqual(core::RecordingFreeSpaceFloorBytes(45'000'000), 337'500'000,
               "the maximum bitrate needs 337.5 MB");

    core::DiskSpaceVerdict tight = core::EvaluateDiskSpace(205'286'399, 27'371'520);
    CheckTrue(!tight.enough, "one byte under the floor is not enough");
    CheckEqual(tight.requiredBytes, 205'286'400, "verdict reports the requirement");

    CheckTrue(core::EvaluateDiskSpace(205'286'400, 27'371'520).enough, "exactly the floor is enough");
    CheckTrue(!core::EvaluateDiskSpace(0, 0).enough, "a full disk is not enough");

    // 读不到可用空间时（-1）不能当成「盘满了」：1.x 只记日志、照常开录。
    CheckTrue(core::EvaluateDiskSpace(-1, 27'371'520).enough, "unknown free space does not block");
}

void TestIdleTrim() {
    CheckTrue(!core::ShouldTrimAfterIdle(0), "zero memory does not trim");
    CheckTrue(!core::ShouldTrimAfterIdle(core::kIdleTrimThresholdBytes - 1),
              "just under 220 MB does not trim");
    CheckTrue(core::ShouldTrimAfterIdle(core::kIdleTrimThresholdBytes), "exactly 220 MB trims");
    CheckTrue(core::ShouldTrimAfterIdle(500LL * 1024 * 1024), "well over the line trims");
    CheckEqual(core::kIdleTrimDelayMs, 1200, "the idle trim waits 1.2 s after activity");
}

}

int main() {
    TestWindowsFloor();
    TestFeatures();
    TestLogNames();
    TestLogTarget();
    TestLogPruning();
    TestDiskFloor();
    TestIdleTrim();

    if (failures != 0) {
        std::printf("CAPABILITY_POLICY_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("CAPABILITY_POLICY_TESTS=OK checks=%d\n", checks);
    return 0;
}
