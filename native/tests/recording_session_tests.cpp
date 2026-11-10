// S2 的编排层：帧从采集 seam 进来、过时钟与闸门、进编码 seam。
// 三个 seam 都用假的，所以这里验的是「编排」本身：只送有变化的帧、按目标帧率闸门、
// 暂停冻结时间轴、停止时收尾编码器、错误怎么分类。
#include "recording_session.h"

#include <cstdio>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace core = halocap::core;

int failures = 0;
int checks = 0;

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s: expected true\n", what);
    }
}

void CheckText(const char* actual, const char* expected, const char* what) {
    ++checks;
    const std::string_view a = actual == nullptr ? "" : actual;
    const std::string_view b = expected == nullptr ? "" : expected;
    if (a != b) {
        ++failures;
        std::printf("FAIL %s: got \"%.*s\" want \"%.*s\"\n", what, static_cast<int>(a.size()),
                    a.data(), static_cast<int>(b.size()), b.data());
    }
}

void CheckNear(double actual, double expected, double tolerance, const char* what) {
    ++checks;
    if (actual < expected - tolerance || actual > expected + tolerance) {
        ++failures;
        std::printf("FAIL %s: got %.4f want %.4f\n", what, actual, expected);
    }
}

class FakeCapture final : public core::ICaptureSource {
public:
    bool openResult = true;
    int openCount = 0;
    bool closeCalled = false;
    core::IntRect bounds{ 0, 0, 1920, 1080 };
    std::deque<core::FrameUpdate> pending;
    const char* error = "ok";

    // 非空时 WaitForUpdate 直接失败并报这个 kind（模拟运行期丢设备）。
    const char* failKind = nullptr;

    bool Open(int) override {
        ++openCount;
        error = openResult ? "ok" : "busy";
        return openResult;
    }

    void Close() override { closeCalled = true; }

    bool WaitForUpdate(std::int64_t, core::FrameUpdate& out) override {
        if (failKind != nullptr) {
            error = failKind;
            return false;
        }
        if (pending.empty()) {
            error = "timeout";
            return false;
        }
        out = pending.front();
        pending.pop_front();
        error = "ok";
        return true;
    }

    const char* LastErrorKind() const override { return error; }

    core::IntRect DesktopBounds() const override { return bounds; }
};

class FakeEncoder final : public core::IEncoder {
public:
    bool openResult = true;
    const char* openError = "ok";
    bool submitResult = true;
    const char* submitError = "ok";
    bool closeResult = true;

    // 音频第二条流：默认不支持，与「只做视频的编码器」一致。
    bool audioSupported = false;
    bool audioConfigured = false;
    core::AudioEncoderConfig audioConfig;
    std::vector<std::pair<std::int64_t, std::int32_t>> audioBlocks;

    // 调用顺序：用来证明 ConfigureAudio 在 OpenVideo 之前（sink writer 的时序约束）。
    std::vector<std::string> calls;

    core::VideoEncoderConfig config;
    std::string path;
    std::vector<core::EncodedFrame> submitted;
    int openCount = 0;
    int closeCount = 0;
    core::EncoderStats stats;

    bool OpenVideo(const core::VideoEncoderConfig& value, std::string_view outputPath) override {
        ++openCount;
        calls.push_back("video");
        config = value;
        path = std::string(outputPath);
        error = openResult ? "ok" : openError;
        return openResult;
    }

    bool ConfigureAudio(const core::AudioEncoderConfig& value) override {
        calls.push_back("audio");
        if (!audioSupported) {
            return false;
        }
        audioConfigured = true;
        audioConfig = value;
        return true;
    }

    bool SubmitAudio(const core::AudioFrame& frame) override {
        if (!audioSupported || !audioConfigured) {
            return false;
        }
        audioBlocks.emplace_back(frame.timeUs, frame.frameCount);
        return true;
    }

