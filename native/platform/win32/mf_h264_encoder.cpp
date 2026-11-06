#include "mf_h264_encoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mftransform.h>
#include <windows.h>

#include <cstdio>

namespace halocap::platform::win32 {

namespace {

// spike 1 的「属性变体」问题的正解：不猜编码器要什么，按代价从低到高试。
// 0 = 写全（stride + sampleSize），1 = 只写 stride，2 = 两个都不写。
constexpr int kVariantFull = 0;
constexpr int kVariantStrideOnly = 1;
constexpr int kVariantBare = 2;

constexpr int kVariantCount = 3;

ULONGLONG Pack2(ULONGLONG high, ULONGLONG low) { return (high << 32) | low; }

// MF 的HRESULT 都在 0xC00D 那一段，与标准 COM/E_ 码分开报，调用方才能区分
// 「编码器说不」（可预期）与「我们调用写错了」（要修）。
const char* KindFor(HRESULT hr) {
    if (SUCCEEDED(hr)) {
        return "ok";
    }
    if (hr == E_INVALIDARG) {
        return "invalid";
    }
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
        hr == DXGI_ERROR_DEVICE_HUNG) {
        return "device";
    }
    if ((static_cast<ULONG>(hr) & 0xFFFF0000UL) == 0xC00D0000UL) {
        return "mf";
    }
    return "hr";
}

std::int64_t FileBytes(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) == 0) {
        return -1;
    }
    return (static_cast<std::int64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

}

bool MfH264Encoder::StartupFoundation() {
    // COM 先起来；已经是别的线程模型时 CoInitializeEx 会返回 RPC_E_CHANGED_MODE，
    // 那种情况下 MF 照常用现有模型，不该把它当失败。
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE && comHr != S_FALSE) {
        return false;
    }
    return SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
}

void MfH264Encoder::ShutdownFoundation() {
    MFShutdown();
    CoUninitialize();
}

MfH264Encoder::MfH264Encoder(D2dRenderBackend& backend) : backend_(backend) {}

MfH264Encoder::~MfH264Encoder() {
    writer_.Reset();
}

bool MfH264Encoder::Initialize() {
    ID3D11Device* device = backend_.Device();
    ID3D11DeviceContext* context = backend_.D3dContext();
    if (device == nullptr || context == nullptr) {
        lastHr_ = E_FAIL;
        errorKind_ = "hr";
        return false;
    }

    // 这一步就是 §4 说的配对：注册进来的设备决定写入器选哪一家的编码器。
    lastHr_ = MFCreateDXGIDeviceManager(&resetToken_, &deviceManager_);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = deviceManager_->ResetDevice(device, resetToken_);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        deviceManager_.Reset();
        return false;
    }

    if (!converter_.Initialize(device, context)) {
        lastHr_ = converter_.LastHr();
        errorKind_ = converter_.LastErrorKind();
        return false;
    }

    errorKind_ = "ok";
    return true;
}

bool MfH264Encoder::BuildOutputType(IMFMediaType** outType) {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return false;
    }

    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_INTERLACE_MODE, 2 /*MFVideoInterlace_Progressive*/);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT64(MF_MT_FRAME_SIZE,
                             Pack2(static_cast<ULONGLONG>(config_.widthPx),
                                   static_cast<ULONGLONG>(config_.heightPx)));
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT64(MF_MT_FRAME_RATE,
                             Pack2(static_cast<ULONGLONG>(config_.targetFps), 1));
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_MPEG2_PROFILE, 77 /*AM_MPEG2Profile_High*/);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_AVG_BITRATE,
                             static_cast<UINT32>(config_.Bitrate() / 1000));
    }

    if (FAILED(hr)) {
        lastHr_ = hr;
        errorKind_ = KindFor(hr);
        return false;
    }

    *outType = type.Detach();
    return true;
}

