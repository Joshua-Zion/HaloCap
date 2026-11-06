// 编码 seam 的一次跑通：GPU 转换的颜色对不对、写入器收不收、每帧成本与落盘字节数各是多少。
// 全程离屏：自己造 BGRA 纹理，不上屏、不抢前台，也不依赖桌面复制。
#include "d2d_render_backend.h"
#include "mf_h264_encoder.h"
#include "nv12_converter.h"

#include "encoder.h"

#include <psapi.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

namespace core = halocap::core;
using halocap::platform::win32::D2dRenderBackend;
using halocap::platform::win32::MfH264Encoder;
using halocap::platform::win32::Nv12Converter;

struct Swatch {
    const char* name;
    unsigned char r;
    unsigned char g;
    unsigned char b;
};

// 只量「送一帧」的成本：把时钟贴在 Submit 前后，内容与纹理的准备都不算进来。
// 早先用「整循环 CPU 减去单独量的填充 CPU」是错的——那两次填充的成本并不相等，
// 减出来的差值会出现负数（实测 paced 档 LOOP_CPU_MS 比独立填充还小）。
//
// 编码器（硬件的、微软软件 MFT 的）都在自己的工作线程上跑，本线程时间量不到它，
// 所以进程时间只在**整段**上取一次：逐帧累加的微分在 15 帧的短跑里会量化成 0 或 3 ms，
// 那种数字没有意义。这里报三个数：送帧的墙钟成本、整循环的进程 CPU、造同样多帧的进程 CPU，
// 两者的差是编码成本的下限估计。
double ProcessCpuMs(const FILETIME& kernelBefore, const FILETIME& userBefore) {
    FILETIME creation{}, exitTime{}, kernelNow{}, userNow{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernelNow, &userNow);
    const auto toUs = [](const FILETIME& v) {
        return (static_cast<std::int64_t>(v.dwHighDateTime) << 32 | v.dwLowDateTime) / 10;
    };
    return static_cast<double>(toUs(kernelNow) - toUs(kernelBefore) + toUs(userNow) -
                               toUs(userBefore)) /
           1000.0;
}

void CaptureCpuBaseline(FILETIME& kernel, FILETIME& user) {
    FILETIME creation{}, exitTime{};
    GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user);
}

// 五个实心色块是颜色校验的样本：黑与白定「范围」，红与蓝定「矩阵与极性」，
// 中灰定「亮度单调」。实心块意味着期望值算得出来。
const Swatch kSwatches[] = {
    { "black", 0, 0, 0 },
    { "white", 255, 255, 255 },
    { "red", 255, 0, 0 },
    { "blue", 0, 0, 255 },
    { "gray", 128, 128, 128 },
};

constexpr int kSwatchCount = static_cast<int>(sizeof(kSwatches) / sizeof(kSwatches[0]));

std::int64_t NowUs() {
    static LARGE_INTEGER freq = [] {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<std::int64_t>(now.QuadPart * 1000000 / freq.QuadPart);
}

double PrivateMemoryMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.PrivateUsage) / (1024.0 * 1024.0);
}

// 工作集与私有内存都报：1.x 的 ≤350 MB 目标是按任务管理器的内存列出来的，
// 而 GPU 驱动的分配进的是私有内存，两个数会差很多，只报一个就没有可比性。
double WorkingSetMb() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(GetCurrentProcess(),
                             reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters)) == 0) {
        return -1.0;
    }
    return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}