    bool Submit(const core::EncodedFrame& frame) override {
        if (!submitResult) {
            error = submitError;
            return false;
        }
        submitted.push_back(frame);
        error = "ok";
        return true;
    }

    bool Close(core::EncoderStats& outStats) override {
        ++closeCount;
        outStats = stats;
        error = closeResult ? "ok" : "mf";
        return closeResult;
    }

    const char* LastErrorKind() const override { return error; }
    std::int32_t LastErrorCode() const override { return closeResult ? 0 : 0xC00D; }

private:
    const char* error = "ok";
};

core::FrameUpdate Frame(std::int64_t timestampUs, core::ImageHandle texture, bool changed = true) {
    core::FrameUpdate update;
    update.timestampUs = timestampUs;
    update.target = texture;
    update.contentChanged = changed;
    update.accumulatedFrames = changed ? 1 : 0;
    return update;
}

core::RecordingOptions DefaultOptions() {
    core::RecordingOptions options;
    options.outputPathUtf8 = "C:\\temp\\recording.mp4";
    options.targetFps = 30;
    return options;
}

class FakeAudioSource final : public core::IAudioSource {
public:
    core::AudioFormat format{ 48000, 2, true };
    bool openResult = true;
    int openCount = 0;
    bool closeCalled = false;
    const char* error = "ok";

    std::deque<std::vector<float>> packets;
    std::deque<std::int64_t> timestamps;
    bool held = false;

    void Push(std::int64_t timestampUs, std::int32_t frames, float sample = 0.25f) {
        packets.emplace_back(static_cast<std::size_t>(frames) * 2, sample);
        timestamps.push_back(timestampUs);
    }

    bool Open() override {
        ++openCount;
        error = openResult ? "ok" : "notfound";
        return openResult;
    }

    void Close() override { closeCalled = true; }

    core::AudioFormat Format() const override { return format; }

    bool WaitForPacket(std::int64_t, core::AudioChunk& out) override {
        if (held) {
            // 借还契约：没还就想再等，seam 的设计要求这里拒。
            error = "invalid";
            return false;
        }
        if (packets.empty()) {
            error = "timeout";
            return false;
        }
        out.samples = packets.front().data();
        out.frameCount = static_cast<std::int32_t>(packets.front().size() / 2);
        out.timestampUs = timestamps.front();
        out.silent = false;
        out.discontinuity = false;
        held = true;
        error = "ok";
        return true;
    }

    void ReleasePacket() override {
        if (!held) {
            return;
        }
        held = false;
        packets.pop_front();
        timestamps.pop_front();
    }

    const char* LastErrorKind() const override { return error; }
    std::int32_t LastErrorCode() const override { return 0; }
    std::string EndpointNameUtf8() const override { return "fake audio"; }
};

struct Session {
    FakeCapture capture;
    FakeEncoder encoder;
    core::RecordingSession session{ capture, encoder, nullptr, nullptr };
};

void TestStartOpensBothSeams() {
    Session s;
    CheckTrue(s.session.Start(DefaultOptions(), 1000), "start succeeds");

    CheckEqual(s.capture.openCount, 1, "capture opened once");
    CheckEqual(s.encoder.openCount, 1, "encoder opened once");
    CheckEqual(s.encoder.config.widthPx, 1920, "encoder width comes from the capture bounds");
    CheckEqual(s.encoder.config.heightPx, 1080, "encoder height comes from the capture bounds");
    CheckEqual(s.encoder.config.targetFps, 30, "encoder frames per second");
    CheckEqual(s.encoder.config.bitrateBitsPerSec, 0, "zero bitrate means the default rule");
    CheckTrue(s.encoder.config.requestHardware, "hardware is requested by default");
    CheckTrue(s.encoder.path == "C:\\temp\\recording.mp4", "output path is forwarded");

    CheckTrue(s.session.Recording(), "session reports recording");
    CheckTrue(!s.session.Paused(), "session is not paused after start");
    CheckText(s.session.LastErrorKind(), "ok", "start leaves no error");
}

