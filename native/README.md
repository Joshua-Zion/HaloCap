# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支。设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10，工具链与构建前提见文末「S1 的工具链」。

当前状态：**S1 骨架已能构建、core 单测已能跑、规则一已做成机检**。还没有任何产品功能——`platform/` 目录要等第一个真实后端（Direct2D）落地时才出现，不放空壳占位；8 个 seam 的头文件也随对应后端一起定，避免先写好一套再被实现推翻。

```
native/
  core/       纯 C++，只依赖标准库与自家头文件
    geometry.h        Point / Size / Rect / Color 与矩形运算（并、交、归一化、包含）
    display_list.h    显示列表：FillRect / StrokeRect / Blit / GlyphRun / Path，顺序即图层顺序
  tests/      core 的单测（无第三方框架，一个可执行 + CTest）
  platform/   平台实现，按后端分目录（win32/ 等）—— 待建
  build.cmd   vcvars → cmake configure → build → ctest
```

三条来自 §3.3 的规则，以及各自的执行方式：

1. **`core/` 里出现任何平台头文件即构建失败**。`core/CMakeLists.txt` 在 configure 阶段扫描 `core/**` 的 `#include`，命中平台头前缀（`windows.` `d2d1` `dxgi` `mf*` `winrt/` `wrl/` 等）就 `FATAL_ERROR`；`tests/CMakeLists.txt` 再加一道链接期兜底——`halocap_core` 一旦链上任何库（含系统导入库）同样 `FATAL_ERROR`，这条挡的是「头文件没引但真调了 Win32 函数」。两道都做过反向验证：塞一个 `#include <windows.h>` 进去、给 core 加一句 `target_link_libraries(... user32)`，configure 都会失败并指名道姓。规则本身不依赖 CI 是否存在（仓库目前没有 CI）。
2. `core/` 只产显示列表（顶点、图元、字形 run、图层顺序），不直接调用渲染 API。`display_list.h` 是这条规则的载体：`GlyphRun` 只存 `IFontRasterizer` 给的 token 与外部算好的 bounds，排版与测量都在平台侧。
3. 热路径在 `core/` 里只看到「指针 + stride + 尺寸」的裸缓冲，映射与解映射归 `platform/`。`ImageRef{pixels, width, height, stride}` 是这条规则的唯一入口形状。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2。

S0 的三个 spike（编码器纹理直入、桌面复制吞吐、Direct2D 帧成本）已经跑完并给出实测结论，代码在同仓库的 [../spikes/](../spikes/)，不参与任何构建；其中被推翻的写法都已回写进 §4 与 §1.3。

## 构建

前提：MSVC + Windows SDK + CMake + Ninja 已装好（版本与路径见 2.0 计划文档），CMake/Ninja 不在系统 PATH 上也能跑，脚本里用的是绝对路径。

```
native> build.cmd
```

产物在 `native/build/`（不要提交）。单独跑 core 单测：`ctest --test-dir build --output-on-failure`。
