#pragma once

#include "audio_source.h"
#include "capture_source.h"
#include "encoder.h"

#include <cstdint>
#include <string>
#include <vector>

namespace halocap::core {

// S2 的第一块拼装：把 ICaptureSource 的帧、RecordingClock 的时间轴、FrameGate 的节流
// 与 IEncoder 的提交接成一个「录制会话」。
//
// 这一层故意只做编排：不碰 D3D、不碰 MF、不碰 WASAPI，所以它能用假 seam 全量单测——
// 而三个 seam 各自的真实行为已经在 capture_probe / encoder_probe / audio_probe 里量过。
// 这样 S2 的端到端数字来自「真 seam + 同一份编排」，不是探针自己重写一遍。
struct RecordingOptions {
    int outputIndex = 0;

    // 非空 = 采这个窗口（WGC）；否则按 outputIndex 采桌面复制。
    NativeHandle captureWindow = nullptr;

    // true = 用 WGC 采显示器（带光标）；false = 桌面复制。
    bool captureMonitor = false;

    // 非空 = 只录这块屏幕区域（桌面录制）；窗口录制忽略。
    IntRect cropRegion{};

    // 目标帧率。桌面复制的唤醒率由光标主导（实测 100+ 次每秒），所以这个数不是「采集多少帧」，
    // 而是「最多送多少帧」。
    int targetFps = 30;

    // 0 = 按 DefaultBitrateBitsPerSec 推（0.11 bpp × 帧率，夹在 [1, 45] Mbps）。
    std::int64_t bitrateBitsPerSec = 0;

    // 只是请求：实际用没用上硬件看 RecordingStats::hardwareEncoded（协商后读到的厂商位）。
    bool requestHardware = true;

    // 每次等帧等多久。等不到不是错误（静态桌面就是这样）。
    std::int64_t captureTimeoutMs = 20;

    // 静态桌面的保活间隔：画面不变化时也至少每这么久送一帧。0 = 关闭（只适合探针）。
    // 为什么必须有：编码器一帧没收时 Finalize 直接失败（编码 seam 记过「一帧没收」那条），
    // 静止画面录出来会是一个坏文件，而不是「一段静止的视频」。
    std::int64_t keepAliveUs = 1'000'000;

    // 音频：系统声音与麦克风都折成立体声、混进 AAC 第二条流。任何一路打不开都只降级成
    // 少一路（或纯视频），录制本身不失败——「没有声音」不该让一次录制作废。
    bool audioEnabled = false;
    std::int64_t audioBitrateBitsPerSec = 192000;
    float systemAudioGain = 1.0f;
    float microphoneGain = 1.0f;

    // 回环静音时按墙钟补静音；一次最多补这么多，免得一个长暂停后的第一次泵补出一个大洞。
    std::int64_t audioSilenceLimitUs = 500'000;

    std::string outputPathUtf8;
};

struct RecordingStats {
    // 采集侧
    std::int64_t wakeups = 0;          // WaitForUpdate 返回了帧的次数（含仅鼠标）
    std::int64_t contentFrames = 0;    // contentChanged 的帧，也就是真正的画面变化
    std::int64_t dirtyPixelSum = 0;    // 脏矩形面积累计（像素），用来对照「只搬变化区」

    // 闸门与编码器
    std::int64_t framesSubmitted = 0;
    std::int64_t framesGateDropped = 0;   // 太早或时间戳倒退
    std::int64_t framesRejected = 0;      // 编码器没收

    // 结果
    std::int64_t outputBytes = 0;
    std::int64_t mediaTimeUs = 0;   // 最后一样本的媒体时间
    std::int64_t wallTimeUs = 0;    // 录制墙钟时长（含暂停，不含启动前）
    bool hardwareRequested = false;
    bool hardwareEncoded = false;

    // 音频那一侧
    bool audioActive = false;
    std::int64_t audioPackets = 0;
    std::int64_t audioFrames = 0;          // 送进编码器的帧数（含补的静音）
    std::int64_t audioBlocks = 0;          // 1024 帧一块
    std::int64_t audioSilenceFrames = 0;   // 回环保活补的静音
    std::int64_t audioGapFrames = 0;       // 时间戳空洞补的静音
    std::int64_t audioLateFrames = 0;      // 设备时间戳落后、被丢掉的帧
    std::int64_t audioRejectedBlocks = 0;  // 编码器没收的音频块

    // 已提交帧数 / 应有帧数。静态桌面上它会明显小于 1（没有内容变化就应该是 0 帧），
    // 所以判「不丢帧」必须用有画面变化的样本，和 1.x 那条教训一致。
    double SubmitRatio(int targetFps) const;
};

// 一个会话只录一个输出、一个文件。「从哪块屏/哪个窗口来」由调用方打开 seam 时决定，
// 所以这里没有 WGC 与桌面复制的分支——那是平台侧的事。
class RecordingSession {
public:
    // 两个视频 seam 由调用方持有，生命周期必须覆盖会话（平台对象不能在这里销毁）。
    // 音频源可以为空：那就只录视频。两路音频都是可选的。
    RecordingSession(ICaptureSource& capture, IEncoder& encoder,
                     IAudioSource* systemAudio = nullptr, IAudioSource* microphone = nullptr);
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    // 打开采集、按采集尺寸开编码器、启动时钟。失败时把 seam 的错误原样带出来：
    // "busy"（复制权被占）/ "invalid"（尺寸奇数、路径为空）/ "encoder"（写入器没建起来）。
    bool Start(const RecordingOptions& options, std::int64_t wallUs);

