# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支。设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10，工具链与构建前提见计划文档的「S1 的工具链」。

当前状态：**S1–S5 完成，S4 收尾项也已补齐**（热键捕获、结果窗口命中高亮/按行跳转、同进程 OCR 后内存回落）——十八套测试全绿；`HaloCap.exe` 有设置、日志、托盘、热键、覆盖层、录制、截图、OCR、六页设置与剪贴板贴图（托盘「录屏」/`Ctrl+Alt+R`、托盘「截图」/`Ctrl+Alt+A`、托盘「OCR」/`Ctrl+Alt+O`、托盘「贴图」/`Ctrl+Alt+P`、托盘「设置」；`--record-seconds` / `--screenshot-selftest` / `--ocr-selftest` / `--ocr-search-selftest` / `--settings-selftest` / `--hotkey-selftest` / `--pin-selftest` 七个自检，另有 `--version` / `--check-update`）。交付：`package.ps1` 产出 win64 zip（目标 ≤ 20 MB）与 SHA-256 清单；空闲 2.9 MB、OCR 后回落 42.1 MB、4K30 峰值 230.8 MB、截图选区 78.2 ms。

一处环境阻塞要写清：本机现在对**任何**程序都拒绝 `DuplicateOutput`（`E_ACCESSDENIED`；今天早些时候还能跑出吞吐数字的 spike 2 工具现在同样被拒），所以 `ICaptureSource` 的端到端脏矩形量级数字还没拿到。拷贝逻辑本身不依赖复制权，已经单独验过（`rect_copy_test`）。

```
native/
  core/       纯 C++，只依赖标准库与自家头文件
    geometry.h          Point / Size / Rect / IntRect / Color 与矩形运算（并、交、归一化、包含）
    display_list.h      显示列表：FillRect / StrokeRect / Segment / Blit，向量顺序即图层顺序；
                        AppendIntersecting 按脏区裁图元（只压 clip 省不掉命令提交的成本）
    render_backend.h    IRenderBackend seam：Resize / RegisterImage / Render(plan, clip) / Flush /
                        AttachWindow / DetachWindow / Present / WaitForFrameSlot
    font_rasterizer.h   IFontRasterizer seam：FontSpec（字体+字重+字号+颜色）/ TextCell / LayOut
    surface_window.h    ISurfaceWindow seam：Create / SetBounds / SetVisible / SetClickThrough / Handle
    capture_source.h    ICaptureSource seam：Open / WaitForUpdate / LastErrorKind；
                        SummarizeDirtyRects 与 BackoffUs 是纯逻辑（面积要分「实搬量」和「包围盒」两个数）
    encoder.h           IEncoder seam：OpenVideo / Submit(句柄+媒体时间) / Close / EncoderStats；
                        RecordingClock（暂停冻结媒体时间）与 FrameGate（时间戳不递减 + 不超帧率）在同一处
    audio_source.h      IAudioSource seam：Open / WaitForPacket / ReleasePacket / Format；
                        StereoRingBuffer（写满丢最旧）、MixInto、RemixToStereo、线性重采样、AAC 块补齐
    input_host.h        IInputHost seam：RegisterHotkey / SetInterception / PollHotkey / SuspendIme；
                        KeySymbol 76 个符号、设置文本双向换算、MatchesHotkey 与 DecideSwallow
    settings.h          IStore 的 core 半边：扁平设置树 + JSON 子集、
                        与 1.x 逐项对齐的 schema 与默认值、两字段热键的迁移、逐键修复报告
    store.h             IStore seam：设置文本的原子读写、日志追加/枚举/删除；
                        以及「文件不存在 / 读不出 / 坏内容」三者的分开处理
    capability.h        ICapability 的 core 半边：系统下限、日志规则（8 MiB×6 份×14 天）、
                        磁盘下限、空闲回收线、特性开关；平台只汇报事实
    tray_menu.h         托盘与圆盘共用：AppAction 四项、菜单项/可用性/勾选规则、
                        提示文字与 63 个 UTF-16 码元的截断
    recording_session.h 录制编排：采集帧 → 时钟 → 帧闸门 → 编码提交；暂停/停止与统计口径
    annotation.h        S3 标注模型：九种工具、样式、命中测试、快照式撤销/重做、显示列表生成
    selection.h         选区状态机：拖动、八个把手、移动、夹边界、Esc 取消
    mosaic.h            马赛克：块对齐、块平均降采样（每格一个像素）
    ocr.h               OCR 前后处理：检测输入、DBNet 后处理、裁切、CTC 解码、按行排版与搜索
    ocr_engine.h        OCR 引擎 seam（IOcrEngine）：模型目录、识别入口
    update.h            更新清单解析与语义化版本比较（--check-update 用）
    clipboard.h         剪贴板贴图：颜色解析、CF_HTML 取文本、快照优先级、文件批次与粘贴序列
    frame_scheduler.h   帧节奏与丢帧口径：BeginFrame / EndFrame / Stats，时钟由调用方注入微秒
  platform/win32/
    d2d_render_backend.h/.cpp      Direct2D 1.1（ID2D1Device + ID2D1DeviceContext）+ 合成链呈现
    d2d_font_rasterizer.h/.cpp     DirectWrite 排版 + 图集分页，格子=一个文本串
    win32_surface_window.h/.cpp    WS_POPUP + WS_EX_NOREDIRECTIONBITMAP，不激活、可透传
    dxgi_desktop_duplication.h/.cpp 桌面复制；CopyDirtyRegions 单独抽出以便脱离复制权做验证
    nv12_converter.h/.cpp          D3D11 视频处理器做 BGRA→NV12，NV12 三帧环，bind flag 按实测候选试
    mf_h264_encoder.h/.cpp         IMFSinkWriter + MF_SINK_WRITER_D3D_MANAGER + NV12 样本，从服务属性读编码器身份
    wasapi_audio_source.h/.cpp     共享模式事件驱动；回环加 AUDCLNT_STREAMFLAGS_LOOPBACK
    win32_input_host.h/.cpp            message-only 窗口 + RegisterHotKey + WH_KEYBOARD_LL + ImmAssociateContext
    win32_store.h/.cpp                 IStore：%APPDATA%\HaloCap2、.tmp + MoveFileExW 原子替换、日志目录
    win32_capability.h/.cpp            ICapability：RtlGetVersion + WGC 探测 + 私有内存 + 卷剩余空间
    win32_tray.h/.cpp                  托盘：Shell_NotifyIcon + 标准 HMENU + CreateIconIndirect 画的「光圈」
    win32_wgc_capture.h/.cpp           窗口录制：Windows.Graphics.Capture + 常驻纹理（奇数尺寸按偶数裁）
    image_export.h/.cpp                导出：WIC 写 PNG/JPEG、CF_DIB 写剪贴板（调用方需 CoInitializeEx）
    desktop_grab.h/.cpp                截图抓屏：GDI BitBlt 主显示器（本机桌面复制回读全黑，见 §5.1）
    ort_ocr_engine.h/.cpp              ONNX Runtime 的 PP-OCR 引擎：det + cls + rec，模型来自 1.x assets
    winrt_ocr_engine.h/.cpp            系统 Windows.Media.Ocr 引擎（第二引擎，无需模型）
    win32_clipboard.h/.cpp             剪贴板读取（CF_HDROP/CF_DIB/PNG/HTML/文本）、WIC 图片、GDI 卡片
  tools/frame_cost/     覆盖层帧成本测量（与 spike 3 同口径，离屏、不上屏）
  tools/surface_probe/  上屏链路验证：A/B 抓屏证「看得见」+ 60 fps 排帧数丢帧
  tools/capture_probe/  采集链路统计：应有/实取、仅鼠标唤醒、脏像素之比、每帧 CPU 与内存
  tools/encoder_probe/  编码链路验证：颜色回读证转换对、五档组合量成本/字节/内存与编码器身份
  tools/audio_probe/    音频链路验证：端点形状、静音时出不出包、Initialize 时序矩阵、借还契约
  tools/input_probe/    输入链路验证：热键投递、两个钩子链量拦截是否真生效、IME 断开恢复、错误分类
  tools/store_probe/    设置链路验证：真实路径、原子替换、日志分份、只读导入 1.x 设置、能力读数
  tools/tray_probe/     托盘验证：HMENU 与 core 模型逐项比对、点击/命令两条消息路径、图标与气泡
  tools/ocr_probe/       S4 端到端：GDI 画已知文字 → PP-OCR → 3/3 行断言 + 初始化/识别耗时与内存
  tools/screenshot_probe/ S3 端到端：GDI 抓屏 → 选区 → 九种标注 → 离屏合成 → PNG/剪贴板 + 逐像素验证
  tools/record_probe/   S2 端到端：真复制 + 真编码 + RecordingSession，量帧数/拒收/字节/内存；
                        也支持 synthetic:WxH（合成源，用来量 4K30 峰值）与 static/noaudio
  tools/wgc_probe/      窗口录制端到端：自建窗口 → WGC → 常驻纹理 → MP4
  tools/rect_copy_test/ 脏矩形拷贝的功能验证（进 CTest，不碰屏幕也不需要复制权）
  tests/      core 的单测（无第三方框架，十八个可执行 + CTest）
  app/        组装根：DPI → 能力 → 设置 → 日志 → 托盘 → 热键 → 覆盖层 → 消息循环，
              `--selftest` 量空闲内存并给出退出码
    screenshot_overlay.h/.cpp   截图会话：全屏覆盖层、选区/把手、工具栏、鼠标键盘编辑、文字 EDIT、导出
    pin_window.h/.cpp           贴图：WS_EX_LAYERED + UpdateLayeredWindow，白边 + 光晕，拖动/双击关闭
    ocr_result_window.h/.cpp    OCR 结果：左侧原图 + 红框，右侧按行文本 + 搜索 + 复制全部
    ocr_worker.h/.cpp           OCR worker：raw + 结果文本协议，子进程跑推理
    settings_window.h/.cpp      六页设置 + 环境摘要（截图/录制/OCR/快捷键/外观/关于）
    version.h                   交付版本与通道（2.0.0 / stable）
  build.cmd   vcvars → cmake configure → build → ctest
  package.ps1 交付打包：staging + SHA-256 + zip + 更新清单（≤ 20 MB 断言）
```