long long FileBytes(const std::string& pathUtf8) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    std::wstring wide(pathUtf8.size(), L'\0');
    for (size_t i = 0; i < pathUtf8.size(); ++i) {
        wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(pathUtf8[i]));
    }
    if (GetFileAttributesExW(wide.c_str(), GetFileExInfoStandard, &data) == 0) {
        return -1;
    }
    return (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

// 每帧内容都得变：全同帧会让码流失去意义，也会让 NV12 环用错了都露不出来。
//
// 变化必须在 GPU 上做。两种 CPU 上传的路子都试过，都会把编码器的内存盖掉：
// 每帧 UpdateSubresource 一张 DEFAULT 纹理，驱动的内部暂存池一路涨（720p 240 次 ~450 MB）；
// 改成 DYNAMIC + Map(WRITE_DISCARD) 更糟，4K 60 帧直接 2 GB——WRITE_DISCARD 每次换一块。
// 产品的采集路径本来也不走 CPU：脏矩形是 GPU 内的 CopySubresourceRegion。
// 所以这里把一张填好的样图按帧号位移拷进待编码纹理，全程 GPU，内存归因才干净。
struct GpuContent {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> pattern;
    int width = 0;
    int height = 0;

    void Advance(ID3D11DeviceContext* context, ID3D11Texture2D* destination, int frameIndex) const {
        // 半幅位移：源矩形取样图的一半，目的位置按帧号平移，越界的一半被自动裁掉。
        const int shiftX = width > 8 ? (frameIndex * 17) % (width / 2) : 0;
        const int shiftY = height > 8 ? (frameIndex * 11) % (height / 4) : 0;

        D3D11_BOX box{};
        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = static_cast<UINT>(width / 2);
        box.bottom = static_cast<UINT>(height / 2);
        box.back = 1;

        context->CopySubresourceRegion(destination, 0, static_cast<UINT>(shiftX),
                                       static_cast<UINT>(shiftY), 0, pattern.Get(), 0, &box);
    }
};

// 色块样图 + 待编码纹理。样图只从 CPU 填一次，之后每帧靠 GPU 位移拷贝产生变化。
core::ImageHandle MakeSwatchTexture(D2dRenderBackend& backend, int width, int height,
                                    GpuContent& content) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width);
    desc.Height = static_cast<UINT>(height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const int band = y * kSwatchCount / height;
        const Swatch& s = kSwatches[band < kSwatchCount ? band : kSwatchCount - 1];
        unsigned char* row = pixels.data() + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = s.b;
            row[x * 4 + 1] = s.g;
            row[x * 4 + 2] = s.r;
            row[x * 4 + 3] = 255;
        }
    }

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = pixels.data();
    initial.SysMemPitch = static_cast<UINT>(width) * 4;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> pattern;
    if (FAILED(backend.Device()->CreateTexture2D(&desc, &initial, &pattern))) {
        std::printf("SWATCH_TEXTURE=FAILED\n");
        return 0;
    }

    // 待编码的那一张：同一组绑定标志，先不填内容（第一帧之前整幅拷一次）。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
    if (FAILED(backend.Device()->CreateTexture2D(&desc, nullptr, &frame))) {
        std::printf("FRAME_TEXTURE=FAILED\n");
        return 0;
    }

    backend.D3dContext()->CopyResource(frame.Get(), pattern.Get());

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(frame.As(&surface))) {
        return 0;
    }

    // 位图属性按字段填：结构体第五项是 colorContext，不是 draw text options。
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    props.colorContext = nullptr;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(backend.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap))) {
        std::printf("SWATCH_BITMAP=FAILED hr=0x%08lX\n",
                    static_cast<unsigned long>(backend.LastHr()));
        return 0;
    }

    content.pattern = pattern;
    content.width = width;
    content.height = height;

    return backend.RegisterGpuImage(bitmap.Get());
}

