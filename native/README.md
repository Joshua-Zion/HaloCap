# 2.0 原生实现（开发中）

本目录属于 `2.0` 分支，**目前只有这份说明**：S1 开工、确定 C++ 工具链之后才会出现构建文件与代码，不放空壳占位。

设计依据见 [../docs/2.0-native-plan.md](../docs/2.0-native-plan.md)，分支约定见其 §10。这里只固定一条不会被后续实现推翻的边界：

```
native/
  core/       纯 C++，只依赖标准库与自家头文件
  platform/   平台实现，按后端分目录（win32/ 等）
```

三条来自 §3.3、会被 CI 机检的规则：

1. `core/` 里出现任何平台头文件（`windows.h`、`d2d1.h`、`mfapi.h`、`dxgi.h`…）即构建失败——CI 会用一个 stub 后端单独编译 `core/`。
2. `core/` 只产显示列表（顶点、图元、字形 run、图层顺序），不直接调用渲染 API。
3. 热路径在 `core/` 里只看到「指针 + stride + 尺寸」的裸缓冲，映射与解映射归 `platform/`。

平台边界一共 8 个 seam（`ISurfaceWindow`、`IRenderBackend`、`IFontRasterizer`、`ICaptureSource`、`IEncoder`、`IAudioSource`、`IInputHost`、`IStore`+`ICapability`），职责与 macOS 对应物在 §3.2。

S0 的三个 spike（编码器纹理直入、桌面复制吞吐、Direct2D 帧成本）已经跑完并给出实测结论，代码在同仓库的 [../spikes/](../spikes/)，不参与任何构建；其中被推翻的写法都已回写进 §4 与 §1.3。
