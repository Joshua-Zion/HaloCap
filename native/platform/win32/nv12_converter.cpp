#include "nv12_converter.h"

#include <dxgi.h>

namespace halocap::platform::win32 {

namespace {

// NV12 输出纹理的 bind flag 候选，按本机实测的可行性排序（Intel UHD 与 RTX 4060 结论一致）：
// 视频处理器的输出视图**必须**带 D3D11_BIND_RENDER_TARGET，只给 SHADER_RESOURCE 或 DECODER
// 一律 E_INVALIDARG——「NV12 不能当渲染目标」「输出要 DECODER」两种常见说法都不成立。
// RT|SRV 也通过，所以优先选它：纹理除了进编码器还能被采样，留给后续叠加。
constexpr UINT kBindCandidates[] = {
    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    D3D11_BIND_RENDER_TARGET,
    D3D11_BIND_SHADER_RESOURCE,
    D3D11_BIND_DECODER,
    D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE,
    0,
};

}

bool Nv12Converter::Initialize(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (device == nullptr || context == nullptr) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    device_ = device;
    context_ = context;

    lastHr_ = device_->QueryInterface(__uuidof(ID3D11VideoDevice), &videoDevice_);
    if (FAILED(lastHr_) || !videoDevice_) {
        errorKind_ = "novideo";
        return false;
    }

    lastHr_ = context_->QueryInterface(__uuidof(ID3D11VideoContext), &videoContext_);
    if (FAILED(lastHr_) || !videoContext_) {
        errorKind_ = "novideo";
        videoDevice_.Reset();
        return false;
    }

    errorKind_ = "ok";
    return true;
}

bool Nv12Converter::EnsureProcessor(int widthPx, int heightPx) {
    if (enumerator_ && poolWidth_ == widthPx && poolHeight_ == heightPx) {
        return true;
    }

    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = 30;
    desc.InputFrameRate.Denominator = 1;
    desc.InputWidth = static_cast<UINT>(widthPx);
    desc.InputHeight = static_cast<UINT>(heightPx);
    desc.OutputFrameRate = desc.InputFrameRate;
    desc.OutputWidth = static_cast<UINT>(widthPx);
    desc.OutputHeight = static_cast<UINT>(heightPx);
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    lastHr_ = videoDevice_->CreateVideoProcessorEnumerator(&desc, &enumerator_);
    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        return false;
    }

    lastHr_ = videoDevice_->CreateVideoProcessor(enumerator_.Get(), 0, &processor_);
    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        enumerator_.Reset();
        return false;
    }

    // 能力位在这里问一次并缓存：INPUT=1、OUTPUT=2，两者都不满足就说明这台设备做不了这条路。
    UINT nv12 = 0;
    UINT bgra = 0;
    enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, &nv12);
    enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_B8G8R8A8_UNORM, &bgra);
    nv12Support_ = nv12;
    bgraSupport_ = bgra;

    return true;
}

bool Nv12Converter::CreateSlot(Slot& slot, UINT bindFlags, int widthPx, int heightPx,
                               BindAttempt* attempt) {
    D3D11_TEXTURE2D_DESC texDesc{};
    texDesc.Width = static_cast<UINT>(widthPx);
    texDesc.Height = static_cast<UINT>(heightPx);
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_NV12;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = bindFlags;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    attempt->textureHr = device_->CreateTexture2D(&texDesc, nullptr, &texture);
    if (FAILED(attempt->textureHr)) {
        lastHr_ = attempt->textureHr;
        return false;
    }

    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC viewDesc{};
    viewDesc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipSlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    // 三个 Create*（处理器、输入视图、输出视图）都挂在 ID3D11VideoDevice 上，
    // 只有状态设置与 Blt 在 ID3D11VideoContext 上——按直觉写会写成 context 的，编不过。
    attempt->viewHr = videoDevice_->CreateVideoProcessorOutputView(texture.Get(), enumerator_.Get(),
                                                                   &viewDesc, &view);
    if (FAILED(attempt->viewHr)) {
        lastHr_ = attempt->viewHr;
        return false;
    }

    slot.texture = texture;
    slot.view = view;
    return true;
}

bool Nv12Converter::EnsureTargets(int widthPx, int heightPx) {
    if (widthPx <= 0 || heightPx <= 0) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    if (static_cast<int>(slots_.size()) == kPoolDepth && poolWidth_ == widthPx &&
        poolHeight_ == heightPx) {
        return true;
    }

    if (!EnsureProcessor(widthPx, heightPx)) {
        return false;
    }

    // 尺寸变了就把环重建掉：旧纹理还挂在 MF 样本里时不该复用，交给调用方在 Close 之后调。
    slots_.clear();
    inputView_.Reset();
    inputTexture_.Reset();

    attempts_.clear();

    for (UINT bindFlags : kBindCandidates) {
        std::vector<Slot> attemptSlots;
        attemptSlots.reserve(kPoolDepth);

        bool ok = true;
        for (int i = 0; i < kPoolDepth && ok; ++i) {
            Slot slot;
            BindAttempt attempt;
            attempt.bindFlags = bindFlags;
            ok = CreateSlot(slot, bindFlags, widthPx, heightPx, &attempt);
            attempts_.push_back(attempt);
            if (ok) {
                attemptSlots.push_back(std::move(slot));
            }
        }

        if (ok && static_cast<int>(attemptSlots.size()) == kPoolDepth) {
            slots_ = std::move(attemptSlots);
            bindFlagsUsed_ = bindFlags;
            poolWidth_ = widthPx;
            poolHeight_ = heightPx;
            nextSlot_ = 0;
            errorKind_ = "ok";
            return true;
        }
    }

    errorKind_ = "unsupported";
    return false;
}

