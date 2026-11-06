#pragma once

#include "capture_source.h"
#include "d2d_render_backend.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <vector>

namespace halocap::platform::win32 {

// 把脏矩形从 src 拷进常驻纹理。单独抽出来是为了能脱离复制权做验证：
// 「只搬变化区、脏区之外一律不动」这件事必须可测，不能只在能复制的机器上顺带跑到。
// moveCount > 0 时退化成整帧拷贝——只贴脏矩形会拼出错误的画面。
void CopyDirtyRegions(ID3D11DeviceContext* context, ID3D11Texture2D* destination,
                      ID3D11Texture2D* source, const RECT* rects, std::size_t rectCount,
                      std::size_t moveCount, bool& usedFullFrameFallback);

// ICaptureSource 的桌面复制实现。三条硬约束都来自实测（§4、§5.1 spike 2）：
//   1. 用渲染后端那台设备——采集、合成、编码必须是同一个设备/适配器，
//      所以这里只暴露「与本机设备同适配器」的输出，别的直接标成不可复制；
//   2. GetFrameDirtyRects / GetFrameMoveRects 的容量与 required 都是**字节数**；
//   3. 复制权被占用（E_ACCESSDENIED）与等不到帧（WAIT_TIMEOUT）都是常态分支，不是错误。
class Win32DesktopDuplication final : public core::ICaptureSource {
public:
    explicit Win32DesktopDuplication(D2dRenderBackend& backend);

    ~Win32DesktopDuplication() override;

    // 枚举输出：outputs_ 只收本机设备所在适配器的，totalOutputs_ 是整个系统的数量。
    bool Initialize();

    int OutputCount() const { return static_cast<int>(outputs_.size()); }

    int TotalOutputCount() const { return totalOutputs_; }

    core::IntRect OutputDesktopBounds(int index) const;

    bool Open(int outputIndex) override;

    void Close() override;

    bool WaitForUpdate(std::int64_t timeoutMs, core::FrameUpdate& out) override;

    const char* LastErrorKind() const override { return errorKind_; }

    core::IntRect DesktopBounds() const override { return desktopBounds_; }

    // 出现移动矩形时是否退化成整帧拷贝（本机实测永远是 0 个移动矩形，但别赌）。
    bool UsedFullFrameFallback() const { return fullFrameFallback_; }

private:
    struct OutputEntry {
        Microsoft::WRL::ComPtr<IDXGIOutput1> output;
        RECT desktop{};
    };

    bool EnsureMetadataBuffers(std::size_t bytes);

    D2dRenderBackend& backend_;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory_;
    std::vector<OutputEntry> outputs_;
    int totalOutputs_ = 0;

    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> duplication_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> resident_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> residentBitmap_;
    core::ImageHandle targetHandle_ = 0;

    // 帧循环里不重新分配：容量只增不减。
    std::vector<BYTE> dirtyScratch_;
    std::vector<BYTE> moveScratch_;

    core::IntRect desktopBounds_{};
    int widthPx_ = 0;
    int heightPx_ = 0;
    LARGE_INTEGER qpcFrequency_{};
    bool fullFrameFallback_ = false;
    const char* errorKind_ = "none";
};

}