三条硬规则（计划 §3.3）各自的执行方式：

1. **core 只产显示列表**，渲染由后端一次提交。`display_list.h` + `render_backend.h` 是这条规则的载体：`Render(plan, clip)` 一次拿到整张列表，core 侧没有任何渲染 API；`Blit` 只带 `ImageHandle`，因为帧循环里不做纹理上传——裸缓冲只在 `RegisterImage` 这一步跨过边界，之后图元引用的都是它返回的句柄。
2. **热路径只操作裸缓冲**：core 只看得到 `ImageRef{pixels, width, height, stride}`，映射/上传/解绑全在 `platform/`。
3. **可机检**：`core/CMakeLists.txt` 在 configure 阶段逐行扫 `core/**` 的 `#include`，三条判据：越出 core/ 的相对引入直接违规；引号形式且能在 `core/` 里找到同一个文件是自家头，放行；其余按平台头前缀判（`windows.` `d2d1` `dxgi` `mf` `audio` `winrt/` `wrl/` 等），命中就 `FATAL_ERROR`。判据早前是「整文件匹配前缀」，那样 `audio` 会把自家的 `#include "audio_source.h"` 一起抓掉，所以改成按行。`tests/CMakeLists.txt` 再加一道链接期兜底——`halocap_core` 一旦链上任何库（含系统导入库）同样 `FATAL_ERROR`，这条挡的是「头文件没引但真调了 Win32 函数」。三道用例都反向验过：塞 `#include <windows.h>`、塞 `#include "../platform/win32/…h"` 都会 configure 失败并指名道姓，塞 `#include "audio_source.h"` 必须放行。仓库目前没有 CI，所以检查挂在 configure 上，任何人 configure 就跑。

文本不新增图元类型：`IFontRasterizer::LayOut` 返回的 `TextCell` 只能以 `ToBlit()` 进入显示列表，所以 bounds、脏区裁剪与后端都不用为文本特判。折线（`Path`）**仍然故意不在列表里**——它要等几何缓存策略定下来，放进列表却画不出来比不放更糟。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2；**八个都已落地**。第八个（`IStore` + `ICapability`）的 core 半边在 `capability.h` / `settings.h` / `store.h`：平台只汇报事实与搬运文本，判断（系统下限、日志规则、磁盘下限、空闲回收线、特性开关）全在 core，因此都能在没有文件系统的单测里钉住。

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

命令行第 4 个参数：`0`=屏外可见、`1`=屏上可见并做 A/B 抓屏、`2`=屏上但不显示。输出 `PACING`、`PRESENT_UNPACED_MS_PER_FRAME` / `FRAME_SLOT_PACED_MS_PER_FRAME`（加 `VISIBLE_` 前缀是真实可见时的同两项）、私有内存与 detach 后的变化。