int Nv12Converter::AcquireSlot() {
    if (slots_.empty()) {
        return -1;
    }
    const int index = nextSlot_;
    nextSlot_ = (nextSlot_ + 1) % static_cast<int>(slots_.size());
    return index;
}

ID3D11Texture2D* Nv12Converter::SlotTexture(int index) const {
    if (index < 0 || index >= static_cast<int>(slots_.size())) {
        return nullptr;
    }
    return slots_[static_cast<size_t>(index)].texture.Get();
}

bool Nv12Converter::EnsureInputView(ID3D11Texture2D* bgraSource) {
    if (inputView_ && inputTexture_.Get() == bgraSource) {
        return true;
    }

    if (bgraSource == nullptr) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc{};
    desc.FourCC = 0;
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.ArraySlice = 0;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> view;
    lastHr_ = videoDevice_->CreateVideoProcessorInputView(bgraSource, enumerator_.Get(), &desc, &view);
    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        return false;
    }

    inputTexture_ = bgraSource;
    inputView_ = view;
    return true;
}

bool Nv12Converter::ConvertToSlot(ID3D11Texture2D* bgraSource, int slotIndex) {
    if (slotIndex < 0 || slotIndex >= static_cast<int>(slots_.size())) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    if (!EnsureInputView(bgraSource)) {
        return false;
    }

    videoContext_->VideoProcessorSetStreamFrameFormat(processor_.Get(), 0,
                                                      D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    if (hasSourceRect_) {
        videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, TRUE, &sourceRect_);
    } else {
        videoContext_->VideoProcessorSetStreamSourceRect(processor_.Get(), 0, FALSE, nullptr);
    }

    if (hasInputColorSpace_) {
        videoContext_->VideoProcessorSetStreamColorSpace(processor_.Get(), 0, &inputColorSpace_);
    }
    if (hasOutputColorSpace_) {
        videoContext_->VideoProcessorSetOutputColorSpace(processor_.Get(), &outputColorSpace_);
    }

    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.OutputIndex = 0;
    stream.InputFrameOrField = 0;
    stream.PastFrames = 0;
    stream.FutureFrames = 0;
    stream.ppPastSurfaces = nullptr;
    stream.pInputSurface = inputView_.Get();
    stream.ppFutureSurfaces = nullptr;
    stream.ppPastSurfacesRight = nullptr;
    stream.ppFutureSurfacesRight = nullptr;

    const Slot& slot = slots_[static_cast<size_t>(slotIndex)];
    lastHr_ = videoContext_->VideoProcessorBlt(processor_.Get(), slot.view.Get(), 0, 1, &stream);

    // ID3D11VideoContext 没有 Flush——要推命令只能推设备上下文那一个。
    context_->Flush();

    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        return false;
    }

    errorKind_ = "ok";
    return true;
}

void Nv12Converter::SetSourceRect(const RECT& rect) {
    sourceRect_ = rect;
    hasSourceRect_ = true;
}

void Nv12Converter::ClearSourceRect() {
    hasSourceRect_ = false;
}

void Nv12Converter::SetInputColorSpace(const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& colorSpace) {
    inputColorSpace_ = colorSpace;
    hasInputColorSpace_ = true;
}

void Nv12Converter::SetOutputColorSpace(const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& colorSpace) {
    outputColorSpace_ = colorSpace;
    hasOutputColorSpace_ = true;
}

void Nv12Converter::UseDefaultColorSpaces() {
    hasInputColorSpace_ = false;
    hasOutputColorSpace_ = false;
}

bool Nv12Converter::SupportsOutputFormat(UINT* supportFlags) const {
    // 这个查询挂在 enumerator 上（先要有指定尺寸的 enumerator 才问得出格式）。
    if (!enumerator_ || supportFlags == nullptr) {
        return false;
    }
    return SUCCEEDED(enumerator_->CheckVideoProcessorFormat(DXGI_FORMAT_NV12, supportFlags));
}

bool Nv12Converter::ReadBack(ID3D11Texture2D* nv12, std::vector<BYTE>& luma,
                             std::vector<BYTE>& chroma, int heightPx, UINT* lumaPitch,
                             UINT* chromaPitch) {
    if (nv12 == nullptr || heightPx <= 0) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    D3D11_TEXTURE2D_DESC sourceDesc{};
    nv12->GetDesc(&sourceDesc);

    // NV12 能不能直接建成 staging，是这一趟要回答的问题之一：
    // 不行就退回「再走一次视频处理器转成 BGRA 回读」，那条路有第二次量化误差。
    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
    lastHr_ = device_->CreateTexture2D(&stagingDesc, nullptr, &staging);
    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        return false;
    }

    context_->CopyResource(staging.Get(), nv12);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    lastHr_ = context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(lastHr_)) {
        errorKind_ = "hr";
        return false;
    }

    const BYTE* base = static_cast<const BYTE*>(mapped.pData);
    const UINT pitch = static_cast<UINT>(mapped.RowPitch);
    luma.assign(base, base + static_cast<size_t>(pitch) * heightPx);

    // 平面格式映射出来的是一块连续内存：色度面紧接着亮度面，行距与亮度面一致。
    const BYTE* chromaStart = base + static_cast<size_t>(pitch) * heightPx;
    chroma.assign(chromaStart, chromaStart + static_cast<size_t>(pitch) * (heightPx / 2));

    context_->Unmap(staging.Get(), 0);

    *lumaPitch = pitch;
    *chromaPitch = pitch;
    errorKind_ = "ok";
    return true;
}

}