bool MfH264Encoder::BuildInputType(int variant, IMFMediaType** outType) {
    Microsoft::WRL::ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(&type);
    if (FAILED(hr)) {
        return false;
    }

    hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) {
        hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT32(MF_MT_INTERLACE_MODE, 2 /*Progressive*/);
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT64(MF_MT_FRAME_SIZE,
                             Pack2(static_cast<ULONGLONG>(config_.widthPx),
                                   static_cast<ULONGLONG>(config_.heightPx)));
    }
    if (SUCCEEDED(hr)) {
        hr = type->SetUINT64(MF_MT_FRAME_RATE,
                             Pack2(static_cast<ULONGLONG>(config_.targetFps), 1));
    }

    if (variant == kVariantFull || variant == kVariantStrideOnly) {
        hr = type->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(config_.widthPx));
    }
    if (variant == kVariantFull) {
        hr = type->SetUINT32(MF_MT_SAMPLE_SIZE,
                             static_cast<UINT32>(config_.widthPx * config_.heightPx * 3 / 2));
    }

    if (FAILED(hr)) {
        lastHr_ = hr;
        errorKind_ = KindFor(hr);
        return false;
    }

    *outType = type.Detach();
    return true;
}

std::wstring MfH264Encoder::ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), count);
    return wide;
}

bool MfH264Encoder::OpenVideo(const core::VideoEncoderConfig& config,
                              std::string_view outputPathUtf8) {
    inputTypeVariant_ = -1;

    // NV12 是 4:2:0 的两个平面，奇数尺寸没有合法的色度平面划分。
    // 这条约束会一路传染到选区：录制会话必须先把手动框出来的尺寸取整到偶数。
    if (config.widthPx <= 0 || config.heightPx <= 0 || (config.widthPx & 1) != 0 ||
        (config.heightPx & 1) != 0 || config.targetFps <= 0) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    if (!deviceManager_) {
        lastHr_ = MF_E_INVALIDREQUEST;
        errorKind_ = "invalid";
        return false;
    }

    if (writing_) {
        writer_->Finalize();
        writer_.Reset();
        writing_ = false;
    }

    config_ = config;
    lastPathUtf8_ = std::string(outputPathUtf8);
    vendor_.clear();
    hardwareObserved_ = false;
    accepted_ = 0;
    rejected_ = 0;
    lastTimeUs_ = -1;

    if (!converter_.EnsureTargets(config.widthPx, config.heightPx)) {
        lastHr_ = converter_.LastHr();
        errorKind_ = converter_.LastErrorKind();
        return false;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    lastHr_ = MFCreateAttributes(&attributes, 4);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                                    config.requestHardware ? 1 : 0);
    if (SUCCEEDED(lastHr_)) {
        // 节流默认留着：关掉之后写入器不再按时间轴等帧，用来量裸吞吐很合适，
        // 但比实时更快地推帧会让写入器自己把没编完的帧攒在内存里（§5.1 有数字）。
        lastHr_ = attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING,
                                        throttleDisabled_ ? 1 : 0);
    }
    if (SUCCEEDED(lastHr_)) {
        lastHr_ = attributes->SetUnknown(MF_SINK_WRITER_D3D_MANAGER, deviceManager_.Get());
    }
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    const std::wstring path = ToWide(outputPathUtf8);
    DeleteFileW(path.c_str());

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer;
    lastHr_ = MFCreateSinkWriterFromURL(path.c_str(), nullptr, attributes.Get(), &writer);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaType> outputType;
    if (!BuildOutputType(&outputType)) {
        return false;
    }

    lastHr_ = writer->AddStream(outputType.Get(), &streamIndex_);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    // 输入类型要写哪些属性，编码器各家不一样，所以留了退让顺序，第一个被接受的就是答案。
    for (int variant = 0; variant < kVariantCount; ++variant) {
        Microsoft::WRL::ComPtr<IMFMediaType> inputType;
        if (!BuildInputType(variant, &inputType)) {
            return false;
        }

        const HRESULT setHr = writer->SetInputMediaType(streamIndex_, inputType.Get(), nullptr);
        if (SUCCEEDED(setHr)) {
            inputTypeVariant_ = variant;
            lastHr_ = S_OK;
            break;
        }
        lastHr_ = setHr;
    }

    if (inputTypeVariant_ < 0) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = writer->BeginWriting();
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    writer_ = writer;
    writing_ = true;
    errorKind_ = "ok";
    return true;
}