`mode 1` 是唯一能证「屏幕上真看得见」的办法，判据两条：`SCREEN_TOP_IS_OURS`（用 `WindowFromPoint` 确认取样点上我们最上面）和 `SCREEN_CENTER_BGRA` 的 A/B——`before`/`after` 是桌面色、`during` 必须等于画进去的精确 BGRA，另加文本区近白像素计数。`WS_EX_NOREDIRECTIONBITMAP` 没有重定向位图，`PrintWindow` 拿不到内容，所以抓的是桌面合成结果（`BitBlt(SRCCOPY|CAPTUREBLT)`）。

这里踩出两条产品侧也必须照做的要求：**显示时必须再压一次 `SetWindowPos(HWND_TOPMOST)`**——`ShowWindow` 不改 Z 序，第一次 A/B 就是因为被别的 topmost 窗口压住而判 FAIL；以及 **Present 不受 vsync 节流**，`Present(1)` 与 waitable object 都不阻塞（这条链是独立翻转），所以帧率要由 app 自己的调度器决定，「不丢帧」得按「已提交帧数 / 应有帧数」量，不能拿 Present 的耗时当节奏证据。

节奏就在 `core::FrameScheduler`：`BeginFrame(now)` 到钟才接单、一次只领一帧、落后的槽记 `missed`（不靠连发补帧），`EndFrame(now, presented)` 区分超预算与 Present 失败；时钟是注入的微秒，所以完全可单测（`scheduler_tests`，25 项断言）。`SCHEDULE_*` 与 `VISIBLE_SCHEDULE*` 两组键就是这套统计的输出，判定为 `submitted/expected ≥ 0.98`。休眠前必须 `timeBeginPeriod(1)`，否则粒度不够（spike 2 的老坑）。

两个内存事实：离屏目标纹理与 STAGING 是**懒建**的（`Resize` 只记尺寸、放掉旧资源，真要往自己身上画才分配），所以挂着窗口时私有内存是 60 MB 而不是 69–76 MB；`DetachWindow` 里会调 `IDXGIDevice3::Trim()`，但实测私有内存 61→61、一秒后仍 61——这 60 MB 不是我们的引用扣着的，结案要等真实 app 用环境摘要同一字段复验。

采集链路（会在屏幕上搬一个小黑方块来制造真实变化，默认 5 秒；参数：输出序号 / 时长毫秒 / 几帧搬一次 / busy 重试次数）：

```
native> build\tools\capture_probe\capture_probe.exe 0 5000 3
```

`Open` 失败时它会先按 `core::BackoffUs` 退避重试（250 ms 起、×2、封顶 4 s），把 `busy / timeout / lost` 分开报——复制权被占用是常态分支，不是崩溃。这台机器目前每次都返回 `busy`：`DuplicateOutput` 对所有程序都被拒（连今天早些时候跑出过吞吐数字的 spike 2 工具也一样），所以端到端的脏矩形量级数字仍缺，工具本身已经把该打的统计都打好了。

两个必须照做的实现前提（都是实测）：**`SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` 要在枚举输出之前调**，否则 `DXGI_OUTPUT_DESC.DesktopCoordinates` 给的是 DIP 而不是像素，本机 2560×1440 会读成 `0,0..1707,960`，常驻纹理尺寸与所有坐标一起错；**设备必须属于拥有该输出的适配器**，用错设备是 `E_INVALIDARG`，用对设备才走到 `E_ACCESSDENIED` 那一层——`Initialize` 因此只把同适配器的输出放进可复制列表，`OUTPUTS_SYSTEM` 与 `OUTPUTS_CAPTURABLE` 的差就是被这条约束挡掉的。

编码链路（全程离屏：自己造 BGRA 纹理，不上屏、不抢前台。参数：宽 / 高 / 帧率 / 帧数 / 只跑哪一档）：

```
native> build\tools\encoder_probe\encoder_probe.exe                                   # 720p120，五档全跑
native> build\tools\encoder_probe\encoder_probe.exe 3840 2160 30 60 HW-PACED          # 4K，只跑产品那一档
```

第 5 个参数存在的原因本身就是结论之一：一个进程里连着跑多档，内存归因不干净（上一档的编码器资源由驱动延后释放，同一份代码两次跑出 160 MB 与 598 MB 都出现过）。要能写进文档的数字，都得单档单进程。探针做的四件事：把 NV12 回读出来对着色块判**色彩范围与矩阵**；按候选试 **NV12 输出的 bind flag** 并把每一组的两个 HRESULT 打出来；跑**奇数尺寸 / 空句柄**两个负例；再按硬件、软件、排帧、留节流几种组合量 `OpenVideo`、送帧成本、`Finalize`、字节数与内存。

五条只能实测出来的前提（数字与推导都在计划文档 §5.1「S1 的编码 seam」）：

1. NV12 当视频处理器的输出，纹理**必须带 `D3D11_BIND_RENDER_TARGET`**；只给 `SHADER_RESOURCE` 或 `DECODER` 时 `CreateVideoProcessorOutputView` 一律 `E_INVALIDARG`（Intel UHD 与 RTX 4060 一致）。实现取 `RT|SRV`，这样同一张纹理进编码器之外还能被采样。
2. **默认色彩空间就已经是 16–235 + BT.709**（黑 Y=16、白 Y=235、红 Y=63 Cr=240），而显式写 `D3D11_VIDEO_PROCESSOR_COLOR_SPACE` 的 `Nominal_Range=1` 拿到的反而是 0–255 + BT.601——位段不按文档生效，产品路径一律不碰。
3. **宽高必须都是偶数**（NV12 的 4:2:0 平面划分），所以选区取整是录制会话的责任，`OpenVideo` 只负责把这条拒在入口。
4. **编码器身份要等协商完才问得到**：`GetServiceForStream(GUID_NULL, IID_IMFTransform)` 在 `SetInputMediaType` 之前返回 `MF_E_UNSUPPORTED_SERVICE`，之后返回 `S_OK` 并能读到 `MFT_ENUM_HARDWARE_VENDOR_ID_Attribute`（本机 `VEN_8086`）。所以 `EncoderStats` 分成「已请求」与「已观测」两位。
5. **探针自己的内容生成必须走 GPU**（`CopySubresourceRegion`）。每帧 CPU 上传大纹理的两种写法都会把编码器的成本盖掉：`UpdateSubresource` 在 720p 240 次涨 ~450 MB，`DYNAMIC + Map(WRITE_DISCARD)` 在 4K 60 帧涨 ~2.4 GB——后者一度被误记成「不节流导致写入器攒帧 2.69 GB」，改法与纠错过程都记在计划文档里。

编码这条管线的时钟与闸门在 core（`RecordingClock` 暂停冻结媒体时间、`FrameGate` 保证样本时间不递减且不超帧率、码率默认 `0.11 bpp × 帧率` 夹 `[1, 45] Mbps`），由 `encoder_policy_tests` 覆盖；音频（AAC 第二条流）还没进来，所以时间轴目前只有视频一条。

