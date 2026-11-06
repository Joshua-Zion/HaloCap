# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支。设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10，工具链与构建前提见计划文档的「S1 的工具链」。

当前状态：**S1 骨架可构建、core 单测可跑、§3.3 规则三已做成机检；显示列表已能离屏逐字节画对，也已能经 DirectComposition 真窗口 Present 上屏链**。还没有产品功能——采集与编码、热键与输入、托盘与设置都还没进来，对应的 seam 也不提前定形（避免先写一套再被实现推翻）。

```
native/
  core/       纯 C++，只依赖标准库与自家头文件
    geometry.h          Point / Size / Rect / IntRect / Color 与矩形运算（并、交、归一化、包含）
    display_list.h      显示列表：FillRect / StrokeRect / Segment / Blit，向量顺序即图层顺序；
                        AppendIntersecting 按脏区裁图元（只压 clip 省不掉命令提交的成本）
    render_backend.h    IRenderBackend seam：Resize / RegisterImage / Render(plan, clip) / Flush /
                        AttachWindow / DetachWindow / Present
    font_rasterizer.h   IFontRasterizer seam：FontSpec（字体+字重+字号+颜色）/ TextCell / LayOut
    surface_window.h    ISurfaceWindow seam：Create / SetBounds / SetVisible / SetClickThrough / Handle
  platform/win32/
    d2d_render_backend.h/.cpp      Direct2D 1.1（ID2D1Device + ID2D1DeviceContext）+ 合成链呈现
    d2d_font_rasterizer.h/.cpp     DirectWrite 排版 + 图集分页，格子=一个文本串
    win32_surface_window.h/.cpp    WS_POPUP + WS_EX_NOREDIRECTIONBITMAP，不激活、可透传
  tools/frame_cost/     覆盖层帧成本测量（与 spike 3 同口径，离屏、不上屏）
  tools/surface_probe/  上屏链路验证与 Present 成本（窗口默认摆屏幕外）
  tests/      core 的单测（无第三方框架，一个可执行 + CTest）
  build.cmd   vcvars → cmake configure → build → ctest
```

三条硬规则（计划 §3.3）各自的执行方式：

1. **core 只产显示列表**，渲染由后端一次提交。`display_list.h` + `render_backend.h` 是这条规则的载体：`Render(plan, clip)` 一次拿到整张列表，core 侧没有任何渲染 API；`Blit` 只带 `ImageHandle`，因为帧循环里不做纹理上传——裸缓冲只在 `RegisterImage` 这一步跨过边界，之后图元引用的都是它返回的句柄。
2. **热路径只操作裸缓冲**：core 只看得到 `ImageRef{pixels, width, height, stride}`，映射/上传/解绑全在 `platform/`。
3. **可机检**：`core/CMakeLists.txt` 在 configure 阶段扫 `core/**` 的 `#include`，命中平台头前缀（`windows.` `d2d1` `dxgi` `mf*` `winrt/` `wrl/` 等）就 `FATAL_ERROR`；`tests/CMakeLists.txt` 再加一道链接期兜底——`halocap_core` 一旦链上任何库（含系统导入库）同样 `FATAL_ERROR`，这条挡的是「头文件没引但真调了 Win32 函数」。两道都做过反向验证：塞一个 `#include <windows.h>` 进去、给 core 加一句 `target_link_libraries(... user32)`，configure 都会失败并指名道姓。仓库目前没有 CI，所以检查挂在 configure 上，任何人 configure 就跑。

文本不新增图元类型：`IFontRasterizer::LayOut` 返回的 `TextCell` 只能以 `ToBlit()` 进入显示列表，所以 bounds、脏区裁剪与后端都不用为文本特判。折线（`Path`）**仍然故意不在列表里**——它要等几何缓存策略定下来，放进列表却画不出来比不放更糟。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2；目前已落地 `IRenderBackend`、`IFontRasterizer`、`ISurfaceWindow` 三个。

上屏这条路有个必须记在实现旁边的实测结论：**挂在 HWND 上的 flip 交换链不接受预乘 alpha**（9 个组合 × 4 块适配器 × 3 种扩展样式全 `DXGI_ERROR_INVALID_CALL`），透明只能走 **DirectComposition 的 composition 交换链**，且它要求 `DXGI_SCALING_STRETCH`、后缓冲当 D2D 目标时还要 `TARGET | CANNOT_DRAW`。所以窗口用 `WS_EX_NOREDIRECTIONBITMAP`（不是 `WS_EX_LAYERED`），呈现归后端。完整矩阵与三条 API 细节见计划文档 §5.1「S1 的上屏路径」。

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

每个配置会先只放一次整面 blit、回读画面中心像素与源图样逐字节比对（`PROBE=EXACT`），再验证文本：只画那一格必须留下墨迹（`TEXT_INK_PIXELS`）、什么都不画必须是 0（`EMPTY_INK_PIXELS`）——后者同时验证了「每帧只清脏区」的 Clear 语义确实生效。计时部分 `SUBMIT_*` 是整面重绘（每帧含 spike 3 那一行文字），`PARTIAL_*` 是只压 600×600 脏区 clip（图元照旧全提交），`PARTIAL_CULLED_*` 是先用 `core::AppendIntersecting` 按脏区裁图元再压同一个 clip，`READBACK_*` 是每帧整帧回读那一档，`TEXT_FRESH_LAYOUT_US` / `TEXT_CACHED_LAYOUT_US` 是新串排版与缓存命中的成本。实测结论与对口径的影响都写进计划文档 §5.1 的两节复测记录。

上屏链路（窗口默认摆在屏幕外 -32000、不激活、不吃鼠标，桌面上看不到）：

```
native> build\tools\surface_probe\surface_probe.exe                    # 1920x1080，屏外
native> build\tools\surface_probe\surface_probe.exe 3840 2160 120 1    # 真上一次屏（会占桌面）
```

命令行第 4 个参数：`0`=屏外可见、`1`=屏上可见、`2`=屏上但不显示。输出 `ATTACH_HR`（整条 DirectComposition 链）、`PRESENT_ASYNC_MS_PER_FRAME` / `PRESENT_SYNC_MS_PER_FRAME`、私有内存与 detach 后的变化，并明确打一行 `UNVERIFIED=`——`WS_EX_NOREDIRECTIONBITMAP` 没有重定向位图，`PrintWindow` 这类 CPU 手段拿不到内容，「屏幕上真看得见」只能靠一次真实上屏截图，这是该路径目前唯一未证的环节。
