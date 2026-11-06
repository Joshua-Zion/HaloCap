#include "dxgi_desktop_duplication.h"

#include <d3d11.h>

#include <cstring>

namespace halocap::platform::win32 {

namespace {

constexpr std::size_t kMinDirtyBytes = 4096 * sizeof(RECT);

core::IntRect ToIntRect(const RECT& rect) {
    return core::IntRect{ rect.left, rect.top, rect.right, rect.bottom };
}

}

void CopyDirtyRegions(ID3D11DeviceContext* context, ID3D11Texture2D* destination,
                      ID3D11Texture2D* source, const RECT* rects, std::size_t rectCount,
                      std::size_t moveCount, bool& usedFullFrameFallback) {
    usedFullFrameFallback = moveCount > 0;

    if (usedFullFrameFallback) {
        // 移动矩形意味着有整块内容被搬走了，只贴脏矩形会拼出错误的画面；
        // 本机实测这里永远是 0，但 correctness 不能靠那个「永远」。
        context->CopyResource(destination, source);
        return;
    }

    for (std::size_t i = 0; i < rectCount; ++i) {
        const RECT& rect = rects[i];
        if (rect.right <= rect.left || rect.bottom <= rect.top) {
            continue;
        }

        const D3D11_BOX box{ static_cast<UINT>(rect.left), static_cast<UINT>(rect.top), 0,
                             static_cast<UINT>(rect.right), static_cast<UINT>(rect.bottom), 1 };
        context->CopySubresourceRegion(destination, 0, rect.left, rect.top, 0, source, 0, &box);
    }
}

Win32DesktopDuplication::Win32DesktopDuplication(D2dRenderBackend& backend) : backend_(backend) {
    QueryPerformanceFrequency(&qpcFrequency_);
}

Win32DesktopDuplication::~Win32DesktopDuplication() { Close(); }

bool Win32DesktopDuplication::Initialize() {
    outputs_.clear();
    totalOutputs_ = 0;

    if (backend_.Device() == nullptr) {
        errorKind_ = "invalid";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(backend_.Device()->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf())))) {
        errorKind_ = "invalid";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter> ownAdapter;
    if (FAILED(dxgiDevice->GetAdapter(&ownAdapter))) {
        errorKind_ = "invalid";
        return false;
    }

    DXGI_ADAPTER_DESC ownDesc{};
    ownAdapter->GetDesc(&ownDesc);

    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(factory_.GetAddressOf())))) {
        errorKind_ = "invalid";
        return false;
    }

    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(factory_->EnumAdapters1(adapterIndex, &adapter))) {
            break;
        }

        DXGI_ADAPTER_DESC1 adapterDesc{};
        adapter->GetDesc1(&adapterDesc);
        const bool sameAdapter =
            std::memcmp(&adapterDesc.AdapterLuid, &ownDesc.AdapterLuid, sizeof(LUID)) == 0;

        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (FAILED(adapter->EnumOutputs(outputIndex, &output))) {
                break;
            }

            ++totalOutputs_;
            if (!sameAdapter) {
                continue;
            }

            Microsoft::WRL::ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.As(&output1))) {
                continue;
            }

            DXGI_OUTPUT_DESC description{};
            output->GetDesc(&description);
            outputs_.push_back(OutputEntry{ output1, description.DesktopCoordinates });
        }
    }

    if (outputs_.empty()) {
        errorKind_ = "unsupported";
        return false;
    }

    errorKind_ = "ok";
    return true;
}

core::IntRect Win32DesktopDuplication::OutputDesktopBounds(int index) const {
    if (index < 0 || index >= static_cast<int>(outputs_.size())) {
        return core::IntRect{};
    }
    return ToIntRect(outputs_[static_cast<std::size_t>(index)].desktop);
}

bool Win32DesktopDuplication::EnsureMetadataBuffers(std::size_t bytes) {
    if (bytes <= dirtyScratch_.size()) {
        return true;
    }

    // 只增不减：帧循环里不允许重新分配（§4）。
    dirtyScratch_.resize(bytes);
    moveScratch_.resize(bytes);
    return true;
}

