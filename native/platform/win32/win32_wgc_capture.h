#pragma once

#include "capture_source.h"
#include "d2d_render_backend.h"

#include <windows.h>

#include <winrt/Windows.Graphics.Capture.h>

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>

namespace halocap::platform::win32 {

// ICaptureSource 的 WGC（Windows.Graphics.Capture）实现：采一个窗口，而不是一块输出。
//
// 为什么窗口录制要单独留 WGC（计划 §4）：桌面复制只能拿到整块输出的像素与脏矩形，
// 拿不到「某个窗口」的边界、遮挡与 DPI 语义；WGC 是官方唯一按窗口取帧的路径。
//
// 与桌面复制不同，WGC **没有脏矩形**：每一帧都是整窗。所以这一路只适合窗口录制，
// 显示器录制仍然走复制，两条路各自最优。
//
// 帧被拷进一张常驻 BGRA 纹理（RT|SRV），再以句柄交给渲染后端的资源表——与复制路径
// 同一个形状，所以编码器那边一行都不用改。
class Win32WgcCapture final : public core::ICaptureSource {
public:
    explicit Win32WgcCapture(D2dRenderBackend& backend);
    ~Win32WgcCapture() override;

    // 不支持：窗口录制用 OpenWindow。
    bool Open(int outputIndex) override;

    bool OpenWindow(core::NativeHandle window) override;

    // 显示器采集（WGC 的 CreateForMonitor）：桌面上也要带光标。
    bool OpenMonitor(int outputIndex) override;

    void SetCursorCapture(bool enabled) override { cursorCapture_ = enabled; }

    void Close() override;

    bool WaitForUpdate(std::int64_t timeoutMs, core::FrameUpdate& out) override;

    const char* LastErrorKind() const override { return errorKind_; }

    core::IntRect DesktopBounds() const override { return bounds_; }

    // 只给探针：当前常驻纹理的尺寸（0 表示还没有帧）。
    int WidthPx() const { return widthPx_; }
    int HeightPx() const { return heightPx_; }

private:
    struct State;

    bool StartItem(const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item,
                   POINT origin = POINT{ 0, 0 });
    bool EnsureResident(int widthPx, int heightPx);
    void ReleaseResident();

    D2dRenderBackend& backend_;
    std::unique_ptr<State> state_;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> resident_;
    core::ImageHandle residentHandle_ = 0;

    core::IntRect bounds_{};
    int widthPx_ = 0;
    int heightPx_ = 0;
    // 显示器录制时该显示器在屏幕上的原点（窗口录制为 0,0）。纹理每次重建都会重算
    // bounds_，这里单独记住原点，避免被 EnsureResident 覆盖成 (0,0)。
    int originX_ = 0;
    int originY_ = 0;
    const char* errorKind_ = "none";
    bool cursorCapture_ = true;
    std::int32_t lastError_ = 0;
};

// 按 EnumDisplayMonitors 的枚举顺序返回包含该屏幕矩形中心的显示器序号；没有完全包含时
// 取与它重叠面积最大的那块，再不行返回 0。与 OpenMonitor 用的是同一套枚举顺序。
int WgcMonitorIndexForRect(const core::IntRect& screenRect);

}
