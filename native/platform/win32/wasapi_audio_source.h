#pragma once

#include "audio_source.h"

#include <windows.h>

#include <AudioSessionTypes.h>
#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <string>

namespace halocap::platform::win32 {

// IAudioSource 的 WASAPI 实现。三条只有实测才看得见的规矩：
//   1. 共享模式的混合格式一定是 32 位浮点，但**声道数由端点决定**（5.1 就是 6），
//      所以进录制管线之前一律先折成立体声；
//   2. 回环在参考设备静音时根本不出包，`WaitForPacket` 超时是正常分支不是错误，
//      录制侧必须按时间补静音（§5.1 音频 seam 有本机的数字）；
//   3. `GetBuffer` 给的指针是借的，`ReleaseBuffer` 之后就不归调用方了——seam 用
//      配对的 `ReleasePacket` 把这条钉住，没还就想再等直接判 invalid。
class WasapiAudioSource final : public core::IAudioSource {
public:
    explicit WasapiAudioSource(core::AudioSourceKind kind);

    ~WasapiAudioSource() override;

    // 用下面那组钉死的时序参数打开。REFERENCE_TIME 的单位是 100 纳秒。
    bool Open() override;

    struct TimingCase {
        long long bufferHns;
        long long periodicityHns;
        const char* name;
    };

    // Initialize 在共享模式下这两个参数该怎么填，文档说得含糊（一处说缓冲时长由系统定，
    // 一处说事件驱动时 periodicity 必须是 0）。所以这不是设计选择，是一张要跑的表：
    // 探针逐个试、把 HRESULT 与 GetBufferSize 打出来，Open() 用其中量出来能用的那一组。
    static const TimingCase* TimingCases(int* count);

    // 探针用来单独试某一种组合。
    bool OpenWithTimings(std::int64_t bufferHns, std::int64_t periodicityHns);

    void Close() override;

    core::AudioFormat Format() const override { return format_; }

    bool WaitForPacket(std::int64_t timeoutMs, core::AudioChunk& out) override;

    void ReleasePacket() override;

    const char* LastErrorKind() const override { return errorKind_; }
    std::int32_t LastErrorCode() const override { return static_cast<std::int32_t>(lastHr_); }

    std::string EndpointNameUtf8() const override { return friendlyNameUtf8_; }

    // ---- 只给探针用 ----

    UINT32 BufferFrames() const { return bufferFrames_; }
    std::int64_t DefaultDevicePeriodUs() const { return defaultPeriodUs_; }
    std::int64_t MinimumDevicePeriodUs() const { return minimumPeriodUs_; }

    // 队列里下一个包有多少帧（GetNextPacketSize）；静音时这个数一直是 0。
    std::int64_t NextPacketFrames();

    // 混合格式的子类型 GUID，转成字符串给探针打出来（浮点/16 位 PCM 就靠它分辨）。
    std::string SubtypeGuidUtf8() const { return subtypeUtf8_; }

    std::int64_t LastPacketFlags() const { return lastPacketFlags_; }

private:
    bool ActivateDefaultEndpoint();
    bool NegotiateFormat();

    core::AudioSourceKind kind_;

    Microsoft::WRL::ComPtr<IMMDevice> device_;
    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;

    void* event_ = nullptr;
    WAVEFORMATEX* mixFormat_ = nullptr;   // CoTaskMemFree 负责它

    core::AudioFormat format_{};
    std::string friendlyNameUtf8_;
    std::string subtypeUtf8_;

    UINT32 bufferFrames_ = 0;
    std::int64_t defaultPeriodUs_ = 0;
    std::int64_t minimumPeriodUs_ = 0;

    // 借走还没还的那一包。
    bool packetHeld_ = false;
    BYTE* heldData_ = nullptr;
    UINT32 heldFrames_ = 0;
    std::int64_t lastPacketFlags_ = 0;

    HRESULT lastHr_ = S_OK;
    const char* errorKind_ = "none";
};

}
