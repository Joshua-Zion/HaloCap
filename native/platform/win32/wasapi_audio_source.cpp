#include "wasapi_audio_source.h"

#include <functiondiscoverykeys_devpkey.h>

#include <cstdio>

namespace halocap::platform::win32 {

namespace {

constexpr REFERENCE_TIME kHnsPerUs = 10;

// 子类型 GUID 从 ksmedia.h 抄成字面值，不用 KSDATAFORMAT_SUBTYPE_* 宏：
// 那一组是 DEFINE_GUIDSTRUCT/DEFINE_GUIDNAMED 形式，要么拉 ksuser.lib、要么定义
// STATIC_KSDATAFORMAT_SUBTYPE_* 才内联展开，为一个比较去加依赖不值得。
// 值取自同一份头文件（PCM=00000001-…、IEEE_FLOAT=00000003-…，后 8 字节都是 8000-00aa00389b71）。
constexpr GUID kSubtypePcm = { 0x00000001, 0x0000, 0x0010,
                               { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
constexpr GUID kSubtypeIeeeFloat = { 0x00000003, 0x0000, 0x0010,
                                     { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

const char* KindFor(HRESULT hr) {
    if (SUCCEEDED(hr)) {
        return "ok";
    }
    if (hr == E_INVALIDARG || hr == E_POINTER) {
        return "invalid";
    }
    if (hr == E_ACCESSDENIED || hr == static_cast<HRESULT>(0x80070005L)) {
        return "denied";
    }
    // 没有默认端点：GetDefaultAudioEndpoint 的「找不到」是 E_NOTFOUND。
    if (hr == static_cast<HRESULT>(0x80070490L)) {
        return "noendpoint";
    }
    if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == AUDCLNT_E_INVALID_SIZE ||
        hr == AUDCLNT_E_WRONG_ENDPOINT_TYPE) {
        return "format";
    }
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        return "disconnected";
    }
    if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED || hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
        hr == AUDCLNT_E_DEVICE_IN_USE) {
        return "inuse";
    }
    if (hr == REGDB_E_CLASSNOTREG || hr == CO_E_NOTINITIALIZED || hr == E_NOINTERFACE) {
        return "notfound";
    }
    return "hr";
}

std::string ToUtf8(const wchar_t* wide) {
    if (wide == nullptr || *wide == 0) {
        return {};
    }
    const int count = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(count) - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), count, nullptr, nullptr);
    return out;
}

std::string GuidToUtf8(const GUID& guid) {
    wchar_t text[64] = L"";
    StringFromGUID2(guid, text, 64);
    return ToUtf8(text);
}

}

WasapiAudioSource::WasapiAudioSource(core::AudioSourceKind kind) : kind_(kind) {}

WasapiAudioSource::~WasapiAudioSource() { Close(); }

const WasapiAudioSource::TimingCase* WasapiAudioSource::TimingCases(int* count) {
    static constexpr TimingCase kCases[] = {
        { 0, 0, "buffer=0 periodicity=0" },
        { 0, 100000, "buffer=0 periodicity=10ms" },
        { 1000000, 0, "buffer=100ms periodicity=0" },
        { 1000000, 100000, "buffer=100ms periodicity=10ms" },
    };

    *count = static_cast<int>(sizeof(kCases) / sizeof(kCases[0]));
    return kCases;
}

bool WasapiAudioSource::ActivateDefaultEndpoint() {
    // 音频线程要 MTA；已经有别的套间模型时 CoInitializeEx 返回 RPC_E_CHANGED_MODE，
    // 那种情况下 COM 照样可用，不当失败。
    const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE && comHr != S_FALSE) {
        lastHr_ = comHr;
        errorKind_ = KindFor(comHr);
        return false;
    }

    Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
    lastHr_ = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(enumerator.GetAddressOf()));
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    const EDataFlow flow = kind_ == core::AudioSourceKind::SystemLoopback ? eRender : eCapture;

    Microsoft::WRL::ComPtr<IMMDevice> device;
    lastHr_ = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    // Activate 的 iid 已经在第一个参数里了，输出只能是 void**——这里不能再套一层
    // IID_PPV_ARGS（那会把 REFIID 与 void** 两个参数一起带进去，变成 5 个参数）。
    Microsoft::WRL::ComPtr<IAudioClient> client;
    lastHr_ = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    device_ = device;
    client_ = client;

    Microsoft::WRL::ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(device_->OpenPropertyStore(STGM_READ, &properties)) && properties) {
        PROPVARIANT value{};
        PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR) {
            friendlyNameUtf8_ = ToUtf8(value.pwszVal);
        }
        PropVariantClear(&value);
    }

    return true;
}

bool WasapiAudioSource::NegotiateFormat() {
    lastHr_ = client_->GetMixFormat(&mixFormat_);
    if (FAILED(lastHr_) || mixFormat_ == nullptr) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    format_.sampleRateHz = static_cast<std::int32_t>(mixFormat_->nSamplesPerSec);
    format_.channelCount = static_cast<std::int32_t>(mixFormat_->nChannels);

    subtypeUtf8_.clear();

    if (mixFormat_->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mixFormat_->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(mixFormat_);
        subtypeUtf8_ = GuidToUtf8(extensible->SubFormat);
        format_.floatSamples = extensible->SubFormat == kSubtypeIeeeFloat;
    } else {
        format_.floatSamples = mixFormat_->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        subtypeUtf8_ = GuidToUtf8(format_.floatSamples ? kSubtypeIeeeFloat : kSubtypePcm);
    }

    // 共享模式按理一定给浮点；真给了别的形状就宁可失败，也不要在 seam 里偷偷转成浮点——
    // 那会把「这台机器的混合格式是什么」这个问题藏起来。
    if (!format_.floatSamples) {
        errorKind_ = "format";
        return false;
    }

    return true;
}

