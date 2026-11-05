#pragma once

#include "render_backend.h"

#include <windows.h>

#include <d3d11.h>
#include <d2d1_1.h>
#include <dxgi.h>
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

    // 下面两个只服务测量与导出：目标位图要能被 CPU 读过一次，GPU 队列才算真收干。
    bool EnableCpuReadback();

    // centerPixelRgba 非空时顺带回读画面中心的像素，用来验证「真画出来了」。
    bool SyncToCpu(unsigned char* centerPixelRgba = nullptr);

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
