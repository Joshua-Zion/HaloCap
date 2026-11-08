#include "recording_session.h"

#include <string_view>

namespace halocap::core {

namespace {

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

RecordingSession::RecordingSession(ICaptureSource& capture, IEncoder& encoder)
    : capture_(capture), encoder_(encoder) {}

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
    if (path_.empty()) {
        SetError("invalid");
        return false;
    }

    if (!capture_.Open(options.outputIndex)) {
        const char* kind = capture_.LastErrorKind();
        // 复制权被占用（busy）是常态分支：调用方按 BackoffUs 重试，不是给用户报错。
        SetError(kind == nullptr ? "none" : kind);
        return false;
    }
    captureOpen_ = true;

    const IntRect bounds = capture_.DesktopBounds();
    const int widthPx = bounds.right - bounds.left;
    const int heightPx = bounds.bottom - bounds.top;

    // NV12 是 4:2:0 的两个平面，奇数尺寸没有合法的色度划分，编码 seam 在 OpenVideo 就拒。
    // 选区取整是调用方的责任（ResolveOutputSize 已经保证偶数）；这里只把不合法的输入
    // 拒在入口，免得编码器返回一个距离原因很远的 MF 错误。
    if (widthPx <= 0 || heightPx <= 0 || (widthPx & 1) != 0 || (heightPx & 1) != 0) {
        capture_.Close();
        captureOpen_ = false;
        SetError("invalid");
        return false;
    }

    VideoEncoderConfig config;
    config.widthPx = widthPx;
    config.heightPx = heightPx;
    config.targetFps = options.targetFps > 0 ? options.targetFps : 30;
    config.bitrateBitsPerSec = options.bitrateBitsPerSec;
    config.requestHardware = options.requestHardware;

    if (!encoder_.OpenVideo(config, path_)) {
        capture_.Close();
        captureOpen_ = false;
        SetError(encoder_.LastErrorKind(), encoder_.LastErrorCode());
        return false;
    }
    encoderOpen_ = true;

    clock_.Start(wallUs);
    gate_ = FrameGate(config.targetFps);
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

    // 仅鼠标唤醒的帧不送编码器：spike 2 实测 8 秒 1215 次唤醒里 1068 次只是鼠标在动。
    if (!update.contentChanged) {
        return true;
    }

    ++stats_.contentFrames;
    stats_.dirtyPixelSum += SummarizeDirtyRects(update.dirtyRects).sumAreaPx;

    // 时间戳优先用采集侧给的时刻（那是帧被拿到的时刻）；负数才表示「没给」，退到本次泵的时间。
    // 0 是合法时间戳：第一帧就落在录制起点上，不能把它当成「缺失」。
    const std::int64_t stampUs = update.timestampUs >= 0 ? update.timestampUs : wallUs;
    const std::int64_t mediaUs = clock_.MediaTimeUs(stampUs);

    if (gate_.Decide(mediaUs) != FrameGate::Decision::Submit) {
        // 太早（比目标帧率快）或时间戳倒退。两种都不进编码器，但要记下来：
        // 「丢帧」与「本来就没有新内容」是两件事。
        ++stats_.framesGateDropped;
        return true;
    }

    EncodedFrame frame;
    frame.texture = update.target;
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
    if (encoderOpen_) {
        EncoderStats ignored;
        encoder_.Close(ignored);
        encoderOpen_ = false;
    }
    recording_ = false;
}

}