void TestStartGuards() {
    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "first start succeeds");
        CheckTrue(!s.session.Start(DefaultOptions(), 1), "second start is refused");
        CheckText(s.session.LastErrorKind(), "state", "second start reports state");
        CheckEqual(s.capture.openCount, 1, "second start does not reopen capture");
    }

    {
        Session s;
        s.capture.openResult = false;
        CheckTrue(!s.session.Start(DefaultOptions(), 0), "capture failure fails start");
        CheckText(s.session.LastErrorKind(), "busy", "capture busy is carried out as busy");
        CheckEqual(s.encoder.openCount, 0, "encoder is not opened when capture fails");
    }

    {
        // 奇数尺寸没有合法的 NV12 色度划分：拒在入口，不交给编码器。
        Session s;
        s.capture.bounds = core::IntRect{ 0, 0, 1921, 1080 };
        CheckTrue(!s.session.Start(DefaultOptions(), 0), "odd capture size fails start");
        CheckText(s.session.LastErrorKind(), "invalid", "odd size reports invalid");
        CheckTrue(s.capture.closeCalled, "capture is closed when the size is rejected");
        CheckEqual(s.encoder.openCount, 0, "encoder is not opened for an odd size");
    }

    {
        Session s;
        core::RecordingOptions options = DefaultOptions();
        options.outputPathUtf8.clear();
        CheckTrue(!s.session.Start(options, 0), "empty output path fails start");
        CheckText(s.session.LastErrorKind(), "invalid", "empty path reports invalid");
        CheckEqual(s.capture.openCount, 0, "capture is not opened without a path");
    }

    {
        Session s;
        s.encoder.openResult = false;
        s.encoder.openError = "mf";
        CheckTrue(!s.session.Start(DefaultOptions(), 0), "encoder failure fails start");
        CheckText(s.session.LastErrorKind(), "mf", "encoder error kind is carried out");
        CheckTrue(s.capture.closeCalled, "capture is closed when the encoder fails");
    }
}