void ReportColorSpace(const char* tag, MfH264Encoder& encoder, D2dRenderBackend& backend,
                      core::ImageHandle source, int width, int height, int& failures) {
    Nv12Converter& converter = encoder.Converter();

    // 探针在 OpenVideo 之前就要量颜色，所以纹理环得先自己建一次（幂等）。
    if (!converter.EnsureTargets(width, height)) {
        std::printf("%s TARGETS=FAILED kind=%s code=0x%08lX\n", tag, converter.LastErrorKind(),
                    static_cast<unsigned long>(converter.LastHr()));
        ++failures;
        return;
    }

    ID3D11Texture2D* bgra = nullptr;
    if (!backend.TextureFor(source, &bgra)) {
        std::printf("%s: 取不到源纹理 hr=0x%08lX\n", tag,
                    static_cast<unsigned long>(backend.LastHr()));
        ++failures;
        return;
    }

    const int slot = converter.AcquireSlot();
    if (slot < 0 || !converter.ConvertToSlot(bgra, slot)) {
        std::printf("%s CONVERT=FAILED kind=%s code=0x%08lX\n", tag, converter.LastErrorKind(),
                    static_cast<unsigned long>(converter.LastHr()));
        ++failures;
        return;
    }

    std::vector<BYTE> luma;
    std::vector<BYTE> chroma;
    UINT lumaPitch = 0;
    UINT chromaPitch = 0;
    if (!converter.ReadBack(converter.SlotTexture(slot), luma, chroma, height, &lumaPitch,
                            &chromaPitch)) {
        std::printf("%s READBACK=FAILED kind=%s code=0x%08lX\n", tag, converter.LastErrorKind(),
                    static_cast<unsigned long>(converter.LastHr()));
        ++failures;
        return;
    }

    std::printf("%s NV12_LUMA_PITCH=%u NV12_CHROMA_PITCH=%u\n", tag, lumaPitch, chromaPitch);

    int blackY = -1;
    int whiteY = -1;
    int grayY = -1;
    int redCr = 0;
    int redCb = 0;
    int blueCr = 0;
    int blueCb = 0;

    for (int i = 0; i < kSwatchCount; ++i) {
        const int rowY = (height * (2 * i + 1)) / (2 * kSwatchCount);
        const int y = luma[static_cast<size_t>(rowY) * lumaPitch + width / 2];
        const size_t chromaRow = static_cast<size_t>(rowY / 2) * chromaPitch;
        const int cb = chroma[chromaRow + static_cast<size_t>(width / 2)];
        const int cr = chroma[chromaRow + static_cast<size_t>(width / 2) + 1];

        std::printf("%s   %-6s want=(%3d,%3d,%3d) Y=%3d Cb=%3d Cr=%3d\n", tag, kSwatches[i].name,
                    kSwatches[i].r, kSwatches[i].g, kSwatches[i].b, y, cb, cr);

        switch (i) {
        case 0: blackY = y; break;
        case 1: whiteY = y; break;
        case 2: redCr = cr; redCb = cb; break;
        case 3: blueCr = cr; blueCb = cb; break;
        default: grayY = y; break;
        }
    }

    // 三条结构性判据：不依赖选哪个范围或哪个矩阵，搞反了就一定红。
    if (!(whiteY > grayY && grayY > blackY)) {
        std::printf("%s FAIL 亮度不单调（black %d / gray %d / white %d）\n", tag, blackY, grayY,
                    whiteY);
        ++failures;
    }
    if (!(redCr > redCb)) {
        std::printf("%s FAIL 红色的 Cr 应高于 Cb（%d vs %d）\n", tag, redCr, redCb);
        ++failures;
    }
    if (!(blueCb > blueCr)) {
        std::printf("%s FAIL 蓝色的 Cb 应高于 Cr（%d vs %d）\n", tag, blueCb, blueCr);
        ++failures;
    }

    const bool videoRange = blackY >= 12 && blackY <= 22 && whiteY >= 230 && whiteY <= 240;
    const bool fullRange = blackY <= 4 && whiteY >= 250;
    std::printf("%s RANGE=%s (black %d, white %d)\n", tag,
                videoRange ? "VIDEO_16_235" : (fullRange ? "FULL_0_255" : "NEITHER"), blackY,
                whiteY);

    if (!videoRange && !fullRange) {
        std::printf("%s FAIL 黑白的 Y 既不在视频范围也不在全范围\n", tag);
        ++failures;
    }

    if (std::string_view(tag) == "DEFAULT") {
        // 产品就用驱动默认这一组：本机量出来是 16–235 的视频范围 + BT.709 矩阵
        // （黑 16 / 灰 126 / 白 235，红 Y=63 ≈ 16+219×0.2126）。这一条是钉死的行为要求。
        if (!videoRange) {
            std::printf("DEFAULT FAIL 默认的 NV12 输出不是 16-235 视频范围\n");
            ++failures;
        }
    }
    if (std::string_view(tag) == "LEGACY") {
        // 显式写老的结构体位段（Nominal_Range=1 名义上是 16-235）实测得到的是 0-255：
        // 这一组位段在本机驱动上不按文档生效，所以产品路径不碰它，只留这一次量证。
        std::printf("LEGACY note 位段 Nominal_Range=1 在本机给出的其实是 0-255 全范围\n");
    }
}

const char* NameForAttribute(const GUID& key) {
    // 名字取自头文件，不靠猜：这五位足以判定「实际用的是哪一家编码器」。
    static const struct {
        GUID key;
        const char* name;
    } kKnown[] = {
        { { 0x6821c42b, 0x65a4, 0x4e82, { 0x99, 0xbc, 0x9a, 0x88, 0x20, 0x5e, 0xcd, 0x0c } },
          "MFT_TRANSFORM_CLSID_Attribute" },
        { { 0x3aecb0cc, 0x035b, 0x4bcc, { 0x81, 0x85, 0x2b, 0x8d, 0x55, 0x1e, 0xf3, 0xaf } },
          "MFT_ENUM_HARDWARE_VENDOR_ID_Attribute" },
        { { 0x2fb866ac, 0xb078, 0x4942, { 0xab, 0x6c, 0x00, 0x3d, 0x05, 0xcd, 0xa6, 0x74 } },
          "MFT_ENUM_HARDWARE_URL_Attribute" },
        { { 0xf81a699a, 0x649a, 0x497d, { 0x8c, 0x73, 0x29, 0xf8, 0xfe, 0xd6, 0xad, 0x7a } },
          "MF_TRANSFORM_ASYNC" },
        { { 0xe5666d6b, 0x3422, 0x4eb6, { 0xa4, 0x21, 0xda, 0x7d, 0xb1, 0xf8, 0xe2, 0x07 } },
          "MF_TRANSFORM_ASYNC_UNLOCK" },
    };

    for (const auto& entry : kKnown) {
        if (key == entry.key) {
            return entry.name;
        }
    }
    return nullptr;
}

