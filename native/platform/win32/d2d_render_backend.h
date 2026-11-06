#pragma once

#include "render_backend.h"

#include <windows.h>

#include <d3d11.h>
#include <d2d1_1.h>
#include <dcomp.h>
#include <dxgi.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>
#include <wrl/client.h>

#include <vector>

namespace halocap::platform::win32 {

// IRenderBackend 的 Direct2D 实现：常驻 D3D11 纹理当目标，走 ID2D1Device + ID2D1DeviceContext
// （spike 3 用的是 D2D 1.0 的 CreateDxgiSurfaceRenderTarget，这里换成 2.0 真正要用的那条链）。
class D2dRenderBackend final : public core::IRenderBackend {
public:
    bool Initialize();

    HRESULT LastHr() const { return lastHr_; }

    bool Resize(int widthPx, int heightPx) override;

    core::ImageHandle RegisterImage(const core::ImageRef& image) override;

    void ReleaseImage(core::ImageHandle handle) override;

    void Render(const core::FramePlan& plan, core::IntRect clip) override;

    void Flush() override;

    bool AttachWindow(core::NativeHandle handle, int widthPx, int heightPx) override;

    void DetachWindow() override;

    void Present() override;

    // 下面两个只服务测量与导出：目标位图要能被 CPU 读过一次，GPU 队列才算真收干。
    bool EnableCpuReadback();

    // centerPixelRgba 非空时顺带回读画面中心的像素，用来验证「真画出来了」。
    bool SyncToCpu(unsigned char* centerPixelRgba = nullptr);

    // 数一下目标上不透明的像素，验证文本这类「有东西才该有数」的输出。
    bool CountInkedPixels(int* count);

    // 平台内部共享：字体栅格器要往自己的图集纹理里画，画完由这里换成显示列表能引用的句柄。
    ID2D1DeviceContext* Dc() const { return dc_.Get(); }
    ID3D11Device* Device() const { return device_.Get(); }
    ID2D1Factory1* Factory() const { return factory_.Get(); }
    core::ImageHandle RegisterGpuImage(ID2D1Bitmap1* bitmap);

    // 有句柄说明这条链带 waitable object，Present 的节流才不是「自己骗自己」。
    HANDLE FrameLatencyWaitable() const { return frameLatencyWaitable_; }

    // 正确的节奏是「画之前等一个帧槽」；在 Present 之后等是量不到 vblank 的。
    void WaitForFrameSlot() override;

private:
    ID2D1SolidColorBrush* BrushFor(const core::Color& color);

    void DrawBlit(const core::Blit& blit);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2dDevice_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> target_;
    Microsoft::WRL::ComPtr<IDXGISurface> targetSurface_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> targetBitmap_;

    Microsoft::WRL::ComPtr<IDXGIFactory2> dxgiFactory_;
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain_;
    Microsoft::WRL::ComPtr<IDXGISwapChain2> swapChainLatency_;
    HANDLE frameLatencyWaitable_ = nullptr;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> windowTarget_;
    core::NativeHandle window_ = nullptr;

    // 上屏用的是 DirectComposition 那套：实测 HWND 上的 flip 交换链不接受 premultiplied alpha。
    Microsoft::WRL::ComPtr<IDCompositionDevice> composition_;
    Microsoft::WRL::ComPtr<IDCompositionTarget> compositionTarget_;
    Microsoft::WRL::ComPtr<IDCompositionVisual> compositionVisual_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_;
    bool readbackEnabled_ = false;
    int width_ = 0;
    int height_ = 0;

    // 回读时真取的那个字节；留着只为防止映射被优化掉。
    volatile unsigned int observedByte_ = 0;

    // 句柄从 1 起算，0 表示无效；空槽位留着不回收，帧循环里不做搬移。
    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap1>> images_;
    std::vector<std::pair<core::Color, Microsoft::WRL::ComPtr<ID2D1SolidColorBrush>>> brushes_;

    HRESULT lastHr_ = S_OK;
};

}
