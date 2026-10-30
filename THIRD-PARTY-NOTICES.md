# HaloCap 第三方组件与模型声明

HaloCap 的模型识别引擎使用以下开源组件与模型。它们都随程序离线提供，程序运行时不会下载模型或访问网络。

## RapidOcrNet

- 项目：https://github.com/BobLd/RapidOcrNet
- NuGet：https://www.nuget.org/packages/RapidOcrNet/4.2.0
- 许可证：Apache License 2.0

## PP-OCR 模型

模型识别引擎内置 PP-OCRv6 tiny 检测与识别模型，以及 PP-OCRv5 文本方向分类模型：

- PP-OCRv6 tiny：https://github.com/PaddlePaddle/PaddleOCR
- 模型整理与 ONNX 转换：https://github.com/BobLd/RapidOcrNet
- 许可证：Apache License 2.0

随程序嵌入的文件：

| 文件 | 大小 | SHA-256 |
| --- | --- | --- |
| `ppocrv6_tiny_det.onnx` | 1,829,618 字节 | `f42c0fbd294d95eac1a550e131b277dac97462c8025fa4b6c3cec1b7894bd3d5` |
| `ppocrv6_tiny_rec.onnx` | 4,489,813 字节 | `e16e242de5937ad92609223f19bc2aff3727ee40b095f996907c24749bad251b` |
| `ppocrv5_textline_cls.onnx` | 1,018,508 字节 | `54379ae5174d026780215fc748a7f31910dee36818e63d49e17dc598ecc82df7` |
| `ppocrv6_tiny_dict.txt` | 27,156 字节 | `c5cbe34ef40c29c4df07ed012bf96569cb69a2d2a01a07027e9f13cb832bd9cd` |

## 运行时依赖

- Microsoft.ML.OnnxRuntime 1.29.0 — MIT License
- SkiaSharp 3.119.1 / SkiaSharp.NativeAssets.Win32 — MIT License
- Clipper2 2.0.0 — Boost Software License 1.0