bool MfH264Encoder::Submit(const core::EncodedFrame& frame) {
    if (!writing_ || !writer_) {
        lastHr_ = MF_E_INVALIDREQUEST;
        errorKind_ = "invalid";
        ++rejected_;
        return false;
    }

    if (frame.timeUs < lastTimeUs_) {
        // 时间轴倒退会直接写坏样本表，宁可丢帧。
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        ++rejected_;
        return false;
    }

    ID3D11Texture2D* source = nullptr;
    if (!backend_.TextureFor(frame.texture, &source) || source == nullptr) {
        lastHr_ = backend_.LastHr() != S_OK ? backend_.LastHr() : E_FAIL;
        errorKind_ = KindFor(lastHr_);
        ++rejected_;
        return false;
    }

    const int slot = converter_.AcquireSlot();
    if (slot < 0 || !converter_.ConvertToSlot(source, slot)) {
        lastHr_ = converter_.LastHr();
        errorKind_ = converter_.LastErrorKind();
        ++rejected_;
        return false;
    }

    Microsoft::WRL::ComPtr<IMFMediaBuffer> buffer;
    lastHr_ = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D),
                                        converter_.SlotTexture(slot), 0, FALSE, &buffer);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        ++rejected_;
        return false;
    }

    // 这一步不能省：DXGI surface 缓冲的 GetCurrentLength() 恒为 0，
    // 不写长度就是每帧 E_INVALIDARG，最后 MF_E_SINK_NO_SAMPLES_PROCESSED。
    lastHr_ = buffer->SetCurrentLength(
        static_cast<DWORD>(config_.widthPx * config_.heightPx * 3 / 2));
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        ++rejected_;
        return false;
    }

    Microsoft::WRL::ComPtr<IMFSample> sample;
    lastHr_ = MFCreateSample(&sample);
    if (SUCCEEDED(lastHr_)) {
        lastHr_ = sample->AddBuffer(buffer.Get());
    }
    if (SUCCEEDED(lastHr_)) {
        lastHr_ = sample->SetSampleTime(core::ToReferenceTime100ns(frame.timeUs));
    }
    if (SUCCEEDED(lastHr_)) {
        lastHr_ = sample->SetSampleDuration(core::ToReferenceTime100ns(config_.FrameUs()));
    }

    if (SUCCEEDED(lastHr_)) {
        lastHr_ = writer_->WriteSample(streamIndex_, sample.Get());
    }

    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        ++rejected_;
        return false;
    }

    ++accepted_;
    lastTimeUs_ = frame.timeUs;
    errorKind_ = "ok";
    return true;
}

bool MfH264Encoder::Close(core::EncoderStats& outStats) {
    outStats = core::EncoderStats{};

    if (!writing_ || !writer_) {
        lastHr_ = MF_E_INVALIDREQUEST;
        errorKind_ = "invalid";
        return false;
    }

    // 观测必须在 Finalize 之前：收尾之后服务就还回去了，读不到任何属性。
    ObserveChosenEncoder();

    lastHr_ = writer_->Finalize();
    const bool ok = SUCCEEDED(lastHr_);
    errorKind_ = KindFor(lastHr_);

    writing_ = false;
    writer_.Reset();

    outStats.acceptedFrames = accepted_;
    outStats.rejectedFrames = rejected_;
    outStats.mediaTimeUs = lastTimeUs_ < 0 ? 0 : lastTimeUs_;
    outStats.hardwareRequested = config_.requestHardware;
    outStats.hardwareEncoded = hardwareObserved_;
    outStats.outputBytes = ok ? FileBytes(ToWide(lastPathUtf8_)) : -1;

    return ok;
}

