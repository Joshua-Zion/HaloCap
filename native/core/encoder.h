#pragma once

#include "display_list.h"
#include "frame_scheduler.h"

#include <cstdint>
#include <string_view>

namespace halocap::core {

// 8 个 seam 里的 IEncoder（§3.2）：H.264 → MP4。
// 这一条管线的入口形状已被 spike 1 钉死：写入器 + DXGI 管理器 + NV12 纹理，
// 裸 MFT 走不通（两块硬件编码器都是 MF_E_TRANSFORM_ASYNC_LOCKED）。
struct VideoEncoderConfig {
    int widthPx = 0;
    int heightPx = 0;
    int targetFps = 30;

    // 非空 = 只编码源纹理上的这块区域（屏幕坐标，NV12 转换时裁剪）。
    IntRect cropRegion{};

    // 0 = 按 DefaultBitrateBitsPerSec 推。
    std::int64_t bitrateBitsPerSec = 0;

    // true = 请求硬件编码器。这个字段只是请求，不是结论，见 EncoderStats::hardwareRequested。
    bool requestHardware = true;

    // 采样时长用名义帧间隔，时间戳本身是真实的媒体时间（不做 CFR 重整）。
    std::int64_t FrameUs() const { return FrameIntervalUs(targetFps); }

    std::int64_t Bitrate() const;
};

// 一帧的交付方式。§3.3 规则二：core 只看得到句柄与尺寸，转换与上传都在平台侧。
// texture 指「要编码的那一帧」在渲染后端资源表里的句柄，也就是采集的常驻纹理（BGRA）；
// BGRA→NV12 由平台侧在 GPU 上做完再交给写入器——spike 1 实测 BGRA 样本会被一路接受、
// 到 Finalize 才失败并留下 0 字节文件，失败点离原因很远，所以转换不能省。
struct EncodedFrame {
    ImageHandle texture = 0;
    std::int64_t timeUs = 0;   // 必须是 RecordingClock 折算过的媒体时间
};

// 第二条流：AAC。配置必须在 OpenVideo 之前给出——两条流要一起 AddStream，
// 之后才能 SetInputMediaType 与 BeginWriting（sink writer 的时序，不是我们的选择）。
struct AudioEncoderConfig {
    int sampleRateHz = 48000;
    int channelCount = 2;              // 管线里一律是立体声（RemixToStereo 之后）
    std::int64_t bitrateBitsPerSec = 192000;
};

// 一块 PCM16 交织立体声（帧 = 采样点数 / 声道数）。1024 帧是 AAC 的块长。
struct AudioFrame {
    const std::int16_t* samples = nullptr;
    std::int32_t frameCount = 0;
    std::int64_t timeUs = 0;
};

// 统计口径与 1.x 对齐（§4 最后一条），两版的实测数字要能直接比。
struct EncoderStats {
    std::int64_t acceptedFrames = 0;    // ≈ 1.x 的 EncodedFrames
    std::int64_t rejectedFrames = 0;    // ≈ 1.x 的 DroppedFrames：写入器没收的都算丢
    std::int64_t outputBytes = 0;
    std::int64_t mediaTimeUs = 0;       // 最后一样本的呈现时间

    // 音频那一侧：写进第二条流的帧数与块数。
    std::int64_t audioFrames = 0;
    std::int64_t audioBlocks = 0;

    // 观测到的「这一路真用了硬件编码器」。判据来自协商完成之后服务上的
    // MFT_ENUM_HARDWARE_VENDOR_ID_Attribute（本机是 "VEN_8086"），软件编码器没有这一位。
    // 注意时序：spike 1 在 AddStream 之后、SetInputMediaType 之前问 GetServiceForStream，
    // 那时服务还不存在（MF_E_UNSUPPORTED_SERVICE），才得出「问不出来」的结论。
    bool hardwareEncoded = false;

    // 请求的是不是硬件——与上面那行分开，是为了把「请求」与「观测」区分清楚。
    bool hardwareRequested = false;
};

class IEncoder {
public:
    virtual ~IEncoder() = default;

    // outputPathUtf8 用 UTF-8：core 不出现宽字符语义，平台侧自己转。
    // 文件已经存在会被覆盖（录制目标是单个 mp4，续录不在这条管线上）。
    virtual bool OpenVideo(const VideoEncoderConfig& config, std::string_view outputPathUtf8) = 0;

    // 返回 false 表示这一帧没进编码器，计入 Stats().rejectedFrames；时间戳倒退属于调用方错误。
    virtual bool Submit(const EncodedFrame& frame) = 0;

    // MP4 的 moov 在收尾时才写完，不 Close 就没有有效文件。
    virtual bool Close(EncoderStats& outStats) = 0;

    // 第二条流（AAC）。默认实现返回 false：只做视频的实现（以及单测里的假编码器）
    // 不需要为了编译而实现音频；录制会话在配置失败时降级成纯视频。
    virtual bool ConfigureAudio(const AudioEncoderConfig& config) {
        (void)config;
        return false;
    }