// 协商完输入/输出类型之后，GetServiceForStream 能把真正在跑的 IMFTransform 交出来。
// 属性表整个打一遍，是为了回答 §4 那个悬着的问题：「是否硬件编码」到底有没有可读的证据。
void DumpChosenEncoder(MfH264Encoder& encoder, const char* tag) {
    Microsoft::WRL::ComPtr<IMFTransform> transform;
    const HRESULT hr = encoder.ChosenTransform(&transform);
    if (FAILED(hr) || !transform) {
        std::printf("%s CHOSEN_TRANSFORM_HR=0x%08lX\n", tag, static_cast<unsigned long>(hr));
        return;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (FAILED(transform->GetAttributes(&attributes)) || !attributes) {
        std::printf("%s TRANSFORM_ATTRIBUTES=unavailable\n", tag);
        return;
    }

    UINT32 count = 0;
    attributes->GetCount(&count);
    std::printf("%s ENCODER_ATTRIBUTE_COUNT=%u\n", tag, count);

    for (UINT32 i = 0; i < count && i < 48; ++i) {
        GUID key{};
        PROPVARIANT value{};
        if (FAILED(attributes->GetItemByIndex(i, &key, &value))) {
            continue;
        }

        // 认识的属性打名字，其余打 GUID：避免整屏十六进制，也不藏掉没见过的键。
        const char* known = NameForAttribute(key);
        char label[96] = "";
        if (known != nullptr) {
            snprintf(label, sizeof(label), "%s", known);
        } else {
            wchar_t keyText[64] = L"";
            StringFromGUID2(key, keyText, 64);
            WideCharToMultiByte(CP_UTF8, 0, keyText, -1, label, static_cast<int>(sizeof(label)),
                                nullptr, nullptr);
        }

        switch (value.vt) {
        case VT_LPWSTR:
            std::printf("  %s = \"%ls\"\n", label, value.pwszVal ? value.pwszVal : L"");
            break;
        case VT_UI4:
            std::printf("  %s = %lu\n", label, static_cast<unsigned long>(value.ulVal));
            break;
        case VT_CLSID:
            if (value.puuid != nullptr) {
                char inner[96] = "";
                wchar_t innerWide[64] = L"";
                StringFromGUID2(*value.puuid, innerWide, 64);
                WideCharToMultiByte(CP_UTF8, 0, innerWide, -1, inner, static_cast<int>(sizeof(inner)),
                                    nullptr, nullptr);
                std::printf("  %s = {%s}\n", label, inner);
            }
            break;
        default:
            std::printf("  %s = (vt=%u)\n", label, static_cast<unsigned>(value.vt));
            break;
        }

        PropVariantClear(&value);
    }
}

struct RunResult {
    double openMs = 0.0;
    double submitTotalMs = 0.0;
    double submitMaxMs = 0.0;
    double closeMs = 0.0;
    double loopCpuMs = 0.0;
    double finalizeCpuMs = 0.0;
    long long bytes = -1;
    std::int64_t accepted = 0;
    std::int64_t rejected = 0;
    int inputVariant = -999;
    HRESULT serviceHr = E_FAIL;
    double submitCpuTotalMs = 0.0;
    double peakMemoryMb = 0.0;
    double peakWorkingSetMb = 0.0;
    bool hardwareEncoded = false;
    std::string vendor;
    bool ok = false;
};

RunResult RunEncode(D2dRenderBackend& backend, MfH264Encoder& encoder, core::ImageHandle source,
                    const GpuContent& content,
                    const std::string& path, int width, int height, int fps, int frames,
                    bool requestHardware, bool throttleDisabled, bool paced, const char* label) {
    RunResult run{};

    if (!encoder.Initialize()) {
        std::printf("[%s] INITIALIZE=FAILED kind=%s code=0x%08lX\n", label,
                    encoder.LastErrorKind(), static_cast<unsigned long>(encoder.LastErrorCode()));
        return run;
    }

    encoder.SetThrottleDisabled(throttleDisabled);

    core::VideoEncoderConfig config{};
    config.widthPx = width;
    config.heightPx = height;
    config.targetFps = fps;
    config.requestHardware = requestHardware;

    const std::int64_t openBegin = NowUs();
    if (!encoder.OpenVideo(config, path)) {
        std::printf("[%s] OPENVIDEO=FAILED kind=%s code=0x%08lX\n", label,
                    encoder.LastErrorKind(), static_cast<unsigned long>(encoder.LastErrorCode()));
        return run;
    }
    run.openMs = static_cast<double>(NowUs() - openBegin) / 1000.0;
    run.inputVariant = encoder.InputTypeVariant();
    run.serviceHr = encoder.ProbeServiceQuery();
    DumpChosenEncoder(encoder, label);

    ID3D11Texture2D* sourceTexture = nullptr;
    if (!backend.TextureFor(source, &sourceTexture) || sourceTexture == nullptr) {
        std::printf("[%s] SOURCE_TEXTURE=FAILED\n", label);
        return run;
    }

    ID3D11DeviceContext* context = backend.D3dContext();

    const double memoryBeforeLoopMb = PrivateMemoryMb();
    const std::int64_t loopBeginUs = NowUs();
    const std::int64_t intervalUs = core::FrameIntervalUs(fps);

    FILETIME loopKernel{};
    FILETIME loopUser{};
    CaptureCpuBaseline(loopKernel, loopUser);

    for (int i = 0; i < frames; ++i) {
        content.Advance(context, sourceTexture, i);

        core::EncodedFrame frame{};
        frame.texture = source;
        frame.timeUs = static_cast<std::int64_t>(i) * intervalUs;

        const std::int64_t begin = NowUs();
        const bool submitted = encoder.Submit(frame);
        const double ms = static_cast<double>(NowUs() - begin) / 1000.0;

        run.submitTotalMs += ms;
        if (ms > run.submitMaxMs) {
            run.submitMaxMs = ms;
        }
        if (!submitted) {
            std::printf("[%s] SUBMIT_FAILED frame=%d kind=%s code=0x%08lX\n", label, i,
                        encoder.LastErrorKind(), static_cast<unsigned long>(encoder.LastErrorCode()));
            break;
        }

        if ((i % 10) == 0) {
            const double mb = PrivateMemoryMb();
            if (mb > run.peakMemoryMb) {
                run.peakMemoryMb = mb;
            }
            const double ws = WorkingSetMb();
            if (ws > run.peakWorkingSetMb) {
                run.peakWorkingSetMb = ws;
            }
        }

        // paced 那一档按目标帧率的墙钟送帧：这才是产品真实的工作方式，
        // 也是把「编码器来不及」与「写入器替我们攒帧」这两种情况分开的唯一办法。
        if (paced) {
            const std::int64_t dueUs = loopBeginUs + static_cast<std::int64_t>(i + 1) * intervalUs;
            const std::int64_t nowUs = NowUs();
            if (dueUs > nowUs) {
                Sleep(static_cast<DWORD>((dueUs - nowUs + 999) / 1000));
            }
        }
    }

    run.loopCpuMs = ProcessCpuMs(loopKernel, loopUser);
    const double loopWallMs = static_cast<double>(NowUs() - loopBeginUs) / 1000.0;

    core::EncoderStats stats{};
    const std::int64_t closeBegin = NowUs();
    FILETIME closeKernel{};
    FILETIME closeUser{};
    CaptureCpuBaseline(closeKernel, closeUser);
    run.ok = encoder.Close(stats);
    run.finalizeCpuMs = ProcessCpuMs(closeKernel, closeUser);
    run.closeMs = static_cast<double>(NowUs() - closeBegin) / 1000.0;
    run.peakMemoryMb = PrivateMemoryMb();
    run.hardwareEncoded = stats.hardwareEncoded;
    run.vendor = encoder.ChosenEncoderVendor();

    if (!run.ok) {
        std::printf("[%s] CLOSE=FAILED kind=%s code=0x%08lX\n", label, encoder.LastErrorKind(),
                    static_cast<unsigned long>(encoder.LastErrorCode()));
    }

    run.accepted = stats.acceptedFrames;
    run.rejected = stats.rejectedFrames;
    run.bytes = FileBytes(path);

    std::printf("[%s] open_ms=%.2f submit_total_ms=%.2f submit_avg_ms=%.3f submit_max_ms=%.3f "
                "loop_wall_ms=%.0f finalize_ms=%.2f accepted=%lld rejected=%lld bytes=%lld\n", label,
                run.openMs, run.submitTotalMs,
                run.accepted > 0 ? run.submitTotalMs / static_cast<double>(run.accepted) : 0.0,
                run.submitMaxMs, loopWallMs, run.closeMs, static_cast<long long>(run.accepted),
                static_cast<long long>(run.rejected), run.bytes);
    // 内容是 GPU 位移出来的，本进程的 CPU 剩下的就是编码的：整段进程 CPU ÷ 帧数。
    std::printf("[%s] CPU_MS loop=%.0f (%.3f/帧) finalize=%.0f | MEDIA_US=%lld\n", label,
                run.loopCpuMs,
                run.accepted > 0 ? run.loopCpuMs / static_cast<double>(run.accepted) : 0.0,
                run.finalizeCpuMs, static_cast<long long>(stats.mediaTimeUs));
    std::printf("[%s] PRIVATE_MB before_loop=%.0f peak=%.0f after_close=%.0f | WS_MB peak=%.0f "
                "after_close=%.0f | INPUT_TYPE_VARIANT=%d SERVICE_HR=0x%08lX NV12_BIND=0x%X "
                "HW_OBSERVED=%d VENDOR=%s\n",
                label, memoryBeforeLoopMb, run.peakMemoryMb, PrivateMemoryMb(),
                run.peakWorkingSetMb, WorkingSetMb(), run.inputVariant,
                static_cast<unsigned long>(run.serviceHr), encoder.Nv12BindFlags(),
                run.hardwareEncoded ? 1 : 0, run.vendor.empty() ? "(none)" : run.vendor.c_str());

    return run;
}

void ReportNegativeCases(D2dRenderBackend& backend, MfH264Encoder& encoder, int width, int height,
                         int& failures) {
    // 奇数尺寸：NV12 没有合法的色度划分，必须在入口就拒，不能等到 Finalize 才发现。
    {
        core::VideoEncoderConfig config{};
        config.widthPx = width - 1;
        config.heightPx = height;
        config.targetFps = 30;
        const bool opened = encoder.OpenVideo(config, "encoder_probe_odd.mp4");
        std::printf("ODD_SIZE_OPENED=%d kind=%s EXPECT=0\n", opened ? 1 : 0,
                    encoder.LastErrorKind());
        if (opened) {
            ++failures;
        }
    }

    // 不存在的句柄：要记进 rejectedFrames，统计才不会把没收的帧说成收下了。
    {
        core::VideoEncoderConfig config{};
        config.widthPx = width;
        config.heightPx = height;
        config.targetFps = 30;
        if (!encoder.OpenVideo(config, "encoder_probe_badhandle.mp4")) {
            std::printf("BADHANDLE_SETUP=FAILED kind=%s\n", encoder.LastErrorKind());
            ++failures;
        } else {
            core::EncodedFrame frame{};
            frame.texture = 0;
            frame.timeUs = 0;
            const bool submitted = encoder.Submit(frame);
            core::EncoderStats stats{};
            const bool closed = encoder.Close(stats);
            std::printf("BADHANDLE_SUBMITTED=%d kind=%s REJECTED=%lld CLOSE_OK=%d EXPECT_REJECTED=1\n",
                        submitted ? 1 : 0, encoder.LastErrorKind(),
                        static_cast<long long>(stats.rejectedFrames), closed ? 1 : 0);
            if (submitted || stats.rejectedFrames != 1) {
                ++failures;
            }
        }
    }
}

}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // DPI 感知要在任何枚举之前设（§5.1 采集 seam 那条），这里连尺寸解释都依赖它。
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) == 0) {
        std::printf("DPI_AWARE=FAILED\n");
    }

    const int width = argc > 1 ? std::atoi(argv[1]) : 1280;
    const int height = argc > 2 ? std::atoi(argv[2]) : 720;
    const int fps = argc > 3 ? std::atoi(argv[3]) : 30;
    const int frames = argc > 4 ? std::atoi(argv[4]) : 120;

    if (!MfH264Encoder::StartupFoundation()) {
        std::printf("MFSTARTUP=FAILED\n");
        return 2;
    }

    D2dRenderBackend backend;
    if (!backend.Initialize() || backend.Device() == nullptr) {
        std::printf("BACKEND=FAILED hr=0x%08lX\n", static_cast<unsigned long>(backend.LastHr()));
        return 2;
    }

    int failures = 0;

    // 视频处理器的能力先问一遍：NV12 输出的 bind flag 是试出来的，不是文档说的。
    {
        Nv12Converter probe;
        if (!probe.Initialize(backend.Device(), backend.D3dContext())) {
            std::printf("VIDEODEVICE=FAILED kind=%s\n", probe.LastErrorKind());
            ++failures;
        } else if (!probe.EnsureTargets(width, height)) {
            std::printf("NV12_TARGETS=FAILED kind=%s code=0x%08lX\n", probe.LastErrorKind(),
                        static_cast<unsigned long>(probe.LastHr()));
            ++failures;
        } else {
            UINT support = 0;
            probe.SupportsOutputFormat(&support);
            std::printf("NV12_OUTPUT_SUPPORT=0x%X (INPUT=1 OUTPUT=2) BGRA_SUPPORT=0x%X BIND_USED=0x%X "
                        "POOL=%d\n",
                        probe.Nv12SupportFlags(), probe.BgraSupportFlags(), probe.BindFlagsUsed(),
                        Nv12Converter::kPoolDepth);
        }

        // 无论成不成，都把候选矩阵打出来：E_INVALIDARG 只在知道败于哪一步之后才有意义。
        for (const auto& attempt : probe.BindAttempts()) {
            std::printf("  BIND=0x%-3X texture_hr=0x%08lX view_hr=0x%08lX\n", attempt.bindFlags,
                        static_cast<unsigned long>(attempt.textureHr),
                        static_cast<unsigned long>(attempt.viewHr));
        }
    }

    GpuContent content;
    const core::ImageHandle source = MakeSwatchTexture(backend, width, height, content);
    if (source == 0) {
        std::printf("SOURCE_TEXTURE=FAILED\n");
        return 2;
    }

    MfH264Encoder encoder(backend);
    if (!encoder.Initialize()) {
        std::printf("ENCODER_INITIALIZE=FAILED kind=%s code=0x%08lX\n", encoder.LastErrorKind(),
                    static_cast<unsigned long>(encoder.LastErrorCode()));
        return 2;
    }

    // 默认色彩空间与显式老位段各量一次：两组 Y/UV 都打出来，该用哪一组由数字说。
    encoder.UseColorSpaces(false);
    ReportColorSpace("DEFAULT", encoder, backend, source, width, height, failures);
    encoder.UseColorSpaces(true);
    ReportColorSpace("LEGACY", encoder, backend, source, width, height, failures);

    ReportNegativeCases(backend, encoder, width, height, failures);

    const double memoryBaselineMb = PrivateMemoryMb();
    std::printf("PRIVATE_MEMORY_BASELINE_MB=%.0f\n", memoryBaselineMb);

    // 一个进程里连着跑多档，内存是分不清归因的：上一档的编码器资源会被驱动延后释放，
    // 同一份代码两次跑出 160 MB 与 598 MB 都出现过。所以第 5 个参数给的是「只跑哪一档」，
    // 要干净的数字就一档开一个进程。不给就全跑（看相对关系用）。
    const std::string only = argc > 5 ? argv[5] : "";
    const auto selected = [&only](const char* label) {
        return only.empty() || std::string_view(label) == only;
    };

    struct Config {
        const char* label;
        const char* path;
        bool hardware;
        bool throttleDisabled;
        bool paced;
    };

    // 五档的分工：
    //   HW-UNPACED    裸吞吐，也暴露「写入器替我们攒帧」；
    //   SW-UNPACED    与上一档同内容，编码器身份之外的第二重证据；
    //   HW-PACED      产品真实节奏（硬件）；
    //   HW-THROTTLED  节流到底管不管用；
    //   SW-THROTTLED  没有硬件编码器时产品实际会落在的那一档。
    const Config configs[] = {
        { "HW-UNPACED", "encoder_probe_hw_unpaced.mp4", true, true, false },
        { "SW-UNPACED", "encoder_probe_sw_unpaced.mp4", false, true, false },
        { "HW-PACED", "encoder_probe_hw_paced.mp4", true, true, true },
        { "HW-THROTTLED", "encoder_probe_hw_throttled.mp4", true, false, false },
        { "SW-THROTTLED", "encoder_probe_sw_throttled.mp4", false, false, false },
    };

    constexpr int kHwUnpaced = 0;
    constexpr int kSwUnpaced = 1;
    constexpr int kHwPaced = 2;
    constexpr int kHwThrottled = 3;
    constexpr int kSwThrottled = 4;

    RunResult results[5];

    for (int index = 0; index < 5; ++index) {
        const Config& config = configs[index];
        if (!selected(config.label)) {
            continue;
        }

        RunResult& slot = results[index];
        slot = RunEncode(backend, encoder, source, content, config.path, width, height, fps,
                         frames, config.hardware, config.throttleDisabled, config.paced,
                         config.label);
        std::printf("[%s] AFTER_SESSION_MB=%.0f（单进程只跑这一档时，这行才是能归因的内存数字）\n",
                    config.label, PrivateMemoryMb());

        // 「是不是硬件」现在是可以断言的：观测到的厂商位必须与请求一致。
        if (config.hardware && (!slot.hardwareEncoded || slot.vendor.empty())) {
            std::printf("FAIL %s 请求硬件却观测不到硬件编码器 vendor=(%s)\n", config.label,
                        slot.vendor.c_str());
            ++failures;
        }
        if (!config.hardware && slot.hardwareEncoded) {
            std::printf("FAIL %s 请求软件编码器却观测到硬件 vendor=%s\n", config.label,
                        slot.vendor.c_str());
            ++failures;
        }
        if (!slot.ok || slot.accepted != frames || slot.rejected != 0) {
            std::printf("FAIL %s accepted=%lld rejected=%lld want=%d\n", config.label,
                        static_cast<long long>(slot.accepted),
                        static_cast<long long>(slot.rejected), frames);
            ++failures;
        }
    }

    if (only.empty()) {
        std::printf("COMPARE_BYTES hw=%lld sw=%lld same=%d (同一份内容，编码器不同的可观测差异)\n",
                    results[kHwUnpaced].bytes, results[kSwUnpaced].bytes,
                    results[kHwUnpaced].bytes == results[kSwUnpaced].bytes ? 1 : 0);
        std::printf("COMPARE_PRIVATE_PEAK_MB hw_unpaced=%.0f hw_paced=%.0f hw_throttled=%.0f "
                    "sw_unpaced=%.0f sw_throttled=%.0f WS_PEAK_MB hw_unpaced=%.0f hw_paced=%.0f "
                    "nv12_frame_mb=%.1f\n",
                    results[kHwUnpaced].peakMemoryMb, results[kHwPaced].peakMemoryMb,
                    results[kHwThrottled].peakMemoryMb, results[kSwUnpaced].peakMemoryMb,
                    results[kSwThrottled].peakMemoryMb, results[kHwUnpaced].peakWorkingSetMb,
                    results[kHwPaced].peakWorkingSetMb,
                    static_cast<double>(width) * height * 1.5 / (1024.0 * 1024.0));
    }

    // 产品的用法是「一次录制建一个编码器」。这里连着建拆四次，看拆完之后内存有没有一路涨。
    if (selected("FRESH")) {
        double freshFirstBeforeMb = -1.0;
        double freshLastAfterMb = -1.0;

        for (int round = 0; round < 4; ++round) {
            const double beforeMb = PrivateMemoryMb();
            if (freshFirstBeforeMb < 0.0) {
                freshFirstBeforeMb = beforeMb;
            }

            {
                MfH264Encoder fresh(backend);
                RunEncode(backend, fresh, source, content, "encoder_probe_fresh.mp4", width,
                          height, fps, frames / 4, true, true, false, "FRESH");
            }

            freshLastAfterMb = PrivateMemoryMb();
            std::printf("FRESH round=%d before=%.0f after_destroy=%.0f\n", round, beforeMb,
                        freshLastAfterMb);
        }

        std::printf("FRESH_SPREAD_MB=%.0f (四次生命周期之间，拆完之后仍留在进程里的增量)\n",
                    freshLastAfterMb - freshFirstBeforeMb);
    }

    const double memoryAfterMb = PrivateMemoryMb();
    std::printf("PRIVATE_MEMORY_MB=%.0f baseline=%.0f encoder_delta=%.0f\n", memoryAfterMb,
                memoryBaselineMb, memoryAfterMb - memoryBaselineMb);

    // 每一档的 !ok、丢帧、厂商位不符都已经在各自的分段里计进 failures。
    if (failures != 0) {
        std::printf("OUTCOME=FAILED checks=%d\n", failures);
        return 1;
    }

    std::printf("OUTCOME=OK\n");
    return 0;
}
