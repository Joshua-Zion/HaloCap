#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace halocap::core {

// 8 个 seam 里的 IAudioSource（§3.2）：系统声音 + 麦克风，输出统一到 48 kHz 立体声。
// 平台侧只负责「把设备的那一串帧交出来」，静音补齐、混音、重采样、对齐 AAC 块全在下面
// 这些纯逻辑里——它们能单测，也是 v1.6.0 那条「两条流共用一个录制时钟」的延续。

enum class AudioSourceKind {
    SystemLoopback,   // 默认播放设备正在出什么就录什么
    Microphone,       // 默认录音设备
};

// 端点混合模式的真实形状。Windows 的共享模式一定是 32 位浮点，但声道数可能是 1、2、甚至
// 5.1 的 6——所以声道数是数据不是假设，进录制管线之前一律先折成立体声。
struct AudioFormat {
    std::int32_t sampleRateHz = 0;
    std::int32_t channelCount = 0;
    bool floatSamples = true;
};

// 一次交付的包。samples 是**借来的**：WASAPI 的 GetBuffer 给的是它自己那块环形缓冲里的
// 指针，下一次 ReleasePacket 之前有效，跨两次等待持有就会读到被覆写的旧数据。
// 这条约束写进 seam 的形状里（配对的 ReleasePacket），比写在注释里可靠。
struct AudioChunk {
    const float* samples = nullptr;
    std::int32_t frameCount = 0;

    // 端点自己的帧计数：只在同一台设备内单调，用来量漂移，不能当录制时钟用。
    std::int64_t devicePositionFrames = 0;
    std::int64_t timestampUs = 0;

    // silent：设备这段没内容（回环静音最常见的样子）。
    // discontinuity：设备丢了数据，时间轴要当成断开了处理。
    bool silent = false;
    bool discontinuity = false;
};

class IAudioSource {
public:
    virtual ~IAudioSource() = default;

    // 打开默认端点并建成事件驱动；失败时用 LastErrorKind 说是哪一类失败
    // （没有该端点、格式谈不拢、设备被独占、系统不让我捕获……都不是同一回事）。
    virtual bool Open() = 0;

    virtual void Close() = 0;

    virtual AudioFormat Format() const = 0;

    // 等一个包。timeoutMs 内没包返回 false——这一条**不是错误**：
    // 回环在桌面静音时根本不出包（见 §5.1 音频 seam），调用方要按时间补静音。
    virtual bool WaitForPacket(std::int64_t timeoutMs, AudioChunk& out) = 0;

    // 与 WaitForPacket 配对：还掉借来的缓冲，允许下一次等待。
    virtual void ReleasePacket() = 0;

    // "ok" / "timeout" / "noendpoint" / "notfound" / "format" / "inuse" / "denied" /
    // "hr" / "invalid" / "none"
    virtual const char* LastErrorKind() const = 0;
    virtual std::int32_t LastErrorCode() const = 0;

    // 诊断用：端点的友好名（UTF-8），设置页与环境摘要都要显示它。
    virtual std::string EndpointNameUtf8() const = 0;
};

// ---- 以下都是纯逻辑，可测，不碰平台 ----

// 定容量的交织立体声环形缓冲。§4 要求「队列深度写死、帧循环里不 new」，所以容量只在构造时
// 给一次；写满丢最旧的，并把丢掉的帧数记下来——录制里「慢了的音频」必须能被数出来。
class StereoRingBuffer {
public:
    explicit StereoRingBuffer(std::size_t capacityFrames);

    void Push(const float* interleavedStereo, std::int32_t frameCount);

    std::int32_t AvailableFrames() const;
    std::int32_t CapacityFrames() const { return static_cast<std::int32_t>(capacityFrames_); }

    // 取走最多 frameCount 帧，返回实际取到的（不足就少给，调用方自己决定补不补静音）。
    std::int32_t Pop(float* destination, std::int32_t frameCount);

    std::int64_t DroppedFrames() const { return droppedFrames_; }
    std::int64_t PushedFrames() const { return pushedFrames_; }

    void Reset();

private:
    std::size_t capacityFrames_ = 0;
    std::vector<float> samples_;   // 容量 × 2
    std::size_t head_ = 0;         // 下一次写的位置
    std::size_t used_ = 0;         // 当前有多少帧
    std::int64_t droppedFrames_ = 0;
    std::int64_t pushedFrames_ = 0;
};

// source 按 gain 加进 destination（都是交织立体声）。饱和到 ±1.0：
// 不饱和就会写出超过浮点范围的样本，AAC 那边表现为爆音而且没有规律。
void MixInto(float* destination, const float* source, std::int32_t frameCount, float gain);

// 任意声道数折成立体声：1 声道两路都用它，>=2 取前两路（5.1 的中置与环绕在这里丢掉，
// 与 1.x 的行为一致），0 声道什么都不做。
void RemixToStereo(float* destination, const float* source, std::int32_t frameCount,
                   std::int32_t sourceChannels);

// 线性重采样。ratio = 目标率 / 源率（比如 48000/44100）。这是端点混合模式不是 48 kHz 时的
// 退路；音质是近似，验收要靠听，这一条要写在产品文档里而不是藏在这里。
std::int32_t ResampledFrameCount(std::int32_t sourceFrames, double ratio);
void ResampleLinear(const float* source, std::int32_t sourceFrames, std::int32_t channels,
                    double ratio, float* destination, std::int32_t destinationFrames);

// AAC 一块 1024 帧：不足要补静音凑齐，凑不齐的那半块留到下一次（v1.6.0 的口径）。
constexpr std::int32_t kAacFrameCount = 1024;

std::int32_t SilenceFramesToNextBlock(std::int64_t totalFrames,
                                      std::int32_t blockSize = kAacFrameCount);

// 帧数 ↔ 微秒。录制时钟是微秒，音频的进度是帧，两边换算必须用同一个写法，
// 否则四舍五入会让两条流在长录制里慢慢错开。
std::int64_t FramesToUs(std::int64_t frames, std::int32_t sampleRateHz);
std::int64_t UsToFrames(std::int64_t us, std::int32_t sampleRateHz);

// 浮点交织样本 → 16 位 PCM（AAC 编码器的输入类型）。先夹到 ±1 再乘 32767：
// 超出范围的样本直接截断会听成爆音，而 16 位 PCM 的满幅是 32767（不是 32768，负数对称才不爆）。
void ConvertFloatToPcm16(const float* source, std::int32_t sampleCount, std::int16_t* destination);

}