void MfH264Encoder::ObserveChosenEncoder() {
    vendor_.clear();
    hardwareObserved_ = false;

    Microsoft::WRL::ComPtr<IMFTransform> transform;
    if (FAILED(writer_->GetServiceForStream(streamIndex_, GUID_NULL,
                                            IID_PPV_ARGS(transform.GetAddressOf()))) ||
        !transform) {
        return;
    }

    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (FAILED(transform->GetAttributes(&attributes)) || !attributes) {
        return;
    }

    // 判据就这一位：硬件编码器带 PCI 厂商标识（本机 "VEN_8086"），软件编码器没有。
    LPWSTR buffer = nullptr;
    UINT32 length = 0;
    if (FAILED(attributes->GetAllocatedString(MFT_ENUM_HARDWARE_VENDOR_ID_Attribute, &buffer,
                                              &length)) ||
        buffer == nullptr) {
        return;
    }

    const int wideLength = static_cast<int>(length);
    const int need = WideCharToMultiByte(CP_UTF8, 0, buffer, wideLength, nullptr, 0, nullptr,
                                         nullptr);
    if (need > 0) {
        vendor_.resize(static_cast<size_t>(need));
        WideCharToMultiByte(CP_UTF8, 0, buffer, wideLength, vendor_.data(), need, nullptr, nullptr);
    }

    CoTaskMemFree(buffer);
    hardwareObserved_ = !vendor_.empty();
}

HRESULT MfH264Encoder::ProbeServiceQuery() {
    if (!writer_) {
        lastHr_ = MF_E_INVALIDREQUEST;
        errorKind_ = "invalid";
        return lastHr_;
    }

    Microsoft::WRL::ComPtr<IMFTransform> transform;
    lastHr_ = writer_->GetServiceForStream(streamIndex_, GUID_NULL, IID_PPV_ARGS(transform.GetAddressOf()));
    errorKind_ = KindFor(lastHr_);
    return lastHr_;
}

HRESULT MfH264Encoder::ChosenTransform(IMFTransform** outTransform) {
    if (!writer_ || outTransform == nullptr) {
        lastHr_ = MF_E_INVALIDREQUEST;
        errorKind_ = "invalid";
        return lastHr_;
    }

    Microsoft::WRL::ComPtr<IMFTransform> transform;
    lastHr_ = writer_->GetServiceForStream(streamIndex_, GUID_NULL, IID_PPV_ARGS(&transform));
    errorKind_ = KindFor(lastHr_);
    if (FAILED(lastHr_)) {
        return lastHr_;
    }

    *outTransform = transform.Detach();
    return lastHr_;
}

void MfH264Encoder::UseColorSpaces(bool withExplicitRanges) {
    if (!withExplicitRanges) {
        converter_.UseDefaultColorSpaces();
        return;
    }

    // 输入按全范围 RGB，输出按视频范围 16–235、BT.601。
    // 这一组对不对不靠文档判定，探针会读回 NV12 看黑色与白色的 Y 落在 0/255 还是 16/235。
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE input{};
    input.Usage = 0;
    input.RGB_Range = 1;
    input.YCbCr_Matrix = 0;
    input.YCbCr_xvYCC = 0;
    input.Nominal_Range = 2;   // 0–255

    D3D11_VIDEO_PROCESSOR_COLOR_SPACE output{};
    output.Usage = 0;
    output.RGB_Range = 0;
    output.YCbCr_Matrix = 0;   // BT.601
    output.YCbCr_xvYCC = 0;
    output.Nominal_Range = 1;  // 16–235

    converter_.SetInputColorSpace(input);
    converter_.SetOutputColorSpace(output);
}

}