音频链路（不出声、不上屏；会打开默认麦克风几秒，只算峰值与 RMS，不落盘不上传。参数：观察时长毫秒）：

```
native> build\tools\audio_probe\audio_probe.exe          # 默认 3000 ms
native> build\tools\audio_probe\audio_probe.exe 8000     # 拉长观察窗口
```

三件必须记在实现旁边的实测事实（数字与被推翻的猜测都在计划文档 §5.1「S1 的音频 seam」）：

1. **桌面静音时回环根本不出包**：3 秒 0 个包、49 次 `timeout`、`GetNextPacketSize` 恒为 0、事件从不触发。所以录制侧的音频长度不能挂在「包到达」上，必须按墙钟补静音，否则静音段会让两条流整体错开。
2. **`Initialize` 的两个时序参数在共享模式下不是二选一，是四选全都行**：缓冲填 0 拿 1056 帧（≈22 ms，最低延迟），填 100 ms 拿 4800 帧，periodicity 没有可见效果——`Open()` 因此钉在 `(0, 0)`。顺带三条签名坑：`IMMDevice::Activate` 不能再套 `IID_PPV_ARGS`（会多出 5 个参数）、采集侧 `ReleaseBuffer` 只有一个参数、`AUDCLNT_E_INVALID_FORMAT` 这个错误名不存在。
3. **`GetBuffer` 的 QPC 位置是采集时刻，不是读取时刻**：与取包时的 QPC 差 17–21 ms（等于缓冲深度）。AAC 那条流接进写入器时要把它当固定延迟补偿，不然口型会偏。

还有两条数学不变量是在真流上验的，不只是单测：`设备位置(末) − 设备位置(首) + 第一包帧数 == 收到的总帧数`，以及环形缓冲的 `pushed == 包帧数之和`（探针故意只消费一半，把 `dropped` 逼出来）。借还契约也在麦克风那一路实测过：拿了一包不还就想再等，返回 false 且 `kind=invalid`。

输入链路（message-only 窗口，不在桌面上留任何东西；会 `SendInput` 注入几次 F13–F17——这几个键基本没人绑定，不改变前台，但确实进一次输入流）：

```
native> build\tools\input_probe\input_probe.exe
```

三条必须写在实现旁边的实测事实（全在计划文档 §5.1「S1 的输入 seam」）：

1. **拦下来之后，自己的 `WM_HOTKEY` 也不再投递**。低级钩子的链按安装顺序倒着走，探针因此先装一个「只数不看」的观察者钩子、再让宿主装拦截钩子：允许吞的时候观察者一个按下都没收到（证明真拦住了），同时我们自己的热键计数不涨（证明拦下与注册热键不能在同一把键上共存）。1.x 的圆盘单键模式在 2.0 里必须由钩子触发动作。
2. **抬起一律放过**是策略而不是漏拦——吞掉 `up` 会让前台卡在修饰键状态里。第一版判据把 up 也算成「没拦住」，是我的测量写错，不是实现错。同理，两次注入之间不排空热键队列，`hotkey_still_fired` 就会拿到上一次的投递当结果。
3. **message-only 不能用 `GetParent(hwnd) == HWND_MESSAGE` 判**：本机 `GetParent` 返回 0，`GetAncestor(GA_PARENT)` 返回系统每线程一个的隐藏父窗口。真正的性质是「本进程在 `EnumWindows` 里没有任何顶层窗口」，探针直接数，数到 0 才算过。

IME 那一侧的验证是：给窗口挂一个自建上下文，`SuspendIme` 的返回值必须正好是那个句柄；恢复之后再探一次（用 `ImmAssociateContext` 探，不用 `ImmGetContext`——没有关联时它会给出系统默认上下文，比不出东西），拿回来的还得是同一个。钩子回调每次事件驻留 114–197 µs，这个数要能打印出来：超过系统的 `LowLevelHooksTimeout` 之后钩子会被直接跳过，表现是「偶尔拦不住」而日志里什么都没有。

设置、日志与能力链路（只往 2.0 自己的目录写，1.x 的设置只读；参数：可选的临时根）：

```
native> build\tools\store_probe\store_probe.exe
```

它先打印三个真实路径（`SETTINGS_PATH` / `LOG_DIR` / `MODEL_DIR`），断言它们都在 `HaloCap2` 下而不是 1.x 的 `HaloCap`；接着在临时根里走一遍「保存 → 读回 → 二次保存原子替换 → 坏文件退回默认值 → 0 字节文件报 `empty`」；再往真实日志目录写一行并跑一次过期清理；最后**只读**导入本机 `%APPDATA%\HaloCap\settings.json`，打印五把热键与四类报告，并比对导入前后文件的大小与时间戳。判据是 `STORE_PROBE=OK`。

本机实测（2026-11-08）：`IMPORT_1X_MIGRATED=5`、`IMPORT_1X_IGNORED=4`、`IMPORT_1X_REPAIRED=0`、`IMPORT_1X_DROPPED=0`；`OS_BUILD=26300`、`WGC_SUPPORTED=1`；4K30 Balanced 的磁盘下限 195.8 MB，可用约 252 GB；探针进程私有内存 2.1 MB（远在 220 MB 的空闲回收线下）。

三条必须记在实现旁边的结论（数字与推导都在计划文档 §5.1「S1 的设置 seam」）：

1. **1.x 的 `recording` 里有四个只读派生字段**（`framesPerSecond`、`countdownSeconds`、`audioBitrateValue`、`wantsAudio`），不是两个。少登记两个的后果是每次导入都多两条「多余的键」假警报——这条只有真读本机文件才会暴露。
2. **`ResolveOutputSize` 与 1.x 有一处故意不同**：比值恰好落在奇数整数上时（6×864 → P720 得 5），1.x 会把奇数宽度交给编码器，而 2.0 的 `OpenVideo` 拒收奇数（NV12 的 4:2:0 划分不允许）。2.0 对宽度再取一次偶（5→4），1 像素换「录制能开始」。
3. **写不进 `%APPDATA%` / `%TEMP%` 时探针不判 FAIL**：工具启动的原生进程是 **Low 完整性令牌**，写不上 Medium 的对象。后来量清了根因：**沙箱只对工作区里的二进制降完整性**，把 `store_probe.exe` 复制到工作区外再跑，`LOG_WRITE` 与 `%TEMP%` 都正常（`TEMP_ROOT_SOURCE=temp`、`LOG_FILES=1`）。同一件事在 `tray_probe` 上更直接：复制出去后 `PROBE_INTEGRITY=12288`、`TRAY_ADDED=1`、`TRAY_NOTIFY=1`。探针里的回落（exe 旁边的临时根、`LOG_WRITE=blocked` 跳过断言）仍然保留，但**「真实 `%APPDATA%\HaloCap2` 可写」已经复验过**。

