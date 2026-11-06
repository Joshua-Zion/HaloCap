#pragma once

#include "d2d_render_backend.h"
#include "encoder.h"
#include "nv12_converter.h"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <string>

namespace halocap::platform::win32 {

// IEncoder 的 Media Foundation 实现。整条序列是 spike 1 量出来的，不是照着文档拼的：
//   · 入口只能是 IMFSinkWriter + MF_SINK_WRITER_D3D_MANAGER（裸 MFT 会被异步协议拒掉）；
//   · 样本必须是 NV12，且 MFCreateDXGISurfaceBuffer 之后要显式 SetCurrentLength，
//     否则每帧 E_INVALIDARG、Finalize 报 MF_E_SINK_NO_SAMPLES_PROCESSED、文件 0 字节；
//   · 注册进 DXGI 管理器的设备决定用哪一家的编码器，所以这里只能用渲染后端那台设备。
class MfH264Encoder final : public core::IEncoder {
public:
    explicit MfH264Encoder(D2dRenderBackend& backend);

    ~MfH264Encoder() override;

    // COM + MFStartup。进程一次，重复调用直接返回 true。
    static bool StartupFoundation();
    static void ShutdownFoundation();

    // 建 DXGI 设备管理器并把后端的 D3D11 设备交出去，同时初始化 NV12 转换器。
    bool Initialize();

    bool OpenVideo(const core::VideoEncoderConfig& config, std::string_view outputPathUtf8) override;

    bool Submit(const core::EncodedFrame& frame) override;

    bool Close(core::EncoderStats& outStats) override;

    const char* LastErrorKind() const override { return errorKind_; }
    std::int32_t LastErrorCode() const override { return static_cast<std::int32_t>(lastHr_); }

    // ---- 只给探针用 ----

    // NV12 输出环实际用的 bind flag（候选是逐个试出来的，见 Nv12Converter）。
    UINT Nv12BindFlags() const { return converter_.BindFlagsUsed(); }

    Nv12Converter& Converter() { return converter_; }
    ID3D11Texture2D* Nv12Slot(int index) const { return converter_.SlotTexture(index); }

    // 输入类型最终被接受的那个属性变体：0 写全（stride+sampleSize）、1 只写 stride、2 都不写。
    // 每家编码器要的不一样，所以这不是设计选择而是试出来的答案。
    int InputTypeVariant() const { return inputTypeVariant_; }

    // 「能不能问出编码器身份」这条要靠实测留档：
    // 在 SetInputMediaType/BeginWriting 之前问会得到 MF_E_UNSUPPORTED_SERVICE（spike 1 就是在这里
    // 得到「问不出来」的结论），协商完之后同一个调用返回 S_OK。
    HRESULT ProbeServiceQuery();
    HRESULT ChosenTransform(IMFTransform** outTransform);

    // 用给定色彩空间设置转换（探针要量默认与显式两种）。
    void UseColorSpaces(bool withExplicitRanges);

    // MF_SINK_WRITER_DISABLE_THROTTLING。默认不关：关掉之后写入器不再按时间轴等帧，
    // 探针用它量裸吞吐，但代价是未节流的快推会让写入器自己缓冲（§5.1 编码 seam 有数字）。
    void SetThrottleDisabled(bool disabled) { throttleDisabled_ = disabled; }

    // 观测到的硬件判据附带读出来的厂商标识，例如 "VEN_8086"；软件编码器为空。
    const std::string& ChosenEncoderVendor() const { return vendor_; }

private:
    bool BuildOutputType(IMFMediaType** outType);
    bool BuildInputType(int variant, IMFMediaType** outType);

    // 读服务上的属性，把「实际用了哪一家」写进 vendor_ / hardwareObserved_。
    void ObserveChosenEncoder();

    static std::wstring ToWide(std::string_view utf8);

    D2dRenderBackend& backend_;
    Nv12Converter converter_;

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    UINT resetToken_ = 0;

    Microsoft::WRL::ComPtr<IMFSinkWriter> writer_;
    Microsoft::WRL::ComPtr<IMFByteStream> byteStream_;
    DWORD streamIndex_ = 0;

    core::VideoEncoderConfig config_{};
    std::string lastPathUtf8_;
    std::string vendor_;
    bool hardwareObserved_ = false;
    bool throttleDisabled_ = false;
    int inputTypeVariant_ = -1;
    std::int64_t accepted_ = 0;
    std::int64_t rejected_ = 0;
    std::int64_t lastTimeUs_ = -1;
    bool writing_ = false;

    HRESULT lastHr_ = S_OK;
    const char* errorKind_ = "none";
};

}
