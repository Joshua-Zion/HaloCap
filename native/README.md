# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支。设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10，工具链与构建前提见计划文档的「S1 的工具链」。

当前状态：**S1 骨架可构建、core 单测可跑、§3.3 规则三已做成机检，Direct2D 后端已能按显示列表画出像素并量过帧成本**。还没有产品功能——窗口/覆盖层、字体栅格、采集与编码都还没进来，对应的 seam 也不提前定形（避免先写一套再被实现推翻）。

```
native/
  core/       纯 C++，只依赖标准库与自家头文件
    geometry.h          Point / Size / Rect / IntRect / Color 与矩形运算（并、交、归一化、包含）
    display_list.h      显示列表：FillRect / StrokeRect / Segment / Blit，向量顺序即图层顺序
    render_backend.h    IRenderBackend seam：Resize / RegisterImage / Render(plan, clip) / Flush
  platform/win32/
    d2d_render_backend.h/.cpp   Direct2D 1.1（ID2D1Device + ID2D1DeviceContext）实现
  tools/frame_cost/   覆盖层帧成本测量工具（与 spike 3 同口径）
  tests/      core 的单测（无第三方框架，一个可执行 + CTest）
  build.cmd   vcvars → cmake configure → build → ctest
```

三条硬规则（计划 §3.3）各自的执行方式：

1. **core 只产显示列表**，渲染由后端一次提交。`display_list.h` + `render_backend.h` 是这条规则的载体：`Render(plan, clip)` 一次拿到整张列表，core 侧没有任何渲染 API；`Blit` 只带 `ImageHandle`，因为帧循环里不做纹理上传——裸缓冲只在 `RegisterImage` 这一步跨过边界，之后图元引用的都是它返回的句柄。
2. **热路径只操作裸缓冲**：core 只看得到 `ImageRef{pixels, width, height, stride}`，映射/上传/解绑全在 `platform/`。
3. **可机检**：`core/CMakeLists.txt` 在 configure 阶段扫 `core/**` 的 `#include`，命中平台头前缀（`windows.` `d2d1` `dxgi` `mf*` `winrt/` `wrl/` 等）就 `FATAL_ERROR`；`tests/CMakeLists.txt` 再加一道链接期兜底——`halocap_core` 一旦链上任何库（含系统导入库）同样 `FATAL_ERROR`，这条挡的是「头文件没引但真调了 Win32 函数」。两道都做过反向验证：塞一个 `#include <windows.h>` 进去、给 core 加一句 `target_link_libraries(... user32)`，configure 都会失败并指名道姓。仓库目前没有 CI，所以检查挂在 configure 上，任何人 configure 就跑。

显示列表里**故意没有** `Path` 与 `GlyphRun`：折线要等几何缓存策略定下来，字形 run 要等 `IFontRasterizer` 的图集。放进列表却画不出来比不放更糟。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2；目前已落地的只有 `IRenderBackend`。

## 构建与测量

前提：MSVC + Windows SDK + CMake + Ninja 已装好（版本与路径见计划文档），脚本里用的是绝对路径，不依赖 PATH。

```
native> build.cmd
```

产物在 `native/build/`（已 gitignore）。core 单测单独跑：`ctest --test-dir build --output-on-failure`。

帧成本（离屏渲染，不建窗口、不上屏）：

```
native> build\tools\frame_cost\frame_cost.exe            # 跑 spike 3 的全部五个配置
native> build\tools\frame_cost\frame_cost.exe 3840 2160 120 300   # 单个配置
```

每个配置会先只放一次整面 blit、回读画面中心像素与源图样逐字节比对（`PROBE=EXACT`），再计时；输出的 `SUBMIT_*` 是整面重绘，`PARTIAL_*` 是只压 600×600 脏区 clip，`READBACK_*` 是每帧整帧回读那一档。实测结论与对口径的影响都写进了计划文档 §5.1「spike 3 的原生复测」。
