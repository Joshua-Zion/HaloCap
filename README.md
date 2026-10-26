# HaloCap · 光圈

在鼠标位置呼出径向菜单的 Windows 效率工具。按下全局快捷键，光标周围展开四个操作：**截图、OCR、录屏、贴图**。

当前版本：**1.0.0**。四个操作均为完整实现，且全部离线运行：不发起网络请求，不引入第三方 NuGet 包，不上传任何内容。

## 功能

- **径向菜单**：光标处呼出，不抢当前输入焦点；支持按住热键拖拽松键、单击、数字键 `1–4`、`Esc` / 圆盘外点击 / 再次按热键关闭；贴边自动偏移。
- **截图**：逐显示器冻结画面与暗色遮罩、拖拽选区、实时像素尺寸、`Enter` 复制、`Esc` 取消；工具栏含贴图 / 复制 / 保存 / OCR / 取消。选区像素从原生屏幕缓冲裁剪，按显示器 DPI 写入图片 DPI，150% 副屏也保持 1:1 清晰。
- **贴图**：置顶无边框贴图，可任意拖拽；滚轮以光标为中心缩放；`Ctrl + 滚轮` 调整透明度；`Esc` / 双击关闭；右键菜单支持复制、另存、恢复初始大小、关闭此贴图 / 全部贴图；支持多贴图并存。
- **OCR**：`Windows.Media.Ocr` 离线识别，优先简体中文，其次英文；结果浮窗显示逐行文本，可复制全部或点击单行复制；中文语言包缺失时给出明确的系统设置指引。
- **录屏**：`Windows.Graphics.Capture` 选区录制，`Media Foundation` 直接编码 H.264/MP4；录制 HUD（红点 + 计时 + 停止）、托盘图标切换与“停止录制”、完成后通知并可一键打开所在文件夹；保存目录可在设置中修改。
- **系统集成**：全局热键与冲突回滚、托盘菜单、开机自启、单实例激活、JSON 设置、本地日志、PerMonitorV2 混合 DPI。

## 环境要求

- Windows 10 1903+ / Windows 11，x64
- .NET 8 SDK（含 Windows Desktop / WPF）
- Windows 10 2004+ 上录屏会显式包含光标；1903/1909 上使用系统默认光标策略

## 构建、测试与发布

```powershell
dotnet build HaloCap.sln -c Debug
dotnet run --project src/HaloCap/HaloCap.csproj
dotnet run --project tests/HaloCap.CoordinateTests/HaloCap.CoordinateTests.csproj
```

截图/录屏坐标换算测试使用无第三方依赖的轻量控制台 runner；通过退出码表示结果。

发布建议（框架依赖单文件，体积最小）：

```powershell
dotnet publish src/HaloCap/HaloCap.csproj -c Release -r win-x64 --self-contained false `
  -p:PublishSingleFile=true
```

程序为托盘常驻应用，启动后没有主窗口。默认快捷键为 `Ctrl + Alt + D`。

## 目录结构

```text
HaloCap.sln
global.json
src/HaloCap/
  App.xaml(.cs)                    应用入口、模块装配、动作路由、退出清理
  app.manifest                     PerMonitorV2、Windows 10/11 兼容性
  Models/
    HotkeyDefinition.cs            热键模型、显示名与校验
    RadialAction.cs                圆盘动作及图标/配色元数据
    AppSettings.cs                 热键、截图格式、录屏目录
    ScreenModels.cs                PhysicalRect / ScreenMonitor / 纯坐标换算
    CaptureModels.cs               截图模式、命令与结果
  Interop/
    NativeMethods.cs               热键、显示器枚举、DPI、窗口、钩子 P/Invoke
    WinRtCaptureInterop.cs         HMONITOR → GraphicsCaptureItem、D3D11 设备
    MediaFoundationWriter.cs       Media Foundation H.264/MP4 SinkWriter
  Services/
    HotkeyService.cs               RegisterHotKey、冲突检测与回滚
    InputHookService.cs            圆盘展示期间的键盘/鼠标钩子
    TrayIconService.cs             托盘图标、录制状态/停止项、可点击通知
    AutoStartService.cs            HKCU Run 注册表自启
    SingleInstanceService.cs       Mutex + 激活事件
    SettingsStore.cs               %APPDATA%\HaloCap\settings.json
    MonitorService.cs              显示器枚举、DPI、前台窗口恢复
    ScreenCaptureService.cs        原生像素屏幕捕获、裁剪、DPI 适配
    ImageClipboard.cs              带重试的图片/文字剪贴板
    ImageFileService.cs            PNG/JPG 保存与另存对话框
    OcrService.cs                  Windows.Media.Ocr 离线封装
    RecordingService.cs            录制状态机、HUD、停止与通知
    RecordingSession.cs            WGC 抓帧、像素裁剪、编码线程与 MP4 落盘
  UI/
    RadialMenuWindow.xaml          圆盘视觉
    RadialItemView.cs              动作图标/标签/角标
    RadialMenuController.cs        无焦点交互、命中测试、DPI 定位
    ScreenshotOverlayWindow.xaml   逐屏遮罩、选区、尺寸角标、工具栏
    ScreenshotSession.cs           多屏遮罩协调、裁剪、焦点恢复
    PinWindow.xaml                 贴图窗口、缩放/透明度/右键菜单
    OcrResultWindow.xaml           OCR 结果浮窗
    RecordingHudWindow.xaml        录制红点、计时、停止
    SettingsWindow.xaml            热键 / 自启 / 图片格式 / 录屏目录
    UiColors.cs                    颜色工具
  Diagnostics/Log.cs               本地异常日志