托盘链路（会在托盘里短暂出现一个「光圈」图标，跑完即摘；没有 Shell 时跳过图标相关的两条）：

```
native> build\tools\tray_probe\tray_probe.exe
```

`TRAY_PROBE=OK` 的判据分两半：**不需要 Shell 的那半**把菜单模型建成真的 `HMENU` 再逐项读回来，与 `core::BuildTrayMenu` 比对命令 id、文字、快捷键列、可用、勾选、分隔（空闲 11 行、录制 12 行），并验托盘点击与 `WM_COMMAND` 两条消息路径；**需要 Shell 的那半**是图标真被加上、通知被接受。

三条必须记在实现旁边的结论（细节与推导在计划文档 §5.1「S1 的托盘」）：

1. **托盘宿主不能用 message-only 窗口**。`TaskbarCreated` 是广播消息，message-only 窗口收不到，资源管理器重启之后图标就再也回不来。宿主是 `WS_POPUP | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE` 的隐藏顶层窗口，收到广播后重新 `NIM_ADD`。
2. **标准 `HMENU` 是与 1.x 唯一故意的差别**。1.x 的托盘菜单是自绘 WPF `ContextMenu`（圆角、图标、彩色动作），视觉等价要等 UI 那一轮；菜单项、顺序、可用性、勾选与快捷键列是 1:1 的，且都有单测。系统菜单顺带给了 Esc 收起、点别处收起、键盘导航与高 DPI。
3. **这台机器上图标加不上，原因不在代码**：工具启动的原生进程是 **Low 完整性令牌**（`PROBE_INTEGRITY=4096`，PowerShell 是 High `12288`；`PROBE_APPCONTAINER=0`）。UIPI 因此挡住发给 Shell 托盘窗口的消息，`Shell_NotifyIconW(NIM_ADD)` 返回 `ERROR_ACCESS_DENIED`——换系统自带图标也一样（`STOCK_ICON_ADDED=0`）。同一条限制也是 `store_probe` 写不进 `%APPDATA%`/`%TEMP%` 的根因。**「图标真的出现在托盘、气泡真的弹出」要在普通权限的机器上跑一次 `tray_probe` 才能结案**；菜单、提示文字与两条消息路径已经验过（120 项断言）。

托盘在 core 里的规则全部来自 1.x 的 `TrayMenu.BuildMenu`：录制中只有「停止录制」与「暂停录制」可用，截图/OCR/贴图/打开光圈/设置/自启全灰；贴图还要求剪贴板里真有内容；「开机自启」是勾选项。提示文字是「正在录制 / 录制已暂停 / HaloCap · 热键 / HaloCap 光圈」四档，按 63 个 UTF-16 码元截断且不切开代理对（1.x 按 `string.Length` 截，表情会留半个字）。

## app：S1 的组装与自检

```
native> build\app\HaloCap.exe                              正常起（待着；托盘菜单退出）
native> build\app\HaloCap.exe --selftest                   起来 → 量空闲 → 建覆盖层 → 再量 → 退出
native> build\app\HaloCap.exe --selftest --overlay-size 3840x2160
native> build\app\HaloCap.exe --selftest --report out.txt  写进文件（explorer / 计划任务启动时没有控制台）
native> build\app\HaloCap.exe --selftest --no-tray --no-input --no-overlay   归因用
```

组装顺序就是 S1 的清单：`SetProcessDpiAwarenessContext` → 能力探测（`OS_BUILD` / `WGC`）→ 设置（`LoadSettingsFrom`，首次运行写默认值）→ 日志（`AppendLogLine` + 启动时 `PruneLogs`）→ 托盘（`Win32Tray`，菜单状态来自 `core::TrayMenuState`）→ 热键（五把，token 1–5）→ 消息循环（30 ms 一醒，轮询热键与托盘点击）。托盘命令与热键现在只记日志，动作在 S3/S4 接；单实例是命名 mutex，`--selftest` 跳过它以便连跑。**覆盖层不再在启动时预热**（原先那一步是照 1.x 抄的，但 2.0 的截图/OCR 走 `ScreenshotOverlay` 自己的设备，预热那台只服务录制、白占 58 MB）：`OverlayStack` 在 `beginRecording` 里按需建，`--selftest` 第二段仍会显式建一次来量成本（61.0 MB）。

本机实测（`APP_SELFTEST=OK`，退出码 0，1920×1080 桌面）：

| 场景 | 私有内存 | 工作集 |
| --- | --- | --- |
| idle：托盘 + 热键 + 设置 + 日志，覆盖层未建 | 2.3 MB | 16.0 MB |
| 覆盖层挂上（窗口 + 合成链 + 一页文本图集），未 Present | **62.5 MB**（1440p 62.4、4K 62.1） | 45.9 MB |

三条读数（推导与对照见计划文档 §5.1「S1 的组装与空闲内存」）：

1. **两条口径都过 80 MB**：idle 2.3 MB、覆盖层预热后 62.5 MB。S1 的退出条件成立。
2. **私有内存不随覆盖层尺寸变化**：1080p→4K 只差 0.4 MB——后缓冲在这块账上不计（本机是 GPU/驱动侧分配）。换成工作集或显存口径要另算，那是 S3 覆盖层的成本，不在 S1 的 idle 判据里；`surface_probe` 在同一次会话里仍给出 60 fps 的 120/120、比值 1.0000。
3. **idle 的 2.3 MB 是下界**：本会话托盘图标、日志写入与设置保存都被 Low 完整性令牌挡住，真实机器上这几项只会再加几 MB。

## S2 的录制链路（record_probe）

```
native> build\tools\record_probe\record_probe.exe                5 秒、输出 0、./record-probe.mp4
native> build\tools\record_probe\record_probe.exe 10 0 D:\tmp\a.mp4
```

它跑的是产品要用的三块真 seam：`Win32DesktopDuplication` → `RecordingSession` → `MfH264Encoder`（BGRA→NV12 → `IMFSinkWriter`）。自己不重写采集或编码逻辑，所以数字能代表管线。为了让静态桌面也有内容帧，它会搬一个 240×160 的黑方块（与 `capture_probe` 同一条办法）。

本机实测（5.3 秒，1080p30，硬件编码；取样策略已是 `FramePacer`）：

| 项 | 值 |
| --- | --- |
| 采集唤醒 / 有内容 | 506 / 501 |
| 送编码器 / 网格跳过 / 编码器拒收 | 151 / 350 / 0 |
| 比值（对 5.3 秒 × 30 fps） | 0.95（28.5 fps；严格最小间隔那版是 0.79） |
| 输出 | 166 KB，媒体时长 5.32 s |
| 私有内存 | 112 MB |

