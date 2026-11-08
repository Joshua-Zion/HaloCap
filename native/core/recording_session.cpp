#include "recording_session.h"

#include <algorithm>
#include <string_view>

namespace halocap::core {

namespace {

constexpr std::int32_t kAudioSampleRate = 48000;

// 音频待提交队列的上限（3 秒）。超过它说明时间轴对不上（设备时间戳跳了），
// 这一包就丢掉并计入 late，而不是让内存一路涨。
constexpr std::int64_t kAudioMaxPendingFrames = kAudioSampleRate * 3;

bool IsKind(const char* kind, const char* wanted) {
    return kind != nullptr && std::string_view(kind) == wanted;
}

}  // namespace

double RecordingStats::SubmitRatio(int targetFps) const {
    if (targetFps <= 0 || wallTimeUs <= 0) {
        return 0.0;
    }
    const double expected = static_cast<double>(wallTimeUs) / 1'000'000.0 * targetFps;
    if (expected <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(framesSubmitted) / expected;
}

RecordingSession::RecordingSession(ICaptureSource& capture, IEncoder& encoder,
                                   IAudioSource* systemAudio, IAudioSource* microphone)
    : capture_(capture), encoder_(encoder), systemAudio_(systemAudio), microphone_(microphone) {}

RecordingSession::~RecordingSession() { CloseOnDestroy(); }

void RecordingSession::SetError(const char* kind, std::int32_t code) {
    errorKind_ = kind == nullptr ? "none" : kind;
    lastErrorCode_ = code;
}

bool RecordingSession::Start(const RecordingOptions& options, std::int64_t wallUs) {
    if (recording_) {
        SetError("state");
        return false;
    }

    options_ = options;
    stats_ = RecordingStats{};
    stats_.hardwareRequested = options.requestHardware;
    path_ = options.outputPathUtf8;
    audioConfigured_ = false;
    audioPending_.clear();
    audioReadFrames_ = 0;
    audioFramesSubmitted_ = 0;

    if (path_.empty()) {
        SetError("invalid");
        return false;
    }

    // 窗口录制走 WGC，显示器录制走桌面复制：同一个 seam，两个入口。
    const bool opened = options.captureWindow != nullptr
                            ? capture_.OpenWindow(options.captureWindow)
                            : capture_.Open(options.outputIndex);
    if (!opened) {
        const char* kind = capture_.LastErrorKind();
        // 复制权被占用（busy）是常态分支：调用方按 BackoffUs 重试，不是给用户报错。
        SetError(kind == nullptr ? "none" : kind);
        return false;
    }
    captureOpen_ = true;

    const IntRect bounds = capture_.DesktopBounds();
    int widthPx = bounds.right - bounds.left;
    int heightPx = bounds.bottom - bounds.top;
    IntRect localCrop{};
    if (!options.cropRegion.IsEmpty() && options.captureWindow == nullptr) {
        // 调用方给的是屏幕坐标；编码器要的是「源纹理内」的坐标。
        IntRect region = options.cropRegion;
        region.left = std::clamp(region.left - bounds.left, 0, widthPx);
        region.right = std::clamp(region.right - bounds.left, 0, widthPx);
        region.top = std::clamp(region.top - bounds.top, 0, heightPx);
        region.bottom = std::clamp(region.bottom - bounds.top, 0, heightPx);
        region.right = std::min(region.right, region.left + (widthPx - region.left));
        region.bottom = std::min(region.bottom, region.top + (heightPx - region.top));
        // NV12 要求偶数尺寸：向内取整。
        region.right = region.left + ((region.right - region.left) & ~1);
        region.bottom = region.top + ((region.bottom - region.top) & ~1);
        if (region.right - region.left >= 2 && region.bottom - region.top >= 2) {
            localCrop = region;
            widthPx = region.right - region.left;
            heightPx = region.bottom - region.top;
        }
    }

    // NV12 是 4:2:0 的两个平面，奇数尺寸没有合法的色度划分，编码 seam 在 OpenVideo 就拒。
    // 选区取整是调用方的责任（ResolveOutputSize 已经保证偶数）；这里只把不合法的输入
    // 拒在入口，免得编码器返回一个距离原因很远的 MF 错误。
    if (widthPx <= 0 || heightPx <= 0 || (widthPx & 1) != 0 || (heightPx & 1) != 0) {
        capture_.Close();
        captureOpen_ = false;
        SetError("invalid");
        return false;
    }

    // 音频要先于 OpenVideo：sink writer 的两条流要一起 AddStream 再 BeginWriting。
    if (options.audioEnabled) {
        bool anyOpened = false;
        if (systemAudio_ != nullptr && systemAudio_->Open()) {
            systemFormat_ = systemAudio_->Format();
            anyOpened = true;
        }
        if (microphone_ != nullptr && microphone_->Open()) {
            microphoneFormat_ = microphone_->Format();
            anyOpened = true;
        }

        if (anyOpened) {
            AudioEncoderConfig audioConfig;
            audioConfig.sampleRateHz = kAudioSampleRate;
            audioConfig.channelCount = 2;
            audioConfig.bitrateBitsPerSec = options.audioBitrateBitsPerSec;
            if (encoder_.ConfigureAudio(audioConfig)) {
                audioConfigured_ = true;
                stats_.audioActive = true;
            } else {
                // 编码器不接收第二条流（或这一路开不起来）：降级成纯视频，录制继续。
                CloseAudio();
            }
        }
    }

    VideoEncoderConfig config;
    config.widthPx = widthPx;
    config.heightPx = heightPx;
    config.targetFps = options.targetFps > 0 ? options.targetFps : 30;
    config.bitrateBitsPerSec = options.bitrateBitsPerSec;
    config.requestHardware = options.requestHardware;
    config.cropRegion = localCrop;

    if (!encoder_.OpenVideo(config, path_)) {
        capture_.Close();
        captureOpen_ = false;
        CloseAudio();
        SetError(encoder_.LastErrorKind(), encoder_.LastErrorCode());
        return false;
    }
    encoderOpen_ = true;

    clock_.Start(wallUs);
    pacer_ = FramePacer(config.targetFps);
    recording_ = true;
    startWallUs_ = wallUs;
    SetError("ok");
    return true;
}

bool RecordingSession::Pump(std::int64_t wallUs) {
    if (!recording_) {
        SetError("state");
        return false;
    }

    stats_.wallTimeUs = wallUs > startWallUs_ ? wallUs - startWallUs_ : 0;

    const bool videoOk = PumpVideo(wallUs);
    if (audioConfigured_) {
        PumpAudio(wallUs);
    }
    return videoOk;
}

bool RecordingSession::PumpVideo(std::int64_t wallUs) {
    // 暂停期间不产生样本，但采集照常排掉：桌面复制会把变化累积到下一次 WaitForUpdate，
    // 不排的话恢复后拿到的第一帧带着「暂停期间」的时间戳与脏矩形，会被当成一帧旧画面提交。
    // 排掉的帧不计入 wakeups/contentFrames——那不是这次录制的有效内容。
    if (clock_.Paused()) {
        FrameUpdate discarded;
        if (!capture_.WaitForUpdate(options_.captureTimeoutMs, discarded)) {
            const char* kind = capture_.LastErrorKind();
            if (!IsKind(kind, "timeout")) {
                SetError(kind == nullptr ? "none" : kind);
                return false;
            }
        }
        return true;
    }

    FrameUpdate update;
    if (!capture_.WaitForUpdate(options_.captureTimeoutMs, update)) {
        const char* kind = capture_.LastErrorKind();
        // 等不到帧是静态桌面的常态，不是错误。
        if (IsKind(kind, "timeout")) {
            SetError("ok");
            return true;
        }
        // 运行期只剩 lost（桌面切换、UAC、显示模式变化）。会话必须结束，由调用方重开。
        SetError(kind == nullptr ? "none" : kind);
        return false;
    }

    ++stats_.wakeups;

    // 时间戳优先用采集侧给的时刻（那是帧被拿到的时刻）；负数才表示「没给」，退到本次泵的时间。
    // 0 是合法时间戳：第一帧就落在录制起点上，不能把它当成「缺失」。
    const std::int64_t stampUs = update.timestampUs >= 0 ? update.timestampUs : wallUs;
    const std::int64_t mediaUs = clock_.MediaTimeUs(stampUs);

    // 仅鼠标唤醒的帧不送编码器（spike 2 实测 8 秒 1215 次唤醒里 1068 次只是鼠标在动），
    // 但静态桌面要保活，否则一帧样本都没有、Finalize 失败。
    if (!update.contentChanged) {
        if (options_.keepAliveUs <= 0 || update.target == 0 || mediaUs < stats_.mediaTimeUs) {
            return true;
        }
        const bool firstFrame = stats_.framesSubmitted == 0;
        const bool due = stats_.framesSubmitted > 0 &&
                         mediaUs - stats_.mediaTimeUs >= options_.keepAliveUs;
        if (!firstFrame && !due) {
            return true;
        }
        return SubmitFrame(mediaUs, update.target);
    }

    ++stats_.contentFrames;
    stats_.dirtyPixelSum += SummarizeDirtyRects(update.dirtyRects).sumAreaPx;

    if (pacer_.Decide(mediaUs) != FramePacer::Decision::Submit) {
        // 这个采样格子里已经有一帧了，或者时间戳倒退。两者都不进编码器，但要记下来：
        // 「按目标帧率取样」与「本来就没有新内容」是两件事。
        ++stats_.framesGateDropped;
        return true;
    }

    return SubmitFrame(mediaUs, update.target);
}

bool RecordingSession::SubmitFrame(std::int64_t mediaUs, ImageHandle texture) {
    EncodedFrame frame;
    frame.texture = texture;
    frame.timeUs = mediaUs;

    if (!encoder_.Submit(frame)) {
        ++stats_.framesRejected;
        const char* kind = encoder_.LastErrorKind();
        if (IsKind(kind, "device")) {
            // 设备没了：整段录制作废，会话结束（1.x 在这条路上也只提示重来）。
            SetError("device", encoder_.LastErrorCode());
            return false;
        }
        // 单帧被拒（例如写入器暂时不收）不算致命，继续泵。
        SetError("ok");
        return true;
    }

    ++stats_.framesSubmitted;
    stats_.mediaTimeUs = mediaUs;
    return true;
}

// ---- 音频 ----

std::int64_t RecordingSession::AvailableAudioFrames() const {
    const std::int64_t total = static_cast<std::int64_t>(audioPending_.size() / 2);
    return total > audioReadFrames_ ? total - audioReadFrames_ : 0;
}

std::int64_t RecordingSession::QueuedAudioUs() const {
    return FramesToUs(audioFramesSubmitted_ + AvailableAudioFrames(), kAudioSampleRate);
}

void RecordingSession::EnsurePendingFrames(std::int64_t frames) {
    if (frames <= 0) {
        return;
    }
    const std::size_t samples = static_cast<std::size_t>(frames) * 2;
    if (audioPending_.size() < samples) {
        // 补的是静音：0 就是静音，不需要另外填。
        audioPending_.resize(samples, 0.0f);
    }
}

void RecordingSession::AppendSilenceFrames(std::int64_t frames) {
    if (frames <= 0) {
        return;
    }
    EnsurePendingFrames(static_cast<std::int64_t>(audioPending_.size() / 2) + frames);
}

void RecordingSession::DiscardAudioPackets(IAudioSource* source) {
    if (source == nullptr) {
        return;
    }
    // 暂停时把设备里的包排掉，恢复后就不会拿一段旧音频当第一块。
    for (int guard = 0; guard < 8; ++guard) {
        AudioChunk chunk;
        if (!source->WaitForPacket(0, chunk)) {
            break;
        }
        ++stats_.audioPackets;
        source->ReleasePacket();
    }
}

void RecordingSession::DrainAudioSource(IAudioSource* source, const AudioFormat& format, float gain) {
    if (source == nullptr) {
        return;
    }

    // 一次最多取 8 包：音频不能把视频泵饿死。
    for (int guard = 0; guard < 8; ++guard) {
        AudioChunk chunk;
        if (!source->WaitForPacket(0, chunk)) {
            break;
        }

        audioGotPacket_ = true;
        ++stats_.audioPackets;

        // 一包正常是 480 帧（10 ms）；异常大的包直接还掉，别让它把队列顶爆。
        if (chunk.frameCount <= 0 || chunk.samples == nullptr || chunk.frameCount > kAudioSampleRate) {
            source->ReleasePacket();
            continue;
        }

        audioScratch_.resize(static_cast<std::size_t>(chunk.frameCount) * 2);
        RemixToStereo(audioScratch_.data(), chunk.samples, chunk.frameCount, format.channelCount);

        const float* stereo = audioScratch_.data();
        std::int32_t frames = chunk.frameCount;

        // 端点不是 48 kHz 时用线性重采样（音质是近似，验收靠听；1.x 也是这条退路）。
        if (format.sampleRateHz > 0 && format.sampleRateHz != kAudioSampleRate) {
            const double ratio = static_cast<double>(kAudioSampleRate) / format.sampleRateHz;
            frames = std::max<std::int32_t>(1, ResampledFrameCount(chunk.frameCount, ratio));
            audioResample_.resize(static_cast<std::size_t>(frames) * 2);
            ResampleLinear(stereo, chunk.frameCount, 2, ratio, audioResample_.data(), frames);
            stereo = audioResample_.data();
        }

        // 这一包的第一个样本该落在的绝对帧位置（相对录制起点）。
        const std::int64_t chunkUs = clock_.MediaTimeUs(chunk.timestampUs);
        std::int64_t offset = UsToFrames(chunkUs, kAudioSampleRate) - audioFramesSubmitted_;

        if (offset < 0) {
            // 设备时间戳比队列末端还早：丢掉这一包前面的部分，别把时间轴往回写。
            const std::int64_t skip = std::min<std::int64_t>(-offset, frames);
            stereo += skip * 2;
            frames -= static_cast<std::int32_t>(skip);
            stats_.audioLateFrames += skip;
            offset = 0;
        }

        if (frames > 0) {
            if (offset + frames > kAudioMaxPendingFrames) {
                // 时间轴跳得太远（设备换了/时间戳坏了）：这一包不要了。
                stats_.audioLateFrames += frames;
            } else {
                EnsurePendingFrames(offset + frames);
                MixInto(audioPending_.data() + static_cast<std::size_t>(offset) * 2, stereo, frames, gain);
            }
        }

        source->ReleasePacket();
    }
}

void RecordingSession::FlushAudioBlocks(bool padLastBlock) {
    if (!audioConfigured_) {
        return;
    }

    if (padLastBlock && AvailableAudioFrames() > 0) {
        const std::int32_t remainder =
            static_cast<std::int32_t>(AvailableAudioFrames() % kAacFrameCount);
        if (remainder != 0) {
            AppendSilenceFrames(kAacFrameCount - remainder);
        }
    }

    audioPcm_.resize(static_cast<std::size_t>(kAacFrameCount) * 2);
    while (AvailableAudioFrames() >= kAacFrameCount) {
        ConvertFloatToPcm16(audioPending_.data() + static_cast<std::size_t>(audioReadFrames_) * 2,
                            kAacFrameCount * 2, audioPcm_.data());

        AudioFrame frame;
        frame.samples = audioPcm_.data();
        frame.frameCount = kAacFrameCount;
        frame.timeUs = FramesToUs(audioFramesSubmitted_, kAudioSampleRate);

        if (!encoder_.SubmitAudio(frame)) {
            ++stats_.audioRejectedBlocks;
            break;
        }

        audioFramesSubmitted_ += kAacFrameCount;
        audioReadFrames_ += kAacFrameCount;
        stats_.audioFrames += kAacFrameCount;
        ++stats_.audioBlocks;
    }

    // 每秒整理一次：帧循环里不做大搬移，但也别让 vector 一直往前长。
    if (audioReadFrames_ >= kAudioSampleRate) {
        audioPending_.erase(audioPending_.begin(),
                            audioPending_.begin() + static_cast<std::ptrdiff_t>(audioReadFrames_) * 2);
        audioReadFrames_ = 0;
    }
}

void RecordingSession::PumpAudio(std::int64_t wallUs) {
    if (!audioConfigured_) {
        return;
    }

    if (clock_.Paused()) {
        // 暂停时静音不动时间轴：只把设备里的包排掉。
        DiscardAudioPackets(systemAudio_);
        DiscardAudioPackets(microphone_);
        return;
    }

    audioGotPacket_ = false;
    DrainAudioSource(systemAudio_, systemFormat_, options_.systemAudioGain);
    DrainAudioSource(microphone_, microphoneFormat_, options_.microphoneGain);

    // 回环保活：桌面静音时回环一个包都不出（§5.1 实测），只能按墙钟补静音。
    // 有包时不补——包的设备时间戳比墙钟更准，用它对齐。
    if (!audioGotPacket_) {
        const std::int64_t queuedUs = QueuedAudioUs();
        const std::int64_t wallMediaUs = clock_.MediaTimeUs(wallUs);
        if (wallMediaUs > queuedUs + 100'000) {
            const std::int64_t fillUs = std::min(wallMediaUs - queuedUs, options_.audioSilenceLimitUs);
            const std::int64_t frames = UsToFrames(fillUs, kAudioSampleRate);
            if (frames > 0) {
                AppendSilenceFrames(frames);
                stats_.audioSilenceFrames += frames;
            }
        }
    }

    FlushAudioBlocks(false);
}

void RecordingSession::CloseAudio() {
    if (systemAudio_ != nullptr) {
        systemAudio_->Close();
    }
    if (microphone_ != nullptr) {
        microphone_->Close();
    }
    audioConfigured_ = false;
    audioPending_.clear();
    audioReadFrames_ = 0;
    audioFramesSubmitted_ = 0;
}

// ---- 状态切换 ----

bool RecordingSession::Pause(std::int64_t wallUs) {
    if (!recording_) {
        SetError("state");
        return false;
    }
    clock_.Pause(wallUs);
    SetError("ok");
    return true;
}

bool RecordingSession::Resume(std::int64_t wallUs) {
    if (!recording_) {
        SetError("state");
        return false;
    }
    clock_.Resume(wallUs);
    SetError("ok");
    return true;
}

bool RecordingSession::Stop(std::int64_t wallUs, RecordingStats& outStats) {
    if (!recording_) {
        SetError("state");
        outStats = stats_;
        return false;
    }

    // 时长按媒体时间算：暂停的时间不该算进文件的长度。
    stats_.wallTimeUs = clock_.MediaTimeUs(wallUs);

    if (captureOpen_) {
        capture_.Close();
        captureOpen_ = false;
    }

    // 音频必须在编码器收尾之前写完：最后一块补静音凑到 1024。
    if (audioConfigured_) {
        FlushAudioBlocks(true);
        CloseAudio();
    }

    bool ok = true;
    EncoderStats encoderStats;
    if (encoderOpen_) {
        ok = encoder_.Close(encoderStats);
        encoderOpen_ = false;
        MergeEncoderStats(encoderStats);
        if (!ok) {
            SetError(encoder_.LastErrorKind(), encoder_.LastErrorCode());
        }
    }

    recording_ = false;
    outStats = stats_;
    if (ok) {
        SetError("ok");
    }
    return ok;
}

void RecordingSession::MergeEncoderStats(const EncoderStats& encoderStats) {
    stats_.outputBytes = encoderStats.outputBytes;
    if (encoderStats.mediaTimeUs > stats_.mediaTimeUs) {
        stats_.mediaTimeUs = encoderStats.mediaTimeUs;
    }
    stats_.hardwareEncoded = encoderStats.hardwareEncoded;
    // 被拒帧数不在这里并：Submit 返回 false 时已经算过一次，并两次会重复计数。
}

void RecordingSession::CloseOnDestroy() {
    if (!recording_) {
        return;
    }
    if (captureOpen_) {
        capture_.Close();
        captureOpen_ = false;
    }
    if (audioConfigured_) {
        FlushAudioBlocks(true);
        CloseAudio();
    }
    if (encoderOpen_) {
        EncoderStats ignored;
        encoder_.Close(ignored);
        encoderOpen_ = false;
    }
    recording_ = false;
}

}
