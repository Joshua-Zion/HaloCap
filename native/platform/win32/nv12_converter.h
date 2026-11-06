#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

namespace halocap::platform::win32 {

// BGRA → NV12 必须留在 GPU 上：spike 1 实测 BGRA 样本会被写入器一路接受，到 Finalize
// 才失败并留下 0 字节文件，失败点离原因很远，所以转换是 2.0 自己的责任。
// 用 D3D11 视频处理器做：不用写 shader，也不产生一次额外拷贝。
class Nv12Converter {
public:
    // 输出纹理环的深度。环绕复用要等编码器读完上一帧才安全，spike 1 用的就是 3；
    // 「读完了没有」这条还没有独立证明（见 §5.1 编码 seam 的未决项）。
    static constexpr int kPoolDepth = 3;

    bool Initialize(ID3D11Device* device, ID3D11DeviceContext* context);

    HRESULT LastHr() const { return lastHr_; }
    const char* LastErrorKind() const { return errorKind_; }

    // 只在尺寸变化时重建；帧循环里不分配。
    bool EnsureTargets(int widthPx, int heightPx);

    // 环绕取下一个写入槽位。
    int AcquireSlot();
    ID3D11Texture2D* SlotTexture(int index) const;
    int PoolWidth() const { return poolWidth_; }
    int PoolHeight() const { return poolHeight_; }

    // 整幅转换，写进指定槽位。destination 用 SlotTexture(index) 拿。
    bool ConvertToSlot(ID3D11Texture2D* bgraSource, int slotIndex);

    // 不设就用驱动默认；探针要把默认与显式两种都量一遍再决定钉哪个。
    void SetInputColorSpace(const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& colorSpace);
    void SetOutputColorSpace(const D3D11_VIDEO_PROCESSOR_COLOR_SPACE& colorSpace);
    void UseDefaultColorSpaces();

    // NV12 输出的能力只能问设备（INPUT/OUTPUT 两个位）。真正决定可用的是建输出视图会不会失败，
    // 所以 EnsureTargets 是按候选 bind flag 试出来的，这里带出实际用了哪一组。
    bool SupportsOutputFormat(UINT* supportFlags) const;
    UINT BindFlagsUsed() const { return bindFlagsUsed_; }

    // 每一组候选的失败点：纹理建不成还是输出视图建不成，是两个完全不同的原因。
    // 没有这份记录，「全都返回 E_INVALIDARG」就只是一个数字而不是一个答案。
    struct BindAttempt {
        UINT bindFlags = 0;
        HRESULT textureHr = S_OK;
        HRESULT viewHr = S_OK;
    };

    const std::vector<BindAttempt>& BindAttempts() const { return attempts_; }
    UINT Nv12SupportFlags() const { return nv12Support_; }
    UINT BgraSupportFlags() const { return bgraSupport_; }

    // 读回一块 NV12 用于校验颜色。luma 高 height、chroma 高 height/2，各自按返回的 pitch 排。
    bool ReadBack(ID3D11Texture2D* nv12, std::vector<BYTE>& luma, std::vector<BYTE>& chroma,
                  int heightPx, UINT* lumaPitch, UINT* chromaPitch);

private:
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11VideoProcessorOutputView> view;
    };

    bool EnsureProcessor(int widthPx, int heightPx);
    bool EnsureInputView(ID3D11Texture2D* bgraSource);
    bool CreateSlot(Slot& slot, UINT bindFlags, int widthPx, int heightPx, BindAttempt* attempt);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VideoDevice> videoDevice_;
    Microsoft::WRL::ComPtr<ID3D11VideoContext> videoContext_;

    Microsoft::WRL::ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessor> processor_;

    // 常驻的那一块 BGRA 采集纹理：输入视图按纹理指针缓存，每帧重建视图是白给的成本。
    Microsoft::WRL::ComPtr<ID3D11Texture2D> inputTexture_;
    Microsoft::WRL::ComPtr<ID3D11VideoProcessorInputView> inputView_;

    std::vector<Slot> slots_;
    int poolWidth_ = 0;
    int poolHeight_ = 0;
    int nextSlot_ = 0;

    bool hasInputColorSpace_ = false;
    bool hasOutputColorSpace_ = false;
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE inputColorSpace_{};
    D3D11_VIDEO_PROCESSOR_COLOR_SPACE outputColorSpace_{};

    UINT bindFlagsUsed_ = 0;
    std::vector<BindAttempt> attempts_;
    UINT nv12Support_ = 0;
    UINT bgraSupport_ = 0;
    HRESULT lastHr_ = S_OK;
    const char* errorKind_ = "none";
};

}
