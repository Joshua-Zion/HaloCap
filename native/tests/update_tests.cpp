// S5 的更新清单与版本比较：扁平 JSON、语义化版本、预发布版排序。
#include "update.h"

#include <cstdio>
#include <string>

namespace {

int failures = 0;
int checks = 0;

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void TestManifest() {
    const char* json =
        "{\n"
        "  \"version\": \"2.0.1\",\n"
        "  \"url\": \"https://example.com/HaloCap-2.0.1-win64.zip\",\n"
        "  \"sha256\": \"abc123\",\n"
        "  \"size\": 12345678,\n"
        "  \"notes\": \"fixes \\\"things\\\"\"\n"
        "}";

    halocap::core::UpdateManifest manifest;
    std::string error;
    CheckTrue(halocap::core::ParseUpdateManifest(json, &manifest, &error), "manifest parses");
    CheckTrue(manifest.version == "2.0.1", "manifest version");
    CheckTrue(manifest.url.rfind("https://", 0) == 0, "manifest url");
    CheckTrue(manifest.sha256 == "abc123", "manifest sha256");
    CheckEqual(manifest.size, 12345678, "manifest size");
    CheckTrue(manifest.notes == "fixes \"things\"", "manifest notes escape");

    halocap::core::UpdateManifest missing;
    CheckTrue(!halocap::core::ParseUpdateManifest("{\"version\":\"2.0.1\"}", &missing, &error),
              "manifest without url is rejected");
    CheckTrue(!halocap::core::ParseUpdateManifest("{\"url\":\"x\"}", &missing, &error),
              "manifest without version is rejected");
    CheckTrue(!halocap::core::ParseUpdateManifest("not json", &missing, &error),
              "garbage is rejected");
}

void TestVersions() {
    using halocap::core::CompareVersions;
    CheckEqual(CompareVersions("2.0.0", "2.0.0"), 0, "equal versions");
    CheckEqual(CompareVersions("2.0.1", "2.0.0"), 1, "patch bump is newer");
    CheckEqual(CompareVersions("2.0", "2.0.0"), 0, "missing component is zero");
    CheckEqual(CompareVersions("2.1.0", "2.0.9"), 1, "minor bump wins");
    CheckEqual(CompareVersions("10.0.0", "9.9.9"), 1, "numeric, not lexical");
    CheckEqual(CompareVersions("2.0.0-beta", "2.0.0"), -1, "prerelease is older");
    CheckEqual(CompareVersions("2.0.0", "2.0.0-beta"), 1, "release is newer than prerelease");

    CheckTrue(halocap::core::IsNewerVersion("2.0.1", "2.0.0"), "2.0.1 is an update");
    CheckTrue(!halocap::core::IsNewerVersion("2.0.0", "2.0.0"), "same version is not an update");
    CheckTrue(!halocap::core::IsNewerVersion("1.9.9", "2.0.0"), "older is not an update");
}

void TestAtomFeed() {
    const std::string feed = R"(<?xml version="1.0" encoding="UTF-8"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <id>tag:github.com,2008:Repository/1/v2.1.0</id>
    <updated>2026-11-11T10:00:00Z</updated>
    <link rel="alternate" type="text/html" href="https://github.com/Joshua-Zion/HaloCap/releases/tag/v2.1.0"/>
    <title>HaloCap v2.1.0</title>
  </entry>
  <entry>
    <id>tag:github.com,2008:Repository/1/v2.0.1</id>
    <link rel="alternate" type="text/html" href="https://github.com/Joshua-Zion/HaloCap/releases/tag/v2.0.1"/>
    <title>HaloCap v2.0.1</title>
  </entry>
</feed>)";

    std::string tag;
    std::string error;
    CheckTrue(halocap::core::ParseLatestTagFromAtom(feed, &tag, &error), "atom feed finds a tag");
    CheckTrue(tag == "2.1.0", "atom feed takes the newest tag and strips v");

    CheckTrue(!halocap::core::ParseLatestTagFromAtom("<feed></feed>", &tag, &error),
              "atom without entry is rejected");
    CheckTrue(!halocap::core::ParseLatestTagFromAtom("<feed><entry></entry></feed>", &tag, &error),
              "entry without tag link is rejected");
    const std::string badTag =
        "<feed><entry><link href=\"https://github.com/o/r/releases/tag/latest\"/></entry></feed>";
    CheckTrue(!halocap::core::ParseLatestTagFromAtom(badTag, &tag, &error),
              "non-version tag is rejected");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestManifest();
    TestVersions();
    TestAtomFeed();

    if (failures != 0) {
        std::printf("UPDATE_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }
    std::printf("UPDATE_TESTS=OK checks=%d\n", checks);
    return 0;
}