void TestPumpSubmitsContentFrames() {
    Session s;
    CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
    s.capture.pending.push_back(Frame(0, 101));
    s.capture.pending.push_back(Frame(40'000, 102));
    s.capture.pending.push_back(Frame(80'000, 103));

    CheckTrue(s.session.Pump(10'000), "pump 1");
    CheckTrue(s.session.Pump(50'000), "pump 2");
    CheckTrue(s.session.Pump(90'000), "pump 3");

    CheckEqual(static_cast<long long>(s.encoder.submitted.size()), 3, "three frames submitted");
    if (s.encoder.submitted.size() == 3) {
        CheckEqual(s.encoder.submitted[0].timeUs, 0, "first frame media time");
        CheckEqual(s.encoder.submitted[1].timeUs, 40'000, "second frame media time");
        CheckEqual(s.encoder.submitted[2].timeUs, 80'000, "third frame media time");
        CheckEqual(s.encoder.submitted[0].texture, 101, "texture handle is forwarded");
    }
    CheckEqual(s.session.Stats().framesSubmitted, 3, "submitted count");
    CheckEqual(s.session.Stats().wakeups, 3, "wakeup count");
    CheckEqual(s.session.Stats().contentFrames, 3, "content frame count");
    CheckEqual(s.session.Stats().wallTimeUs, 90'000, "wall time follows the pump clock");
}

void TestStaticFramesKeepAlive() {
    Session s;
    CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
    s.capture.pending.push_back(Frame(0, 100, false));       // 静态：第一帧保活
    s.capture.pending.push_back(Frame(40'000, 101, false));  // 一秒内：跳过
    s.capture.pending.push_back(Frame(80'000, 102, true));   // 有内容：正常走网格取样

    CheckTrue(s.session.Pump(10'000), "pump 1");
    CheckTrue(s.session.Pump(50'000), "pump 2");
    CheckTrue(s.session.Pump(90'000), "pump 3");

    CheckEqual(s.session.Stats().wakeups, 3, "all wakeups counted");
    CheckEqual(s.session.Stats().contentFrames, 1, "only one frame had content");
    CheckEqual(s.session.Stats().framesSubmitted, 2, "keep-alive frame plus the content frame");

    s.capture.pending.push_back(Frame(1'200'000, 103, false));
    CheckTrue(s.session.Pump(1'200'000), "pump 4");
    CheckEqual(s.session.Stats().framesSubmitted, 3, "a keep-alive frame is sent after one second");

    if (!s.encoder.submitted.empty()) {
        CheckEqual(s.encoder.submitted[0].texture, 100, "the first static frame is the keep-alive");
    }
}

void TestGateDropsFastAndBackwardsFrames() {
    {
        // 目标 30 fps（33.3 ms）；10 ms 一帧会被网格取样丢到约 30 fps，而不是每帧都送。
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        s.capture.pending.push_back(Frame(0, 1));
        s.capture.pending.push_back(Frame(10'000, 2));
        s.capture.pending.push_back(Frame(20'000, 3));
        s.session.Pump(0);
        s.session.Pump(10'000);
        s.session.Pump(20'000);
        CheckEqual(s.session.Stats().framesSubmitted, 2, "two of three frames share a grid slot");
        CheckEqual(s.session.Stats().framesGateDropped, 1, "one frame is skipped by the pacer");
    }

    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        s.capture.pending.push_back(Frame(80'000, 1));
        s.capture.pending.push_back(Frame(40'000, 2));   // 倒退
        s.session.Pump(0);
        s.session.Pump(0);
        CheckEqual(s.session.Stats().framesSubmitted, 1, "first frame submitted");
        CheckEqual(s.session.Stats().framesGateDropped, 1, "backwards frame is dropped");
    }
}

void TestPauseFreezesTimeline() {
    Session s;
    CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
    s.capture.pending.push_back(Frame(0, 1));
    CheckTrue(s.session.Pump(0), "pump before pause");

    CheckTrue(s.session.Pause(1'000'000), "pause");
    CheckTrue(s.session.Paused(), "paused");
    s.capture.pending.push_back(Frame(1'500'000, 2));
    CheckTrue(s.session.Pump(1'500'000), "pump while paused");
    CheckEqual(static_cast<long long>(s.encoder.submitted.size()), 1,
               "no frame is submitted while paused");
    // 暂停期间采集照常排掉：这一帧不会被留下当作恢复后的第一帧。
    CheckEqual(static_cast<long long>(s.capture.pending.size()), 0,
               "the stale frame is drained while paused");
    CheckEqual(s.session.Stats().wakeups, 1, "drained frames are not counted as wakeups");

    CheckTrue(s.session.Resume(2'000'000), "resume");
    CheckTrue(!s.session.Paused(), "no longer paused");
    s.capture.pending.push_back(Frame(2'500'000, 3));
    CheckTrue(s.session.Pump(2'500'000), "pump after resume");

    // 暂停的 1 秒不进媒体时间：这一帧在墙钟 2.5s、采集时间戳 2.5s，减去暂停的 1s 后是 1.5s。
    CheckEqual(static_cast<long long>(s.encoder.submitted.size()), 2, "two frames submitted");
    if (s.encoder.submitted.size() == 2) {
        CheckEqual(s.encoder.submitted[1].timeUs, 1'500'000,
                   "pause is removed from the media timeline");
    }
}

void TestEncoderRejectionAndDeviceLoss() {
    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        s.encoder.submitResult = false;
        s.encoder.submitError = "hr";
        s.capture.pending.push_back(Frame(0, 1));
        CheckTrue(s.session.Pump(0), "a rejected frame does not end the session");
        CheckEqual(s.session.Stats().framesRejected, 1, "rejected frame is counted");
        CheckEqual(s.session.Stats().framesSubmitted, 0, "rejected frame is not submitted");
        CheckText(s.session.LastErrorKind(), "ok", "rejection is not a session error");
    }

    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        s.encoder.submitResult = false;
        s.encoder.submitError = "device";
        s.capture.pending.push_back(Frame(0, 1));
        CheckTrue(!s.session.Pump(0), "device loss ends the session");
        CheckText(s.session.LastErrorKind(), "device", "device loss is reported");
    }
}

void TestCaptureTimeoutAndLoss() {
    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        CheckTrue(s.session.Pump(10'000), "an empty pump is not an error");
        CheckText(s.session.LastErrorKind(), "ok", "timeout leaves no error");
        CheckEqual(s.session.Stats().wakeups, 0, "no wakeup on timeout");
    }

    {
        Session s;
        CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
        s.capture.failKind = "lost";
        CheckTrue(!s.session.Pump(0), "capture loss ends the session");
        CheckText(s.session.LastErrorKind(), "lost", "capture loss is reported as lost");
    }
}

void TestStopMergesStats() {
    Session s;
    CheckTrue(s.session.Start(DefaultOptions(), 0), "start");
    s.capture.pending.push_back(Frame(0, 1));
    s.session.Pump(0);

    s.encoder.stats.outputBytes = 123'456;
    s.encoder.stats.mediaTimeUs = 500'000;
    s.encoder.stats.hardwareEncoded = true;

    core::RecordingStats stats;
    CheckTrue(s.session.Stop(1'000'000, stats), "stop succeeds");
    CheckTrue(!s.session.Recording(), "session is no longer recording");
    CheckEqual(s.encoder.closeCount, 1, "encoder closed once");
    CheckTrue(s.capture.closeCalled, "capture closed");

    CheckEqual(stats.outputBytes, 123'456, "output bytes come from the encoder");
    CheckTrue(stats.hardwareEncoded, "hardware observation is carried out");
    CheckEqual(stats.framesSubmitted, 1, "session stats survive the stop");
    CheckEqual(stats.wallTimeUs, 1'000'000, "duration is the media time at stop");
    CheckEqual(s.session.Stats().outputBytes, 123'456, "session keeps the final stats");

    core::RecordingStats again;
    CheckTrue(!s.session.Stop(2'000'000, again), "stopping twice is refused");
    CheckText(s.session.LastErrorKind(), "state", "second stop reports state");
}

void TestDestructorClosesSeams() {
    FakeCapture capture;
    FakeEncoder encoder;
    {
        core::RecordingSession session(capture, encoder, nullptr, nullptr);
        CheckTrue(session.Start(DefaultOptions(), 0), "start in scope");
    }
    CheckEqual(encoder.closeCount, 1, "destructor closes the encoder");
    CheckTrue(capture.closeCalled, "destructor closes the capture");
}

void TestSubmitRatio() {
    core::RecordingStats stats;
    stats.wallTimeUs = 1'000'000;
    stats.framesSubmitted = 30;
    CheckNear(stats.SubmitRatio(30), 1.0, 0.0001, "30 frames in one second at 30 fps is 1.0");

    stats.framesSubmitted = 15;
    CheckNear(stats.SubmitRatio(30), 0.5, 0.0001, "half the frames is 0.5");
    CheckNear(stats.SubmitRatio(0), 0.0, 0.0001, "no target fps has no ratio");

    stats.wallTimeUs = 0;
    CheckNear(stats.SubmitRatio(30), 0.0, 0.0001, "no duration has no ratio");
}

void TestAudioConfiguredBeforeVideo() {
    FakeCapture capture;
    FakeEncoder encoder;
    encoder.audioSupported = true;
    FakeAudioSource audio;
    core::RecordingSession session(capture, encoder, &audio, nullptr);

    core::RecordingOptions options = DefaultOptions();
    options.audioEnabled = true;
    CheckTrue(session.Start(options, 0), "start with audio");

    CheckTrue(encoder.audioConfigured, "audio stream is configured");
    CheckEqual(encoder.audioConfig.sampleRateHz, 48000, "audio sample rate");
    CheckEqual(encoder.audioConfig.channelCount, 2, "audio channels");
    CheckEqual(encoder.audioConfig.bitrateBitsPerSec, 192000, "audio bitrate");
    CheckEqual(session.Stats().audioActive ? 1 : 0, 1, "stats report audio active");

    // sink writer 的时序：两条流要一起 AddStream，所以音频配置必须发生在 OpenVideo 之前。
    CheckTrue(encoder.calls.size() >= 2, "encoder saw both calls");
    if (encoder.calls.size() >= 2) {
        CheckTrue(encoder.calls[0] == "audio", "audio is configured before the video open");
        CheckTrue(encoder.calls[1] == "video", "video open comes second");
    }

    core::RecordingStats stats;
    CheckTrue(session.Stop(0, stats), "stop with audio");
}

void TestAudioBlocksAndTimeline() {
    FakeCapture capture;
    FakeEncoder encoder;
    encoder.audioSupported = true;
    FakeAudioSource audio;
    core::RecordingSession session(capture, encoder, &audio, nullptr);

    core::RecordingOptions options = DefaultOptions();
    options.audioEnabled = true;
    options.keepAliveUs = 0;
    CheckTrue(session.Start(options, 0), "start");

    // 两包各 480 帧（10 ms），时间戳 0 与 10 ms：960 帧，不够一块 1024。
    audio.Push(0, 480);
    audio.Push(10'000, 480);
    CheckTrue(session.Pump(20'000), "pump 1");
    CheckEqual(static_cast<long long>(encoder.audioBlocks.size()), 0, "no block until 1024 frames");

    // 第三包补齐：1440 帧 → 提交一块。
    audio.Push(20'000, 480);
    CheckTrue(session.Pump(30'000), "pump 2");
    CheckEqual(static_cast<long long>(encoder.audioBlocks.size()), 1, "one block after 1440 frames");
    if (!encoder.audioBlocks.empty()) {
        CheckEqual(encoder.audioBlocks[0].first, 0, "the first block starts at media time zero");
        CheckEqual(encoder.audioBlocks[0].second, 1024, "blocks are 1024 frames");
    }
    CheckEqual(session.Stats().audioFrames, 1024, "frames submitted to the encoder");

    core::RecordingStats stats;
    CheckTrue(session.Stop(35'000, stats), "stop");
    CheckEqual(stats.audioBlocks, 2, "the remainder is padded to a whole block at stop");
    CheckEqual(stats.audioFrames, 2048, "two blocks total");
    CheckTrue(stats.audioActive, "stats still report audio active after stop");
}

void TestAudioSilenceFill() {
    FakeCapture capture;
    FakeEncoder encoder;
    encoder.audioSupported = true;
    FakeAudioSource audio;   // 一个包都不出：回环在桌面静音时就是这样
    core::RecordingSession session(capture, encoder, &audio, nullptr);

    core::RecordingOptions options = DefaultOptions();
    options.audioEnabled = true;
    options.keepAliveUs = 0;
    CheckTrue(session.Start(options, 0), "start");

    // 一秒没有包：一次最多补 500 ms 静音（24000 帧），所以有 23 整块。
    CheckTrue(session.Pump(1'000'000), "pump with no packets");
    CheckEqual(session.Stats().audioSilenceFrames, 24'000, "silence is capped at 500 ms per pump");
    CheckEqual(session.Stats().audioBlocks, 23, "23 whole blocks from the silence fill");

    core::RecordingStats stats;
    CheckTrue(session.Stop(1'000'000, stats), "stop");
}

void TestAudioUnavailableFallsBackToVideo() {
    FakeCapture capture;
    FakeEncoder encoder;
    encoder.audioSupported = true;
    FakeAudioSource audio;
    audio.openResult = false;   // 没有录音设备 / 被独占
    core::RecordingSession session(capture, encoder, &audio, nullptr);

    core::RecordingOptions options = DefaultOptions();
    options.audioEnabled = true;
    CheckTrue(session.Start(options, 0), "recording still starts without audio");
    CheckTrue(!encoder.audioConfigured, "no audio stream is added when no source opened");
    CheckEqual(session.Stats().audioActive ? 1 : 0, 0, "audio is inactive");

    // 视频照常录：没有声音不该让一次录制作废。
    capture.pending.push_back(Frame(0, 1));
    CheckTrue(session.Pump(0), "video still records");
    CheckEqual(session.Stats().framesSubmitted, 1, "one video frame submitted");

    core::RecordingStats stats;
    CheckTrue(session.Stop(0, stats), "stop");
}

void TestAccessorsAndAudioLevels() {
    FakeCapture capture;
    FakeEncoder encoder;
    encoder.audioSupported = true;
    FakeAudioSource system;
    core::RecordingSession session(capture, encoder, &system, nullptr);

    core::RecordingOptions options = DefaultOptions();
    options.audioEnabled = true;
    options.keepAliveUs = 0;
    CheckTrue(session.Start(options, 0), "start with system audio");

    // 尺寸/帧率直接转给浮层信息行（1.x BuildHudInfo 的那几个数）。
    CheckEqual(session.OutputWidth(), 1920, "output width from the capture bounds");
    CheckEqual(session.OutputHeight(), 1080, "output height from the capture bounds");
    CheckEqual(session.TargetFps(), 30, "target fps from the options");

    // 系统声音开着、麦克风没开：两个活跃标志要分开，浮层据此决定画几路电平表。
    CheckTrue(session.SystemAudioActive(), "system audio is active");
    CheckTrue(!session.MicrophoneActive(), "microphone is not active");

    // 一包满幅 0.7 的样本：Pump 之后系统电平就是这一包的峰值。
    system.Push(0, 480, 0.7f);
    CheckTrue(session.Pump(10'000), "pump with a loud packet");
    CheckNear(session.SystemAudioLevel(), 0.7, 1e-4, "system level follows the packet peak");
    CheckNear(session.MicrophoneLevel(), 0.0, 1e-6, "an inactive microphone stays at zero");

    // 下一拍没有包：电平重算后回零，不残留上一拍的峰值（否则淡出就是假的）。
    system.packets.clear();
    system.timestamps.clear();
    CheckTrue(session.Pump(100'000), "pump with no packet");
    CheckNear(session.SystemAudioLevel(), 0.0, 1e-6, "level drops when no packet arrives");

    core::RecordingStats stats;
    CheckTrue(session.Stop(100'000, stats), "stop");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // 崩溃时也要能看到走到哪一步：默认的 stdout 是块缓冲，__fastfail 会跳过刷新。
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestStartOpensBothSeams();
    TestStartGuards();
    TestPumpSubmitsContentFrames();
    TestStaticFramesKeepAlive();
    TestGateDropsFastAndBackwardsFrames();
    TestPauseFreezesTimeline();
    TestEncoderRejectionAndDeviceLoss();
    TestCaptureTimeoutAndLoss();
    TestStopMergesStats();
    TestDestructorClosesSeams();
    TestSubmitRatio();
    TestAudioConfiguredBeforeVideo();
    TestAudioBlocksAndTimeline();
    TestAudioSilenceFill();
    TestAudioUnavailableFallsBackToVideo();
    TestAccessorsAndAudioLevels();

    if (failures != 0) {
        std::printf("RECORDING_SESSION_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("RECORDING_SESSION_TESTS=OK checks=%d\n", checks);
    return 0;
}