两件已经修掉、但值得记下来的事（推导见计划文档 §5.1「S2 的录制会话」）：

1. **取样不能用「距上次 ≥ 帧间隔」的严格闸门**：采集约每 10 ms 一帧时，它会把接受间隔抬到 40 ms（23.7 fps）。`FramePacer` 把媒体时间四舍五入到最近的帧格子，同一份节奏给出 28.5 fps；剩下的 5% 是采集节奏 33 ms 与网格 33.3 ms 的拍频，编码器拒收仍是 0。
2. **静态桌面必须有保活帧**：`keepAliveUs` 默认 1 s、第一帧立即送；否则编码器一帧不收，`Finalize` 失败、录出来是坏文件。第 4 个参数写 `static` 可以关掉搬方块，用来在这个方向上做手工验证（本机桌面一直有别的画面在动，完全静态很难造）。

`RECORD_PROBE=OK` 的判据是：会话活着、`Stop` 成功、`framesSubmitted > 0`、文件字节 > 0。复制权被别的程序占用（`busy`）时按 `core::BackoffUs` 重试 4 次，仍失败则打印 `RECORD_PROBE=BLOCKED` 并以 0 退出——那是环境事实，不是失败。

## S2 的其余三块：WGC、4K30 内存与 app 录制

窗口录制（自建窗口、每 33 ms 换底色，走 Windows.Graphics.Capture → 常驻纹理 → 编码）：

```
native> build\tools\wgc_probe\wgc_probe.exe                3 秒、./wgc-probe.mp4
native> build\tools\wgc_probe\wgc_probe.exe 6 D:\tmp\w.mp4
```

本机实测：`WGC_OPEN=OK`、42 帧、`FILE_VIDEO_TRACK=1`、`WGC_PROBE=OK`。**WGC 没有脏矩形**，一帧就是整窗；而且窗口内容常常是奇数尺寸（测试窗口是 946×533），NV12 不接受奇数，所以捕获侧按偶数裁掉最右/最下一像素，`DesktopBounds()` 报的就是裁完的尺寸。

4K30 峰值内存（本机只有 1080p 输出，所以用合成源；它是一张 4K BGRA 纹理，与桌面复制的常驻纹理同数量级）：

```
native> build\tools\record_probe\record_probe.exe 6 0 build\4k.mp4 synthetic:3840x2160 noaudio
```

第一版合成源用了 3 张纹理（361.5 MB，超了 350 MB 目标），改成 1 张后**峰值 230.8 MB**，`RECORD_4K_MEMORY_OK=1`。合成源每帧只换时间戳、不碰 GPU——每帧 `ClearRenderTargetView` + `Flush` 会和写入器在同一台设备上互相等，实测泵会从微秒级掉到几十毫秒级。

app 录制：

```
native> build\app\HaloCap.exe                        托盘「录屏」或 Ctrl+Alt+R 开/关录制
native> build\app\HaloCap.exe --record-seconds 3     无人值守：录 3 秒后退出
```

输出落在设置的录制目录（空则 `视频\Captures`），文件名 `HaloCap_yyyy-MM-dd_HH-mm-ss.mp4`；录制中托盘菜单变「停止录制 / 暂停录制」、图标切红点，结束弹一条「已保存：路径」。实测 2.5 秒 61 帧、文件 63 KB、退出码 0。录制对象在 Start 时建、Stop 时拆，所以 idle 内存不受影响。

**一条必须记住的构建坑**：给 `ICaptureSource` 加 `OpenWindow` 改的是**虚表**。增量构建之后 app 会在 `DesktopBounds()` 处 AV（Windows 事件日志是「出错模块 unknown、偏移 0」，典型的空指针虚调用），而探针有时还能跑——那是旧 obj 与新头文件混着。删掉 `native/build` 全量重建即恢复；以后改任何 seam 的虚函数集合都照这个来。

## S3 的截图（覆盖层 + 探针）

```
native> build\app\HaloCap.exe                  托盘「截图」或 Ctrl+Alt+A 打开覆盖层
native> build\app\HaloCap.exe --screenshot-selftest out.png   无人值守：开覆盖层 → 标注 → 保存 → 贴图 → 退出

native> build\tools\screenshot_probe\screenshot_probe.exe                      900x600@100,100
native> build\tools\screenshot_probe\screenshot_probe.exe out.png 1280x720@0,0
```

覆盖层里可用：拖选、Shift 正方形、八个把手、空格全屏标注、数字键 1–9 选工具、工具栏（九种工具 + 八色板 + 撤销/重做/复制/保存/贴图/关闭）、`Ctrl+Z/Y`、`Ctrl+C`、`Ctrl+S`、`Enter` 复制、`Esc` 退出。文字工具会弹一个真正的 `EDIT` 小窗，输入法与候选窗交给系统。贴图是分层窗口：白边 + 向外渐隐的光晕，拖动移动、双击关闭。

`screenshot_probe` 不画 UI，只跑「领域 + 导出」：GDI 抓屏 → 选区 → 九种标注 → 离屏合成 → 整帧回读 → 逐像素断言 → PNG 与剪贴板。马赛克那一步在探针里先画一层棋盘格，再从回读像素取块平均，验证「像素化它下面的内容」。

本机实测（900×600 @ 100,100；app 自检为 600×400 @ 100,100）：

| 项 | 探针 | app 覆盖层 |
| --- | --- | --- |
| 选区打开 | **25.3 ms** | **78–107 ms**（S3 目标 ≤ 150 ms） |
| 合成 + 回读 | 2.6–3.8 ms | — |
| 私有内存峰值 | **101.3 MB** | **107.6 MB**（S3 目标 ≤ 120 MB） |
| 关闭后 | 97.1 MB | — |
| 输出 | 225,926 字节 PNG；剪贴板 DIB 尺寸一致 | 保存 PNG + 贴图各 1 |

四件记在实现旁边的事：

1. **显示列表加了椭圆图元**：1.x 的椭圆/序号圆是原生椭圆，折线模拟在 4K 上能看出多边形；D2D 的 `FillEllipse`/`DrawEllipse` 本来就是抗锯齿的。
2. **撤销用整份快照**：一次截图的标注是几十条量级，快照比逐操作反演可靠——橡皮删除、序号回退、将来的移动/缩放都不用各写一套。
3. **导出用 `WriteSource` 而不是 `WritePixels`**：PNG 编码器会换像素格式，`WriteSource` 会替我们转换；另外 WIC 走 COM，调用方要先 `CoInitializeEx`。
4. **截图改走 GDI**：本机的 `DuplicateOutput` 回读全黑，同一桌面 `BitBlt` 有内容；截图不是逐帧增量，一次 `BitBlt` 就够，也顺带避开「录制占用复制权时截图打不开」。录制仍走桌面复制，它的画面内容要单独复查（见 §9 第 19 条）。

