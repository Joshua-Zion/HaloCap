#include "d2d_render_backend.h"

#include <d3d11.h>

#include <type_traits>
#include <utility>

namespace halocap::platform::win32 {

namespace {

constexpr float kDpi = 96.0f;

D2D1_RECT_F ToD2D(const core::Rect& rect) {
    return D2D1_RECT_F{ rect.left, rect.top, rect.right, rect.bottom };
}

D2D1_POINT_2F ToD2D(const core::Point& point) {
    return D2D1_POINT_2F{ point.x, point.y };
}

D2D1_COLOR_F ToD2D(const core::Color& color) {
    return D2D1_COLOR_F{ color.r, color.g, color.b, color.a };
}

D2D1_BITMAP_PROPERTIES1 PixelProps(D2D1_BITMAP_OPTIONS options) {
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = kDpi;
    props.dpiY = kDpi;
    props.bitmapOptions = options;
    props.colorContext = nullptr;
    return props;
}

}

bool D2dRenderBackend::Initialize() {
    lastHr_ = D3D11CreateDevice(nullptr,
                                D3D_DRIVER_TYPE_HARDWARE,
                                nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                nullptr,
                                0,
                                D3D11_SDK_VERSION,
                                &device_,
                                nullptr,
                                &context_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = device_.As(&dxgiDevice_);
    if (FAILED(lastHr_)) {
        return false;
    }

    // 交换链由 DXGI factory 建，与 D3D 设备同源。
    lastHr_ = CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory_.GetAddressOf()));
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                IID_PPV_ARGS(factory_.GetAddressOf()));
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = factory_->CreateDevice(dxgiDevice_.Get(), &d2dDevice_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    return SUCCEEDED(lastHr_);
}

bool D2dRenderBackend::Resize(int widthPx, int heightPx) {
    if (!dc_ || widthPx <= 0 || heightPx <= 0) {
        lastHr_ = E_INVALIDARG;
        return false;
    }

    if (target_ && widthPx == width_ && heightPx == height_) {
        return true;
    }

    // Resize 只改尺寸并放掉旧资源，不分配：真实覆盖层经常只做「挂到窗口」而不需要离屏目标，
    // 提前分配就等于常驻一块 4K 目标（33 MB）+ 可能的 STAGING。
    width_ = widthPx;
    height_ = heightPx;
    targetBitmap_.Reset();
    targetSurface_.Reset();
    target_.Reset();
    staging_.Reset();
    return true;
}