    virtual bool SubmitAudio(const AudioFrame& frame) {
        (void)frame;
        return false;
    }

    // 平台侧实际会返回的几种："ok" / "invalid" / "mf"（0xC00D 那一段，编码器说不）/
    // "device"（设备没了，录制要整段重来）/ "hr"（其余，多半是我们自己调用写错）/ "none"。
    // 把 MF 与 E_/DXGI 分开，是因为前者可以按档位降级处理，后者必须当 bug 报。
    virtual const char* LastErrorKind() const = 0;

    // HRESULT 的数值，用整型带出来，免得 core 认识平台类型。
    virtual std::int32_t LastErrorCode() const = 0;
};

// ---- 以下都是纯逻辑，可测，不碰平台 ----

// MF 的样本时间单位是 100ns。
std::int64_t ToReferenceTime100ns(std::int64_t microUs);

// 码率默认：0.11 bpp × 帧率，夹在 [1 Mbps, 45 Mbps]。系数来自 spike 1 用的档位，
// 上下限是为了让小选区不至于压成噪声、4K60 不失控，改动要靠实测而不是拍脑袋。
std::int64_t DefaultBitrateBitsPerSec(int widthPx, int heightPx, int fps);

// 录制时钟：暂停必须冻结媒体时间（沿用 v1.6.0 的口径——暂停后时间轴不能出现空洞）。
// 时钟由调用方注入，所以这块逻辑完全可测。
class RecordingClock {
public:
    void Start(std::int64_t wallUs);
    void Pause(std::int64_t wallUs);
    void Resume(std::int64_t wallUs);

    // 未 Start 时为 0；暂停期间不前进。
    std::int64_t MediaTimeUs(std::int64_t wallUs) const;

    bool Running() const { return started_; }
    bool Paused() const { return pauseBeganWallUs_ >= 0; }

private:
    std::int64_t startWallUs_ = 0;
    std::int64_t pausedTotalUs_ = 0;
    std::int64_t pauseBeganWallUs_ = -1;
    bool started_ = false;
};

// 帧闸门：决定一帧该不该送进编码器。两条规则各自对应一个真实故障：
//   · 时间戳倒退 → 样本必须严格不递减，否则时间轴错乱；
//   · 送得比目标帧率快 → 桌面复制的唤醒率由光标主导（spike 2 实测 100+ 次每秒），
//     不设闸门就等于让编码器替我们做没有意义的重复工作。
//
// 这是「严格最小间隔」版本：距上次接受不足一个帧间隔就拒。它的问题是节拍由采集节奏决定——
// 采集每 10 ms 一帧、目标 30 fps（33.3 ms）时会稳定地接受在 40 ms 上（23.7 fps，S2 实测）。
// 录制会话因此改用下面的 FramePacer；FrameGate 留给「只要不超速」的测量工具。
class FrameGate {
public:
    enum class Decision {
        Submit,
        TooEarly,
        NonMonotonic,
    };

    explicit FrameGate(int targetFps);

    Decision Decide(std::int64_t mediaTimeUs);

    std::int64_t IntervalUs() const { return intervalUs_; }
    std::int64_t AcceptedCount() const { return accepted_; }
    std::int64_t DroppedCount() const { return dropped_; }

private:
    std::int64_t intervalUs_ = 0;
    std::int64_t lastAcceptedUs_ = -1;
    std::int64_t accepted_ = 0;
    std::int64_t dropped_ = 0;
};

// 按「目标网格」取样：把媒体时间四舍五入到第几个帧间隔，只有落到新的格子上才接受。
// 与 FrameGate 的差别在效果上：
//   · 采集每 10 ms 一帧 → 接受在 20/50/90/120 ms …，长期收敛到 30 fps，而不是钉在 40 ms；
//   · 采集每 16.7 ms 一帧（60 fps 内容）→ 接受在 16.7/50/83.3 ms，正好 30 fps；
//   · 采集比目标慢（每 100 ms 一帧）→ 每一帧都接受（内容就这么多，不能凭空变快）。
// 网格是绝对时间上的，所以长时间跑不会累积漂移；倒退的时间戳一律拒。
class FramePacer {
public:
    enum class Decision {
        Submit,
        TooEarly,
        NonMonotonic,
    };

    explicit FramePacer(int targetFps);

    Decision Decide(std::int64_t mediaTimeUs);

    std::int64_t IntervalUs() const { return intervalUs_; }
    std::int64_t AcceptedCount() const { return accepted_; }
    std::int64_t SkippedCount() const { return skipped_; }

private:
    std::int64_t intervalUs_ = 0;
    std::int64_t lastTick_ = -1;
    std::int64_t lastAcceptedUs_ = -1;
    std::int64_t accepted_ = 0;
    std::int64_t skipped_ = 0;
};

}