bool WasapiAudioSource::Open() {
    // 钉在第 0 组：Initialize 全填 0，让系统自己定共享模式的缓冲——本机四种组合都试过，
    // 结论见 tools/audio_probe 的输出与 §5.1。
    int count = 0;
    const TimingCase* cases = TimingCases(&count);
    return OpenWithTimings(cases[0].bufferHns, cases[0].periodicityHns);
}

bool WasapiAudioSource::OpenWithTimings(std::int64_t bufferHns, std::int64_t periodicityHns) {
    Close();

    if (!ActivateDefaultEndpoint()) {
        return false;
    }

    if (!NegotiateFormat()) {
        return false;
    }

    REFERENCE_TIME defaultPeriodHns = 0;
    REFERENCE_TIME minimumPeriodHns = 0;
    if (FAILED(client_->GetDevicePeriod(&defaultPeriodHns, &minimumPeriodHns))) {
        defaultPeriodHns = 0;
        minimumPeriodHns = 0;
    }
    defaultPeriodUs_ = defaultPeriodHns / kHnsPerUs;
    minimumPeriodUs_ = minimumPeriodHns / kHnsPerUs;

    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (kind_ == core::AudioSourceKind::SystemLoopback) {
        streamFlags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    }

    lastHr_ = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags,
                                  static_cast<REFERENCE_TIME>(bufferHns),
                                  static_cast<REFERENCE_TIME>(periodicityHns), mixFormat_, nullptr);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    event_ = CreateEventExW(nullptr, nullptr, 0, EVENT_MODIFY_STATE | SYNCHRONIZE);
    if (event_ == nullptr) {
        lastHr_ = HRESULT_FROM_WIN32(GetLastError());
        errorKind_ = "hr";
        return false;
    }

    lastHr_ = client_->SetEventHandle(static_cast<HANDLE>(event_));
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = client_->GetService(IID_PPV_ARGS(capture_.GetAddressOf()));
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = client_->GetBufferSize(&bufferFrames_);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    lastHr_ = client_->Start();
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    errorKind_ = "ok";
    return true;
}

void WasapiAudioSource::Close() {
    if (client_) {
        // 停机失败不改变关闭的结果：设备可能已经被拔掉了。
        client_->Stop();
    }

    capture_.Reset();
    client_.Reset();
    device_.Reset();

    if (event_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(event_));
        event_ = nullptr;
    }

    if (mixFormat_ != nullptr) {
        CoTaskMemFree(mixFormat_);
        mixFormat_ = nullptr;
    }

    packetHeld_ = false;
    heldData_ = nullptr;
    heldFrames_ = 0;
    bufferFrames_ = 0;
    format_ = core::AudioFormat{};
    friendlyNameUtf8_.clear();
    subtypeUtf8_.clear();
    errorKind_ = "none";
}

bool WasapiAudioSource::WaitForPacket(std::int64_t timeoutMs, core::AudioChunk& out) {
    out = core::AudioChunk{};

    if (!capture_ || event_ == nullptr) {
        lastHr_ = AUDCLNT_E_NOT_INITIALIZED;
        errorKind_ = "invalid";
        return false;
    }

    // 上一包还没还就想再等：这是调用方的错，报 invalid 而不是静默地把它挤掉。
    if (packetHeld_) {
        lastHr_ = E_INVALIDARG;
        errorKind_ = "invalid";
        return false;
    }

    const DWORD wait = timeoutMs <= 0 ? 0 : static_cast<DWORD>(timeoutMs);
    if (WaitForSingleObject(static_cast<HANDLE>(event_), wait) == WAIT_TIMEOUT) {
        // 静音时回环不出包，这条是常态：报出来但不算错误。
        errorKind_ = "timeout";
        return false;
    }

    UINT32 packetFrames = 0;
    lastHr_ = capture_->GetNextPacketSize(&packetFrames);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }
    if (packetFrames == 0) {
        errorKind_ = "timeout";
        return false;
    }

    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    UINT64 devicePosition = 0;
    UINT64 qpcPosition = 0;

    lastHr_ = capture_->GetBuffer(&data, &frames, &flags, &devicePosition, &qpcPosition);
    if (FAILED(lastHr_)) {
        errorKind_ = KindFor(lastHr_);
        return false;
    }

    heldData_ = data;
    heldFrames_ = frames;
    packetHeld_ = true;
    lastPacketFlags_ = static_cast<std::int64_t>(flags);

    out.samples = reinterpret_cast<const float*>(data);
    out.frameCount = static_cast<std::int32_t>(frames);
    out.devicePositionFrames = static_cast<std::int64_t>(devicePosition);
    // GetBuffer 给的 QPC 位置是 100 纳秒一格；它到底是不是这次取包的时刻，探针会对比。
    out.timestampUs = static_cast<std::int64_t>(qpcPosition) / kHnsPerUs;
    out.silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
    out.discontinuity = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;

    errorKind_ = "ok";
    return true;
}

void WasapiAudioSource::ReleasePacket() {
    if (!packetHeld_) {
        return;
    }

    // 采集侧的 ReleaseBuffer 只有一个参数（写侧才有 dwFlags）——两个接口别混着记。
    capture_->ReleaseBuffer(heldFrames_);

    packetHeld_ = false;
    heldData_ = nullptr;
    heldFrames_ = 0;
}

std::int64_t WasapiAudioSource::NextPacketFrames() {
    if (!capture_) {
        return -1;
    }

    UINT32 frames = 0;
    if (FAILED(capture_->GetNextPacketSize(&frames))) {
        return -1;
    }
    return static_cast<std::int64_t>(frames);
}

}
