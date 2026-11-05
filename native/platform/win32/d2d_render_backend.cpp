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

    // 换尺寸要整条重建：目标位图引用的是那张纹理本身。
    targetBitmap_.Reset();
    targetSurface_.Reset();
    target_.Reset();
    staging_.Reset();
    width_ = widthPx;
    height_ = heightPx;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(widthPx);
    desc.Height = static_cast<UINT>(heightPx);
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

    dc_->SetTarget(targetBitmap_.Get());
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
    if (!dc_ || !targetBitmap_) {
        lastHr_ = E_FAIL;
        return;
    }

    // 目标纹理也会被 D3D 侧用到（复制、编码），所以每帧重新绑定再画。
    dc_->SetTarget(targetBitmap_.Get());
    dc_->BeginDraw();

    const bool clipped = !clip.IsEmpty();
    if (clipped) {
        const D2D1_RECT_F clipRect{ static_cast<float>(clip.left), static_cast<float>(clip.top),
                                    static_cast<float>(clip.right), static_cast<float>(clip.bottom) };
        dc_->PushAxisAlignedClip(&clipRect, D2D1_ANTIALIAS_MODE_ALIASED);
    }

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