bool Win32DesktopDuplication::Open(int outputIndex) {
    Close();

    if (outputIndex < 0 || outputIndex >= static_cast<int>(outputs_.size())) {
        errorKind_ = "invalid";
        return false;
    }

    const OutputEntry& entry = outputs_[static_cast<std::size_t>(outputIndex)];
    desktopBounds_ = ToIntRect(entry.desktop);
    widthPx_ = entry.desktop.right - entry.desktop.left;
    heightPx_ = entry.desktop.bottom - entry.desktop.top;
    if (widthPx_ <= 0 || heightPx_ <= 0) {
        errorKind_ = "invalid";
        return false;
    }

    const HRESULT duplicated = entry.output->DuplicateOutput(backend_.Device(), &duplication_);
    if (duplicated == E_ACCESSDENIED) {
        errorKind_ = "busy";
        return false;
    }
    if (duplicated == DXGI_ERROR_UNSUPPORTED) {
        errorKind_ = "unsupported";
        return false;
    }
    if (FAILED(duplicated)) {
        errorKind_ = "invalid";
        return false;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = static_cast<UINT>(widthPx_);
    desc.Height = static_cast<UINT>(heightPx_);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(backend_.Device()->CreateTexture2D(&desc, nullptr, &resident_))) {
        errorKind_ = "invalid";
        duplication_.Reset();
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    if (FAILED(resident_.As(&surface))) {
        errorKind_ = "invalid";
        duplication_.Reset();
        resident_.Reset();
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;
    props.colorContext = nullptr;

    if (FAILED(backend_.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &residentBitmap_))) {
        errorKind_ = "invalid";
        duplication_.Reset();
        resident_.Reset();
        return false;
    }

    targetHandle_ = backend_.RegisterGpuImage(residentBitmap_.Get());
    if (targetHandle_ == 0) {
        errorKind_ = "invalid";
        Close();
        return false;
    }

    EnsureMetadataBuffers(kMinDirtyBytes);
    fullFrameFallback_ = false;
    errorKind_ = "ok";
    return true;
}

void Win32DesktopDuplication::Close() {
    if (targetHandle_ != 0) {
        backend_.ReleaseImage(targetHandle_);
        targetHandle_ = 0;
    }

    residentBitmap_.Reset();
    resident_.Reset();
    duplication_.Reset();
    desktopBounds_ = core::IntRect{};
    widthPx_ = 0;
    heightPx_ = 0;
}

bool Win32DesktopDuplication::WaitForUpdate(std::int64_t timeoutMs, core::FrameUpdate& out) {
    if (!duplication_) {
        errorKind_ = "invalid";
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info{};
    Microsoft::WRL::ComPtr<IDXGIResource> resource;
    const HRESULT acquired =
        duplication_->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, &resource);

    if (acquired == DXGI_ERROR_WAIT_TIMEOUT) {
        errorKind_ = "timeout";
        return false;
    }
    if (acquired == E_ACCESSDENIED) {
        errorKind_ = "busy";
        return false;
    }
    if (acquired == DXGI_ERROR_ACCESS_LOST) {
        errorKind_ = "lost";
        return false;
    }
    if (FAILED(acquired)) {
        errorKind_ = "invalid";
        return false;
    }

    // 下面任何一步失败都必须 ReleaseFrame，否则复制通道会被自己卡住。
    std::size_t dirtyCount = 0;
    std::size_t moveCount = 0;
    bool ok = true;

    if (info.TotalMetadataBufferSize > 0) {
        ok = EnsureMetadataBuffers(info.TotalMetadataBufferSize);

        UINT requiredBytes = 0;
        if (ok) {
            ok = SUCCEEDED(duplication_->GetFrameDirtyRects(
                info.TotalMetadataBufferSize, reinterpret_cast<RECT*>(dirtyScratch_.data()),
                &requiredBytes));
            dirtyCount = requiredBytes / sizeof(RECT);
        }

        requiredBytes = 0;
        if (ok) {
            ok = SUCCEEDED(duplication_->GetFrameMoveRects(
                info.TotalMetadataBufferSize,
                reinterpret_cast<DXGI_OUTDUPL_MOVE_RECT*>(moveScratch_.data()), &requiredBytes));
            moveCount = requiredBytes / sizeof(DXGI_OUTDUPL_MOVE_RECT);
        }
    }

    Microsoft::WRL::ComPtr<ID3D11Texture2D> frame;
    if (ok) {
        ok = SUCCEEDED(resource.As(&frame));
    }

    if (ok) {
        CopyDirtyRegions(backend_.D3dContext(), resident_.Get(), frame.Get(),
                         reinterpret_cast<const RECT*>(dirtyScratch_.data()), dirtyCount, moveCount,
                         fullFrameFallback_);
    }

    if (ok) {
        out.timestampUs = info.LastPresentTime.QuadPart > 0
                              ? info.LastPresentTime.QuadPart * 1000000 / qpcFrequency_.QuadPart
                              : 0;
        out.accumulatedFrames = info.AccumulatedFrames;
        out.contentChanged = info.AccumulatedFrames > 0;
        out.rectsCoalesced = info.RectsCoalesced != FALSE;
        out.target = targetHandle_;
        out.desktopBounds = desktopBounds_;
        out.widthPx = widthPx_;
        out.heightPx = heightPx_;
        out.dirtyRects.clear();

        // 脏矩形是这个输出自己的坐标（左上角为 0,0），桌面坐标要加上 desktopBounds 的左上角。
        const auto* rects = reinterpret_cast<const RECT*>(dirtyScratch_.data());
        for (std::size_t i = 0; i < dirtyCount; ++i) {
            out.dirtyRects.push_back(ToIntRect(rects[i]));
        }
    }

    duplication_->ReleaseFrame();

    if (!ok) {
        errorKind_ = "invalid";
        return false;
    }

    errorKind_ = "ok";
    return true;
}

}
