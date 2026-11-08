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

    core::VideoEncoderConfig config;
    std::string path;
    std::vector<core::EncodedFrame> submitted;
    int openCount = 0;
    int closeCount = 0;
    core::EncoderStats stats;

    bool OpenVideo(const core::VideoEncoderConfig& value, std::string_view outputPath) override {
        ++openCount;
        config = value;
        path = std::string(outputPath);
        error = openResult ? "ok" : openError;
        return openResult;
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

struct Session {
    FakeCapture capture;
    FakeEncoder encoder;
    core::RecordingSession session{ capture, encoder };
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
        core::RecordingSession session(capture, encoder);
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

}

int main() {
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

    if (failures != 0) {
        std::printf("RECORDING_SESSION_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }

    std::printf("RECORDING_SESSION_TESTS=OK checks=%d\n", checks);
    return 0;
}