已知差异（不阻塞 S4）：马赛克只平均底图、选区不能整体拖动、工具栏是文字标签、文字提交后才上屏。

## S4 的 OCR（ocr_probe）

```
native> set HALOCAP_OCR_MODELS=D:\Projects\HaloCap\src\HaloCap\Assets\OcrModel
native> build\tools\ocr_probe\ocr_probe.exe --png ocr-input.png
native> build\app\HaloCap.exe --ocr-selftest ocr-selftest.png
```

模型就是 1.x 的 `Assets/OcrModel`（PP-OCRv6 tiny：det 1.8 MB、rec 4.5 MB、cls 1.0 MB、dict 6904 字），ORT 1.20.1 的 win-x64 二进制 vendored 在 `third_party/onnxruntime`。app 侧模型目录按 `HALOCAP_OCR_MODELS` → `exe\models\ppocrv6-tiny-1` → 开发树 `src/HaloCap/Assets/OcrModel` → `%LOCALAPPDATA%\HaloCap2\models\ppocrv6-tiny-1` 的顺序找。

`ocr_probe` 用 GDI 画三行已知文字（英文/中文/数字），跑完整链路并断言每一行都出现在结果里：

| 项 | 值 |
| --- | --- |
| 引擎 init（三本会话） | **87.5 ms** |
| 识别（900×280，三行） | **82 ms** |
| 准确率 | **3/3 行命中** |
| 内存 | init 后 36.5 MB、识别后 70 MB、释放后 13.9 MB |

第二引擎是系统 `Windows.Media.Ocr`（`ocr_probe --engine winrt`：init 10.5 ms、识别 44.6 ms）；`ocr.engine` 设置 Native / Model 切换，默认 Native。**推理现在跑在一次性 worker 子进程里**（`HaloCap.exe --ocr-worker in out engine`），主进程不加载 ORT：app 自检（桌面 900×520 区域）经 worker 识别约 175 ms、结果窗口 32 行、`APP_OCR_WORKER=1`。**内存**：识别后主进程回落到 **42.1 MB**（`APP_OCR_AFTER_OVERLAY_CLOSE_UNDER_80=1`），这已在 80 MB 预算内。原先停在 ~97 MB 的真因不是模型、也不是 D3D 设备回收不掉，而是**启动时预热的那台 `OverlayStack` 常驻 58 MB**——它只服务录制，截图/OCR 走 `ScreenshotOverlay` 自己的设备，预热对截图从来没有用处。删掉启动预热、录制改为在 `beginRecording` 里按需建（本来就有懒建路径）之后：预热时 OCR 后 99.1 MB → 不预热 41.6 MB。分层测量见 `--ocr-selftest` 的 `APP_OCR_AFTER_RELEASE_MB`（结果窗口关掉）与 `APP_OCR_AFTER_OVERLAY_CLOSE_MB`（覆盖层也关掉），两者现在都是 42.1 MB，且 > 80 MB 会直接判 FAIL。新进程空闲 2.9 MB。

`--settings-selftest` 打开六页设置并抓图：左侧「截图 / 录制 / OCR / 快捷键 / 外观 / 关于·诊断」，保存走 `core::SaveSettingsTo`：热键先重新注册（冲突或注册失败就原样退回、窗口不关、编辑保留），成功后才落盘；截图格式、OCR 引擎、录制参数等直接写进 `liveSettings` 立即生效。关于页就是环境摘要：系统 build、WGC、私有内存、模型目录、设置与日志路径。

**热键捕获**（`--hotkey-selftest`）：点某一行进入捕获态，`WH_KEYBOARD_LL` 收按键，Esc 取消，与其余四把冲突或裸键不安全时只弹底部横幅、留在捕获态。两个坑记一下：①钩子里的 `GetAsyncKeyState` 可能读到「Ctrl 没按住」，把 Ctrl+Alt+D 变成裸 D，所以修饰键状态改由钩子自己按 `WM_(SYS)KEYDOWN/UP` 维护（起捕获时用 async 状态做一次种子）；②LL 钩子的回调跑在装钩子那根线程的消息队列上，自检块没有消息循环，注入之后必须自己抽队列，否则钩子一次都不会被调用。`--hotkey-selftest` 注入真按键验五道闸门：与别的动作冲突、裸键不安全、合法组合被接受、Esc 取消、以及「设置里本来就带着两把相同的键」时保存被挡（既点真按钮、也直接调 `Save()`，命中区与闸门分开判）。

**结果窗口搜索**（`--ocr-search-selftest`）：喂一份合成的识别结果，验 `x/y` 计数、上下双向回绕、大小写不敏感、`无匹配`、清空查询，以及最容易被忽略的一条——清空查询后图上的绿框/蓝框必须消失。高亮画在父窗口上，测试用 `PrintWindow` 印一张窗口位图直接数绿/蓝像素（实测命中态 4860/1620、清空后 0/0）。

一个踩到的坑：rec 的导出已经做了 softmax，再算一次会把置信度算成 1/6906（0.000），所有行被过滤；`DecodeCtc` 现在先看行和，像概率就取最大值、否则才算 softmax。

收尾项已补齐（2026-11-09）：设置页的热键捕获（冲突提示、裸键拦截、Esc 取消、保存闸门，`--hotkey-selftest`），结果窗口的命中高亮/按行跳转（`x/y` 计数、双向回绕、F3/回车、图上绿/蓝框，`--ocr-search-selftest`），同进程 OCR 后的内存回落（42.1 MB，见上一节）。

## S5 交付（package.ps1）

```
native> powershell -NoProfile -ExecutionPolicy Bypass -File package.ps1
native> build\app\HaloCap.exe --version
native> build\app\HaloCap.exe --check-update dist\HaloCap-2.0.0-win64.json
```

`package.ps1` 把 `HaloCap.exe`、`onnxruntime.dll` 与 `models\ppocrv6-tiny-1\*` 放进 staging，逐个算 SHA-256，压成 zip，并把清单写在 zip 旁边（清单里要放 zip 自己的哈希，放进 zip 会自引用）。

| 项 | 值 |
| --- | --- |
| 包体 | **11.12 MB**（11,671,000 字节（约），zip）≤ 20 MB |
| 文件数 | 8 |
| 清单 | `dist\HaloCap-2.0.0-win64.json`（version / url / sha256 / size / files） |
| 签名 | 2.0 分支不签（`PACKAGE_SIGNED=0`）；main 的发行线补 `signtool` |
| 打包后自检 | 复制到工作区外、清掉 `HALOCAP_OCR_MODELS`：`--ocr-selftest` 与 `--ocr-worker ... pp` 都通过（模型在 exe 旁边） |

