#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace halocap::core {

// S5 的更新清单：一个扁平 JSON（version / url / sha256 / size / notes）。
// 不引入 JSON 库：清单是我们自己写的，字段固定，1.x 的设置解析器也只支持这一子集。
struct UpdateManifest {
    std::string version;
    std::string url;
    std::string sha256;
    std::int64_t size = 0;
    std::string notes;
};

// 只接受扁平对象：字符串/整数/布尔/null。未知字段忽略，缺字段返回 false。
bool ParseUpdateManifest(std::string_view json, UpdateManifest* manifest, std::string* error);

// 语义化版本比较：数字段逐段比，缺段补 0；`2.0.0-beta` < `2.0.0`。
// 返回 <0 / 0 / >0。
int CompareVersions(std::string_view left, std::string_view right);

bool IsNewerVersion(std::string_view candidate, std::string_view current);

// 从 GitHub 的 releases.atom 里取最新版本的 tag（去掉前导 v）。取第一个 <entry> 的
// /releases/tag/<tag> 链接；找不到或 tag 不像版本号时返回 false。
bool ParseLatestTagFromAtom(std::string_view xml, std::string* tag, std::string* error);

}