bool D2dRenderBackend::EnsureOffscreenTarget() {
    if (targetBitmap_ && target_) {
        return true;
    }

    if (!dc_ || width_ <= 0 || height_ <= 0) {
        lastHr_ = E_FAIL;
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // 实测：只给 RENDER_TARGET 时 CreateBitmapFromDxgiSurface 返回 E_INVALIDARG，
    // 必须同时给 SHADER_RESOURCE（与尺寸、alphaMode 无关）。
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    lastHr_ = device_->CreateTexture2D(&desc, nullptr, &target_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = target_.As(&targetSurface_);
    if (FAILED(lastHr_)) {
        return false;
    }

    const D2D1_BITMAP_PROPERTIES1 targetProps = PixelProps(D2D1_BITMAP_OPTIONS_TARGET);
    lastHr_ = dc_->CreateBitmapFromDxgiSurface(targetSurface_.Get(), &targetProps, &targetBitmap_);
    if (FAILED(lastHr_)) {
        return false;
    }

    dc_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return !readbackEnabled_ || EnableCpuReadback();
}

core::ImageHandle D2dRenderBackend::RegisterImage(const core::ImageRef& image) {
    if (!dc_ || image.pixels == nullptr || image.width <= 0 || image.height <= 0 ||
        image.stride <= 0) {
        lastHr_ = E_INVALIDARG;
        return 0;
    }

    const D2D1_SIZE_U size{ static_cast<UINT32>(image.width), static_cast<UINT32>(image.height) };
    const D2D1_BITMAP_PROPERTIES1 bitmapProps = PixelProps(D2D1_BITMAP_OPTIONS_NONE);
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;

    lastHr_ = dc_->CreateBitmap(size, image.pixels, static_cast<UINT32>(image.stride),
                                &bitmapProps, &bitmap);
    if (FAILED(lastHr_)) {
        return 0;
    }

    images_.push_back(std::move(bitmap));
    return static_cast<core::ImageHandle>(images_.size());
}

void D2dRenderBackend::ReleaseImage(core::ImageHandle handle) {
    if (handle > 0 && static_cast<std::size_t>(handle) <= images_.size()) {
        images_[static_cast<std::size_t>(handle) - 1].Reset();
    }
}

core::ImageHandle D2dRenderBackend::RegisterGpuImage(ID2D1Bitmap1* bitmap) {
    if (bitmap == nullptr) {
        lastHr_ = E_INVALIDARG;
        return 0;
    }

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> kept;
    lastHr_ = bitmap->QueryInterface(IID_PPV_ARGS(kept.GetAddressOf()));
    if (FAILED(lastHr_)) {
        return 0;
    }

    images_.push_back(std::move(kept));
    return static_cast<core::ImageHandle>(images_.size());
}

bool D2dRenderBackend::TextureFor(core::ImageHandle handle, ID3D11Texture2D** outTexture) {
    if (handle <= 0 || handle > static_cast<core::ImageHandle>(images_.size()) ||
        outTexture == nullptr) {
        lastHr_ = E_INVALIDARG;
        return false;
    }

    ID2D1Bitmap1* bitmap = images_[static_cast<size_t>(handle) - 1].Get();
    if (bitmap == nullptr) {
        lastHr_ = E_FAIL;
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    lastHr_ = bitmap->GetSurface(&surface);
    if (FAILED(lastHr_)) {
        return false;
    }

    return SUCCEEDED(surface->QueryInterface(IID_PPV_ARGS(outTexture)));
}

bool D2dRenderBackend::CountInkedPixels(int* count) {
    if (!staging_ || !target_ || !context_ || count == nullptr) {
        lastHr_ = E_FAIL;
        return false;
    }

    context_->CopyResource(staging_.Get(), target_.Get());
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    lastHr_ = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(lastHr_)) {
        return false;
    }

    const auto* bits = reinterpret_cast<const unsigned char*>(mapped.pData);
    int inked = 0;
    for (int y = 0; y < height_; ++y) {
        const unsigned char* row = bits + static_cast<std::size_t>(mapped.RowPitch) * y;
        for (int x = 0; x < width_; ++x) {
            inked += row[static_cast<std::size_t>(x) * 4 + 3] != 0 ? 1 : 0;
        }
    }

    context_->Unmap(staging_.Get(), 0);
    *count = inked;
    return true;
}

ID2D1SolidColorBrush* D2dRenderBackend::BrushFor(const core::Color& color) {
    for (auto& entry : brushes_) {
        if (entry.first == color) {
            return entry.second.Get();
        }
    }

    const D2D1_COLOR_F d2dColor = ToD2D(color);
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    lastHr_ = dc_->CreateSolidColorBrush(&d2dColor, nullptr, &brush);
    if (FAILED(lastHr_)) {
        return nullptr;
    }

    brushes_.emplace_back(color, brush);
    return brush.Get();
}

void D2dRenderBackend::Render(const core::FramePlan& plan, core::IntRect clip) {
    if (!dc_) {
        lastHr_ = E_FAIL;
        return;
    }

    // 离屏路径的目标纹理是懒建的：只有真的要往自己身上画时才分配。
    if (!windowTarget_ && !EnsureOffscreenTarget()) {
        return;
    }

    ID2D1Bitmap1* target = windowTarget_ ? windowTarget_.Get() : targetBitmap_.Get();
    if (target == nullptr) {
        lastHr_ = E_FAIL;
        return;
    }

    // 目标纹理也会被 D3D 侧用到（复制、编码），所以每帧重新绑定再画。
    dc_->SetTarget(target);
    dc_->BeginDraw();

    const bool clipped = !clip.IsEmpty();
    if (clipped) {
        const D2D1_RECT_F clipRect{ static_cast<float>(clip.left), static_cast<float>(clip.top),
                                    static_cast<float>(clip.right), static_cast<float>(clip.bottom) };
        dc_->PushAxisAlignedClip(&clipRect, D2D1_ANTIALIAS_MODE_ALIASED);
    }

    // 只清要重绘的那块：透明覆盖层必须每帧从零开始画这一块，否则上一次的笔迹会留着；
    // 而 clip 之外必须保留，不然部分重绘就没有意义了。
    dc_->Clear(static_cast<const D2D1_COLOR_F*>(nullptr));

    for (const core::Item& item : plan.Items()) {
        std::visit(
            [this](const auto& specific) {
                using T = std::decay_t<decltype(specific)>;

                if constexpr (std::is_same_v<T, core::FillRect>) {
                    const D2D1_RECT_F rect = ToD2D(specific.rect);
                    dc_->FillRectangle(&rect, BrushFor(specific.color));
                } else if constexpr (std::is_same_v<T, core::StrokeRect>) {
                    const D2D1_RECT_F rect = ToD2D(specific.rect);
                    dc_->DrawRectangle(&rect, BrushFor(specific.color), specific.thickness, nullptr);
                } else if constexpr (std::is_same_v<T, core::Segment>) {
                    dc_->DrawLine(ToD2D(specific.from), ToD2D(specific.to),
                                  BrushFor(specific.color), specific.thickness, nullptr);
                } else {
                    DrawBlit(specific);
                }
            },
            item);
    }

    if (clipped) {
        dc_->PopAxisAlignedClip();
    }

    lastHr_ = dc_->EndDraw();
}

void D2dRenderBackend::DrawBlit(const core::Blit& blit) {
    const std::size_t index = static_cast<std::size_t>(blit.image) - 1;
    if (blit.image <= 0 || index >= images_.size() || !images_[index]) {
        return;
    }

    const D2D1_RECT_F destination = ToD2D(blit.destination);
    const core::Rect source = blit.source.Normalized();
    const D2D1_RECT_F sourceRect = ToD2D(source);

    dc_->DrawBitmap(images_[index].Get(),
                    &destination,
                    blit.opacity,
                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                    source.IsEmpty() ? nullptr : &sourceRect);
}

void D2dRenderBackend::Flush() {
    if (!dc_) {
        return;
    }

    lastHr_ = dc_->Flush();
    if (context_) {
        context_->Flush();
    }
}

bool D2dRenderBackend::AttachWindow(core::NativeHandle handle, int widthPx, int heightPx) {
    if (!dxgiFactory_ || !device_ || !dc_ || handle == nullptr || widthPx <= 0 || heightPx <= 0) {
        lastHr_ = E_INVALIDARG;
        return false;
    }

    // 尺寸变了就先把离屏目标与回读用的 STAGING 一起重建，两套目标共用同一套尺寸口径。
    if (widthPx != width_ || heightPx != height_) {
        if (!Resize(widthPx, heightPx)) {
            return false;
        }
    }

    windowTarget_.Reset();
    swapChain_.Reset();
    swapChainLatency_.Reset();
    frameLatencyWaitable_ = nullptr;
    compositionVisual_.Reset();
    compositionTarget_.Reset();

    auto* hwnd = static_cast<HWND>(handle);

    if (!composition_) {
        lastHr_ = DCompositionCreateDevice(dxgiDevice_.Get(), __uuidof(IDCompositionDevice),
                                           reinterpret_cast<void**>(composition_.GetAddressOf()));
        if (FAILED(lastHr_)) {
            return false;
        }
    }

    lastHr_ = composition_->CreateTargetForHwnd(hwnd, TRUE, &compositionTarget_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = composition_->CreateVisual(&compositionVisual_);
    if (FAILED(lastHr_)) {
        return false;
    }

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = static_cast<UINT>(widthPx);
    desc.Height = static_cast<UINT>(heightPx);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    // 实测：composition 链只接受 STRETCH；alpha 必须是预乘且只能走 composition。
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    // flip 模型的 Present(1) 不会等 vblank（实测可见状态下仍是 ~4 ms），
    // 真正的节奏只能靠 waitable object。
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    lastHr_ = dxgiFactory_->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_);
    if (FAILED(lastHr_)) {
        return false;
    }

    swapChainLatency_.Reset();
    lastHr_ = swapChain_.As(&swapChainLatency_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = swapChainLatency_->SetMaximumFrameLatency(1);
    if (FAILED(lastHr_)) {
        return false;
    }

    frameLatencyWaitable_ = swapChainLatency_->GetFrameLatencyWaitableObject();

    Microsoft::WRL::ComPtr<IDXGISurface> backBuffer;
    lastHr_ = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(lastHr_)) {
        return false;
    }

    // 交换链后缓冲当 D2D 目标时必须带 CANNOT_DRAW，否则 CreateBitmapFromDxgiSurface 直接 E_INVALIDARG。
    const D2D1_BITMAP_PROPERTIES1 targetProps =
        PixelProps(static_cast<D2D1_BITMAP_OPTIONS>(D2D1_BITMAP_OPTIONS_TARGET |
                                                    D2D1_BITMAP_OPTIONS_CANNOT_DRAW));
    lastHr_ = dc_->CreateBitmapFromDxgiSurface(backBuffer.Get(), &targetProps, &windowTarget_);
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = compositionVisual_->SetContent(swapChain_.Get());
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = compositionTarget_->SetRoot(compositionVisual_.Get());
    if (FAILED(lastHr_)) {
        return false;
    }

    lastHr_ = composition_->Commit();
    if (FAILED(lastHr_)) {
        return false;
    }

    window_ = handle;
    return true;
}

void D2dRenderBackend::DetachWindow() {
    if (compositionTarget_ && composition_) {
        compositionTarget_->SetRoot(nullptr);
        composition_->Commit();
    }

    compositionVisual_.Reset();
    compositionTarget_.Reset();
    windowTarget_.Reset();
    swapChainLatency_.Reset();
    swapChain_.Reset();
    frameLatencyWaitable_ = nullptr;  // 句柄归交换链所有，不关。
    window_ = nullptr;

    if (dc_ && targetBitmap_) {
        dc_->SetTarget(targetBitmap_.Get());
    }

    // 放掉引用不等于 DXGI 把缓存的视频内存吐回来；Win8+ 要显式 Trim。
    // 这条是「Detach 后私有内存一分没降」的直接对策，效果由 surface_probe 量。
    Microsoft::WRL::ComPtr<IDXGIDevice3> trimmable;
    if (SUCCEEDED(device_.As(&trimmable))) {
        trimmable->Trim();
    }
}

void D2dRenderBackend::Present() {
    if (!swapChain_) {
        lastHr_ = E_FAIL;
        return;
    }

    lastHr_ = swapChain_->Present(0, 0);
}

void D2dRenderBackend::WaitForFrameSlot() {
    if (frameLatencyWaitable_ == nullptr) {
        return;
    }

    const DWORD result = WaitForSingleObject(frameLatencyWaitable_, 1000);
    if (result == WAIT_TIMEOUT) {
        lastHr_ = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
}

bool D2dRenderBackend::EnableCpuReadback() {
    if (!device_ || width_ <= 0 || height_ <= 0) {
        lastHr_ = E_FAIL;
        return false;
    }

    readbackEnabled_ = true;
    staging_.Reset();

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(width_);
    desc.Height = static_cast<UINT>(height_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    lastHr_ = device_->CreateTexture2D(&desc, nullptr, &staging_);
    return SUCCEEDED(lastHr_);
}

bool D2dRenderBackend::SyncToCpu(unsigned char* centerPixelRgba) {
    if (!staging_ || !target_ || !context_) {
        lastHr_ = E_FAIL;
        return false;
    }

    context_->CopyResource(staging_.Get(), target_.Get());
    context_->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    lastHr_ = context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(lastHr_)) {
        return false;
    }

    // 行距由驱动决定，不能假设等于宽×4。
    const auto* bits = reinterpret_cast<const unsigned char*>(mapped.pData);
    const std::size_t offset = static_cast<std::size_t>(mapped.RowPitch) * (height_ / 2) +
                               static_cast<std::size_t>(width_ / 2) * 4;

    if (centerPixelRgba != nullptr) {
        for (int channel = 0; channel < 4; ++channel) {
            centerPixelRgba[channel] = bits[offset + channel];
        }
    } else {
        // 不取用像素时也要真读一次，否则整个映射可能被优化掉。
        observedByte_ = bits[offset];
    }

    context_->Unmap(staging_.Get(), 0);
    return true;
}

}