    // 泵一次：等一帧 → 过闸门 → 送编码器。返回 false 表示会话必须结束（错误已记在
    // LastErrorKind 里，调用方据此决定退避重开还是给用户报错）。
    bool Pump(std::int64_t wallUs);

    // 暂停只冻结媒体时间，采集与编码器都不动（暂停期间不产生样本）。
    bool Pause(std::int64_t wallUs);
    bool Resume(std::int64_t wallUs);

    // 关采集、收尾编码器（MP4 的 moov 在这一步写完），把两份统计并起来。
    bool Stop(std::int64_t wallUs, RecordingStats& outStats);

    bool Recording() const { return recording_; }
    bool Paused() const { return clock_.Paused(); }
    const RecordingStats& Stats() const { return stats_; }
    const char* LastErrorKind() const { return errorKind_; }
    std::int32_t LastErrorCode() const { return lastErrorCode_; }
    const char* OutputPath() const { return path_.c_str(); }

    // 录制浮层的信息行与电平表（1.x 的 RecordingHudState 同源）：
    // 尺寸是编码器真正开的尺寸，电平是最近一次泵里两路各自的峰值。
    int OutputWidth() const { return outputWidth_; }
    int OutputHeight() const { return outputHeight_; }
    int TargetFps() const { return options_.targetFps; }
    bool SystemAudioActive() const { return audioConfigured_ && systemFormat_.sampleRateHz > 0; }
    bool MicrophoneActive() const { return audioConfigured_ && microphoneFormat_.sampleRateHz > 0; }
    float SystemAudioLevel() const { return systemLevel_; }
    float MicrophoneLevel() const { return microphoneLevel_; }

private:
    void SetError(const char* kind, std::int32_t code = 0);
    void MergeEncoderStats(const EncoderStats& encoderStats);
    void CloseOnDestroy();

    // 送一帧；返回 false 只表示「会话必须结束」（设备没了）。
    bool SubmitFrame(std::int64_t mediaUs, ImageHandle texture);

    // 视频与音频各泵一次。Pump 只负责顺序与状态。
    bool PumpVideo(std::int64_t wallUs);
    void PumpAudio(std::int64_t wallUs);

    // 从一路设备把包取空（非阻塞），折立体声、重采样、按时间戳混进待提交队列。
    // level 非空时把这一路的峰值电平写进去（浮层电平表用）。
    void DrainAudioSource(IAudioSource* source, const AudioFormat& format, float gain, float* level);
    void DiscardAudioPackets(IAudioSource* source);

    void EnsurePendingFrames(std::int64_t frames);
    void AppendSilenceFrames(std::int64_t frames);
    void FlushAudioBlocks(bool padLastBlock);
    void CloseAudio();

    std::int64_t AvailableAudioFrames() const;
    std::int64_t QueuedAudioUs() const;

    ICaptureSource& capture_;
    IEncoder& encoder_;
    IAudioSource* systemAudio_ = nullptr;
    IAudioSource* microphone_ = nullptr;

    RecordingOptions options_;
    RecordingClock clock_;
    // 按目标网格取样：严格最小间隔的 FrameGate 在采集节奏远快于目标帧率时会掉到 23.7 fps。
    FramePacer pacer_{ 30 };
    RecordingStats stats_;

    // 音频时间轴：audioPending_ 的第 0 帧对应「已提交帧数」这一刻，之后按时间戳拼接。
    bool audioConfigured_ = false;
    AudioFormat systemFormat_{};
    AudioFormat microphoneFormat_{};
    std::vector<float> audioPending_;      // 交织立体声
    std::vector<float> audioScratch_;      // 折立体声后的临时缓冲
    std::vector<float> audioResample_;     // 重采样后的临时缓冲
    std::vector<std::int16_t> audioPcm_;   // 送编码器的一块 PCM16
    std::int64_t audioFramesSubmitted_ = 0;
    std::int64_t audioReadFrames_ = 0;     // audioPending_ 里已经提交掉的帧数
    bool audioGotPacket_ = false;
    float systemLevel_ = 0.0f;
    float microphoneLevel_ = 0.0f;

    int outputWidth_ = 0;
    int outputHeight_ = 0;

    std::int64_t startWallUs_ = 0;
    std::string path_;

    bool recording_ = false;
    bool captureOpen_ = false;
    bool encoderOpen_ = false;

    const char* errorKind_ = "none";
    std::int32_t lastErrorCode_ = 0;
};

}