更新通道是「清单 + 版本比较」：`--check-update` 打印 `UPDATE_CURRENT / UPDATE_LATEST / UPDATE_AVAILABLE / UPDATE_URL / UPDATE_SIZE`，语义化版本比较支持 `2.0.0-beta < 2.0.0`。下载/校验/替换留给 main 的发行线，2.0 只提供可测的协议（`update_tests` 19 条断言）。

**与 1.x 的对照**：空闲 249 → **2.5 MB**；4K30 峰值 841 → **230.8 MB**；交付包 90 → **11.12 MB**；截图选区 0（1.x 未单测）→ **78.2 ms**；4K 体积/分钟 184 MB（真实桌面）vs 合成源 1.7 MB/分钟，**内容不同不可比**，需 4K 真机复测。

**测试迁移**：1.x 的 82 项里，core 部分全部落到 17 个 CTest 可执行文件（坐标/选区/热键/标注/马赛克/OCR/录制/音频/诊断/日志），日志见 `docs/2.0-native-plan.md` §5.1「S5 交付」的矩阵；未迁的是剪贴板历史解析与 WPF 工具栏图标（2.0 只做截屏贴图，工具栏是文字标签）。

## 剪贴板贴图与工具栏图标

```
native> HaloCap.exe                     托盘「贴图」或 Ctrl+Alt+P
native> build\app\HaloCap.exe --pin-selftest pin.png
```

贴图读取优先级与 1.x 一致：文件 > 位图 > HTML > 文本。颜色文本（`#1234`、`rgb(17,34,51)` 等）渲染成颜色卡，普通文本渲染成白底文本卡，位图与本地图片文件直接贴图，其余路径 / 坏图回退成路径卡；一次最多解码 16 张图片文件，同一剪贴板序列贴过一次图片后回退为路径（1.x 的粘贴序列规则）。核心规则在 `core/clipboard.*`（`clipboard_tests` 617 条断言），平台读取/解码/卡片绘制在 `platform/win32/win32_clipboard.*`。

`--pin-selftest` 往剪贴板写 `#1234` 再贴图，断言 `APP_PIN_WINDOW=1 / APP_PIN_PROOF=1` 并抓证明图；图片 / 文件 / HTML 路径由 core 测试与平台读取代码覆盖，尚未逐一做视觉证明。

工具栏图标从单字标签换成了 display list 画的向量图标（笔/矩/椭/箭/T/荧光/马赛克/序号/橡皮 + 撤/重/复/存/贴/关），当前工具仍是蓝底高亮，截图自检的证明图可验。

## UI 对齐 1.x

配色从 1.x 的 XAML 搬过来：覆盖层工具栏 `#FAF8F9FB` 浅色圆角、`#3C4250` 图标、选中 `#2F7CF6` 白图标、`#1A000000` 分组线；提示条是顶部居中的白色圆角卡 + 蓝色圆点；选区边框与 7×7 把手用 `#2F7CF6`。设置窗口是 `#F7F7F9` 底 + `#EDEDF1` 侧栏 + 白色 12 px 卡 + `#007AFF` 主按钮；OCR 结果窗口是浅底 + 白色预览卡；贴图是白色圆角 12 卡 + 1 px 边框 + 柔和阴影。

还没对齐：设置/OCR 窗口的系统标题栏（1.x 是无边框自绘）、设置页标题/副标题、按钮 hover 过渡、贴图悬停工具条。

## 与 1.x 的功能对齐

覆盖层主链已对齐：选区移动、九种工具、**Select 工具移动/删除标注**、**工具选项栏**（空心/实心、粗细、字号、B/I、马赛克块、箭头三种；F / Ctrl+滚轮）、**放大镜 + 取色 + 坐标**、**尺寸徽标**、蓝色选区/把手、白色提示条、hover 高亮、快捷键（V/F/Del/1–9/空格/Ctrl+Z/Y/C/S）、文字 IME、贴图卡、六页设置、OCR 结果搜索 + 上一处/下一处。

完整的 1.x 对照矩阵在 `docs/2.0-native-plan.md` §5.1「1.x 功能对齐矩阵」。仍未对齐：圆盘菜单、录制倒计时/HUD/区域与窗口选择、贴图悬停工具条、OCR 结果双视图、自动启动、更新 UI。

## 光圈（1.x 规格重做）

按 1.x 的 `RadialMenuWindow` 重写：380×380 设计画布、340 白色径向盘、104 中心圆盘、
四个动作在半径 110、扇区 56–168（间隙 3.6°）；展开 150 ms 淡入 + 0.93→1 缩放；
低级鼠标/键盘钩子驱动，死区 52 拖中心、1–4 选动作、Esc/盘外关闭；菜单尺寸
小/中/大 = 360/420/480 DIP，跟随光标居中并夹在工作区内；悬停时扇区 accent 26% 填充、
外弧 2.2 px、图标外移 4 DIP、中心标题切换为动作名并显示 accent 圆点。

## 设置（1.x 规格重做）

820×620 DIP，18 边距 + 18 圆角外投影；标题栏 50（logo + HaloCap + 光圈 + 关闭）、
侧栏 218（六项导航、选中 #E0E0E6 圆角 8）、内容 #F7F7F9；白色卡片 12 圆角 #E7E7EC 边框；
行 = 标题 12.5 + 说明 10.5 + 右侧开关 44×24 / 分段控件 / 快捷键字段；底部「更改在保存后生效」
+ 取消/保存。六页与 1.x 对齐：通用（自启、光圈尺寸）、快捷键、截图与贴图、文字识别、
录屏（画面/控制/保存目录）、关于（版本/更新/诊断）。

## 圆盘菜单

`Ctrl+Alt+D` 或托盘左键在光标处弹出圆盘：四动作从 12 点顺时针（截图 / OCR / 录制 / 贴图），悬停高亮、Esc 或点空白关闭，动作直接调用对应功能。`--radial-selftest` 打开并抓证明图（`APP_RADIAL_WINDOW=1`）。

## 区域 / 窗口录制

设置里的录制范围决定入口：区域 → 覆盖层拖选一块（选区回调后倒计时再开录），窗口 → 取当前前台窗口走 WGC，其余整屏。区域裁剪在 NV12 转换的源矩形上完成，不额外拷贝。

实测：`--record-seconds 3 --record-region 640x360@100,100` 产出 640×360 / 82 帧；`--record-seconds 3 --record-window` 让 WGC 录当前前台窗口，产出 1920×1032 / 81 帧（`record_probe --inspect` 读回尺寸）。
