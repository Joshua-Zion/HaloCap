# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支。设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10，工具链与构建前提见计划文档的「S1 的工具链」。

当前状态：**S1 骨架可构建、七套测试可跑、§3.3 规则三已做成机检；显示列表能离屏逐字节画对、能经 DirectComposition 真窗口上屏、也能自己按 60 fps 排帧数丢帧；桌面复制的 seam 与脏矩形拷贝已落地；H.264 编码这条管线跑通并且量过（GPU 侧 BGRA→NV12 + `IMFSinkWriter`）；WASAPI 的回环与麦克风两路也跑通并量过；全局热键、低级拦截与 IME 生命周期也跑通并量过**。还没有产品功能——托盘与设置都还没进来，对应的 seam 也不提前定形（避免先写一套再被实现推翻）。

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
    settings.h          IStore 的 core 半边（seam 接口还没定形）：扁平设置树 + JSON 子集、
                        与 1.x 逐项对齐的 schema 与默认值、两字段热键的迁移、逐键修复报告
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
  tools/frame_cost/     覆盖层帧成本测量（与 spike 3 同口径，离屏、不上屏）
  tools/surface_probe/  上屏链路验证：A/B 抓屏证「看得见」+ 60 fps 排帧数丢帧
  tools/capture_probe/  采集链路统计：应有/实取、仅鼠标唤醒、脏像素之比、每帧 CPU 与内存
  tools/encoder_probe/  编码链路验证：颜色回读证转换对、五档组合量成本/字节/内存与编码器身份
  tools/audio_probe/    音频链路验证：端点形状、静音时出不出包、Initialize 时序矩阵、借还契约
  tools/input_probe/    输入链路验证：热键投递、两个钩子链量拦截是否真生效、IME 断开恢复、错误分类
  tools/rect_copy_test/ 脏矩形拷贝的功能验证（进 CTest，不碰屏幕也不需要复制权）
  tests/      core 的单测（无第三方框架，七个可执行 + CTest）
  build.cmd   vcvars → cmake configure → build → ctest
```

三条硬规则（计划 §3.3）各自的执行方式：

1. **core 只产显示列表**，渲染由后端一次提交。`display_list.h` + `render_backend.h` 是这条规则的载体：`Render(plan, clip)` 一次拿到整张列表，core 侧没有任何渲染 API；`Blit` 只带 `ImageHandle`，因为帧循环里不做纹理上传——裸缓冲只在 `RegisterImage` 这一步跨过边界，之后图元引用的都是它返回的句柄。
2. **热路径只操作裸缓冲**：core 只看得到 `ImageRef{pixels, width, height, stride}`，映射/上传/解绑全在 `platform/`。
3. **可机检**：`core/CMakeLists.txt` 在 configure 阶段逐行扫 `core/**` 的 `#include`，三条判据：越出 core/ 的相对引入直接违规；引号形式且能在 `core/` 里找到同一个文件是自家头，放行；其余按平台头前缀判（`windows.` `d2d1` `dxgi` `mf` `audio` `winrt/` `wrl/` 等），命中就 `FATAL_ERROR`。判据早前是「整文件匹配前缀」，那样 `audio` 会把自家的 `#include "audio_source.h"` 一起抓掉，所以改成按行。`tests/CMakeLists.txt` 再加一道链接期兜底——`halocap_core` 一旦链上任何库（含系统导入库）同样 `FATAL_ERROR`，这条挡的是「头文件没引但真调了 Win32 函数」。三道用例都反向验过：塞 `#include <windows.h>`、塞 `#include "../platform/win32/…h"` 都会 configure 失败并指名道姓，塞 `#include "audio_source.h"` 必须放行。仓库目前没有 CI，所以检查挂在 configure 上，任何人 configure 就跑。

文本不新增图元类型：`IFontRasterizer::LayOut` 返回的 `TextCell` 只能以 `ToBlit()` 进入显示列表，所以 bounds、脏区裁剪与后端都不用为文本特判。折线（`Path`）**仍然故意不在列表里**——它要等几何缓存策略定下来，放进列表却画不出来比不放更糟。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2；目前已落地 `IRenderBackend`、`IFontRasterizer`、`ISurfaceWindow`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost` 七个。第八个进行中：`core/settings.h` 已经把设置树、JSON 子集与 1.x 的 schema 落盘并编进 `halocap_core`，但**还没有一套测试**，`IStore` 的接口也故意还没定形——等 `win32_store` 与探针跑通再写，免得先定一套再被实现推翻。

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