tests/HaloCap.CoordinateTests/     坐标、裁剪、DPI 换算单元测试
```

## 交互速查

| 操作 | 行为 |
| --- | --- |
| `Ctrl + Alt + D` | 打开/关闭圆盘，不抢当前输入焦点 |
| 按住热键拖向扇区后松开 | 执行目标动作 |
| 单击扇区 / 按 `1–4` | 截图 / OCR / 录屏 / 贴图 |
| `Esc`、圆盘外点击、再按热键 | 关闭圆盘 |
| 截图工具栏 `Enter` / `Ctrl+C` | 复制选区 |
| 截图工具栏 `Ctrl+S` | 保存图片 |
| 贴图滚轮 / `Ctrl+滚轮` | 缩放 / 透明度 |
| 贴图 `Ctrl+C` / `Ctrl+S` / `Ctrl+0` | 复制 / 保存 / 恢复初始大小 |
| 贴图 `Esc` / 双击 | 关闭 |
| 录制中单击托盘图标 / 托盘“停止录制” / HUD 停止 | 结束并保存 MP4 |

## 实现要点

### 截图与混合 DPI

- 每个显示器使用独立的全屏覆盖窗口，显示冻结画面与暗色遮罩；选区限制在起始显示器内。
- 冻结画面与最终裁剪都使用物理像素：`CopyFromScreen` 读取原生像素，`PhysicalRect` 贯穿选区计算，避免 DIP 与像素混用。
- 覆盖窗口通过 `SetWindowPos` 精确落在显示器物理矩形；鼠标坐标以窗口实际布局尺寸反算回物理像素，因此副屏缩放不同也不会出现偏移。
- 裁剪图按 `96 × monitorScale` DPI 创建：在 150% 屏上贴图初始显示仍是 1 个源像素对应 1 个物理像素。
- 截图、贴图、OCR 共用同一份冻结图；贴图不保存额外缩放缓存，避免多贴图造成内存膨胀。

### 无焦点圆盘

- `ShowActivated=false` + `WS_EX_NOACTIVATE`；圆盘可见期间才安装低层键盘/鼠标钩子。
- 数字键与热键主键冲突时，完整匹配修饰键的按键交还给 `RegisterHotKey`，避免无法“再按关闭”。

### OCR

- `Windows.Media.Ocr` + `Windows.Globalization.Language`，完全离线。
- 优先 `zh-Hans / zh-CN`，其次 `en-US / en`，最后回退到用户配置语言。
- 识别结果保留 OCR 行序；缺失中文语言包时提示在“设置 › 时间和语言 › 语言和区域”安装“光学字符识别”功能。
- 大于 `OcrEngine.MaxImageDimension` 的图片会等比缩小后再识别。

### 录屏

- 通过 `IGraphicsCaptureItemInterop.CreateForMonitor` 为显示器创建 `GraphicsCaptureItem`，使用 `Direct3D11CaptureFramePool.CreateFreeThreaded` 抓帧。
- `SoftwareBitmap.CreateCopyFromSurfaceAsync` 把 GPU 帧拷贝为 BGRA，然后在独立 MTA 编码线程中按选区裁剪并写入 Media Foundation SinkWriter；编码线程与 WPF UI 线程隔离。
- SinkWriter 直接输出 H.264/MP4（无 FFmpeg）；帧时间戳来自 Stopwatch，码率按选区面积动态限制在 2–20 Mbps。
- 静态画面时 WGC 可能不再送帧；最后一帧会延长到停止时刻，保证 MP4 时长与真实录制时长一致。
- 录制 HUD 使用 `SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)` 避免被录入；系统不支持时会优先摆在选区之外。
- Windows 11 的黄色捕获提示边框由系统绘制且不会进入视频帧；本项目不依赖它会消失。
- 录屏不含音频（当前范围仅视频）。输出默认位于“视频”文件夹下的 `Captures`，可在设置中改为任意绝对路径。

### 离线与隐私

- 无 `HttpClient` / `WebRequest` / 遥测 / 云上传。
- 无第三方 NuGet 包；H.264 编码使用系统 Media Foundation，OCR 使用系统离线语言组件。
- 设置与日志均写入 `%APPDATA%\HaloCap\`。

## 验收清单

1. 在记事本中按 `Ctrl + Alt + D`：圆盘出现，记事本焦点和输入不丢失；热键动作在 100 ms 量级完成（以本机实测为准）。
2. 按住热键向“截图”拖动并松开：进入选区遮罩；拖拽选区后工具栏出现；`Enter` 复制、`Esc` 取消。
3. 在 150% 缩放的副屏上截图并贴图：图片清晰、位置不偏移；贴图滚轮缩放、`Ctrl+滚轮` 透明度、`Esc` / 双击关闭均正常。
4. 从圆盘选择 OCR，框选一段中文文字：结果浮窗可复制全部，也可点击任意一行复制；无中文包时显示安装指引。
5. 从圆盘选择录屏，框选区域并开始：HUD 显示红点与计时，托盘图标变为录制状态；停止后生成可播放的 MP4，点击通知可定位文件。
6. 托盘菜单切换开机自启，重启后热键仍然可用；再次启动 HaloCap 不会产生第二实例（会激活已有实例）。
7. 在设置中切换 PNG/JPG 并修改录屏目录，重启应用后配置保持。

## 验证状态

已在当前开发环境完成：

- 所有 XAML 通过 WPF `XamlReader` 实际加载（资源、样式、模板、控件树与附加属性均通过），事件处理器名称与 code-behind 一一对应。
- 所有 C# 文件通过括号/字符串扫描与编码检查；`MediaFoundationWriter.cs` 保持 C# 5 兼容。
- 用本机 .NET Framework 编译器独立编译并运行了 **Media Foundation H.264/MP4 writer**：
  - 2 秒、640×360、60 帧的 MP4 可正常解码，方向正确；
  - 单帧 5 秒时长的 MP4 时长正确，验证静态画面延长最后一帧的策略。
- 单元测试工程覆盖正反拖拽、边界裁剪、负坐标显示器、150% 副屏裁剪、DPI 往返换算等 15 个用例。

仍待完成：

- 当前机器只安装了 .NET 运行时，**没有 .NET 8 SDK**，因此无法执行 `dotnet build`、单元测试和真机交互验证。
- 需要在安装 SDK 后重点验证：Windows.Graphics.Capture 运行时路径、Windows.Media.Ocr 语言包行为、混合 DPI 副屏、无焦点圆盘与焦点恢复、托盘的录制状态切换。

## 已知限制与发布策略

- 截图选区限定在起始显示器内（不支持跨屏一次性框选）；这是为了保持逐屏 DPI 的精确性。
- WPF 自包含发布体积较大，难以压到 20 MB 以内。推荐使用**依赖 .NET 8 Desktop Runtime 的框架依赖单文件**发布；若要分发自带运行时的版本，需要接受显著更大的安装包体积，而不是使用不受支持的 WPF 裁剪。
- 单实例、自启、热键注册均依赖当前用户会话；RDP 会话或权限隔离环境下行为可能不同。
- 录屏编码对 CPU 有一定要求；高分辨率显示器会触发队列丢帧以优先保证文件可播放。
- Windows N/KN 版本需要安装 Media Feature Pack 才能使用 H.264 录制；OCR 需要对应的系统语言 OCR 功能。
- Windows 11 的屏幕捕获提示边框由系统控制；本项目不修改该隐私指示器，它也绝不会出现在录制的视频中。
