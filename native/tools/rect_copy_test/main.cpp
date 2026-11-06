// CopyDirtyRegions 的功能验证：不需要复制权，所以任何机器上都能跑，也不需要动屏幕。
// 它证的是 §4 的核心承诺——「只搬变化区，脏区之外一律不动」，以及移动矩形时退化成整帧。
#include "dxgi_desktop_duplication.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using halocap::platform::win32::CopyDirtyRegions;

namespace {

constexpr int kWidth = 64;
constexpr int kHeight = 32;
constexpr int kBytes = kWidth * kHeight * 4;

constexpr unsigned char kOldByte = 0x11;

unsigned char SourceByte(int x, int y, int channel) {
    return static_cast<unsigned char>((x * 3 + y * 5 + channel * 17) & 0xFF);
}

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

ComPtr<ID3D11Texture2D> MakeTexture(ID3D11Device* device, bool staging, const void* data) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kWidth;
    desc.Height = kHeight;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = staging ? D3D11_USAGE_STAGING : D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags = staging ? D3D11_CPU_ACCESS_READ : 0;

    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    initial.SysMemPitch = kWidth * 4;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, data ? &initial : nullptr, &texture))) {
        return ComPtr<ID3D11Texture2D>();
    }
    return texture;
}

// 读回一份可比较的副本。行距由驱动决定，不能假设等于宽×4。
bool ReadBack(ID3D11DeviceContext* context, ID3D11Texture2D* destination,
              ID3D11Texture2D* staging, std::vector<unsigned char>& out) {
    context->CopyResource(staging, destination);
    context->Flush();

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) {
        return false;
    }

    out.assign(kBytes, 0);
    const auto* bits = reinterpret_cast<const unsigned char*>(mapped.pData);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t from = static_cast<std::size_t>(mapped.RowPitch) * y +
                                     static_cast<std::size_t>(x) * 4;
            const std::size_t to = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                out[to + channel] = bits[from + channel];
            }
        }
    }

    context->Unmap(staging, 0);
    return true;
}

bool MatchesSource(const std::vector<unsigned char>& pixels, int x, int y) {
    const std::size_t base = (static_cast<std::size_t>(y) * kWidth + x) * 4;
    for (int channel = 0; channel < 4; ++channel) {
        if (pixels[base + channel] != SourceByte(x, y, channel)) {
            return false;
        }
    }
    return true;
}

bool IsOld(const std::vector<unsigned char>& pixels, int x, int y) {
    const std::size_t base = (static_cast<std::size_t>(y) * kWidth + x) * 4;
    for (int channel = 0; channel < 4; ++channel) {
        if (pixels[base + channel] != kOldByte) {
            return false;
        }
    }
    return true;
}

int CountOld(const std::vector<unsigned char>& pixels) {
    int count = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            count += IsOld(pixels, x, y) ? 1 : 0;
        }
    }
    return count;
}

}

int main() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) {
        // 没有可用显卡时退回 WARP：这条路径的正确性与显卡无关。
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context);
    }
    if (FAILED(hr)) {
        std::printf("DEVICE=FAILED hr=0x%08lX\n", static_cast<unsigned long>(hr));
        return 2;
    }

    std::vector<unsigned char> sourcePixels(kBytes);
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t base = (static_cast<std::size_t>(y) * kWidth + x) * 4;
            for (int channel = 0; channel < 4; ++channel) {
                sourcePixels[base + channel] = SourceByte(x, y, channel);
            }
        }
    }

    std::vector<unsigned char> oldPixels(kBytes, kOldByte);

    const ComPtr<ID3D11Texture2D> source = MakeTexture(device.Get(), false, sourcePixels.data());
    const ComPtr<ID3D11Texture2D> destination = MakeTexture(device.Get(), false, oldPixels.data());
    const ComPtr<ID3D11Texture2D> staging = MakeTexture(device.Get(), true, nullptr);

    if (!source || !destination || !staging) {
        std::printf("TEXTURES=FAILED\n");
        return 2;
    }

    std::vector<unsigned char> readBack;

    // 一个脏矩形：里面换成新内容，外面一个字节都不许动。
    {
        const RECT rects[] = { { 4, 8, 20, 24 } };
        bool fallback = false;
        CopyDirtyRegions(context.Get(), destination.Get(), source.Get(), rects, 1, 0, fallback);

        Check(!fallback, "no move rects means no fallback");
        Check(ReadBack(context.Get(), destination.Get(), staging.Get(), readBack), "read back ok");
        Check(MatchesSource(readBack, 10, 15), "dirty area got the new content");
        Check(MatchesSource(readBack, 4, 8), "dirty area top-left corner included");
        Check(IsOld(readBack, 2, 15), "left of the dirty rect untouched");
        Check(IsOld(readBack, 22, 15), "right of the dirty rect untouched");
        Check(IsOld(readBack, 10, 6), "above the dirty rect untouched");
        Check(IsOld(readBack, 10, 25), "below the dirty rect untouched");
    }

    // 第二个脏矩形：两块都更新，第三块仍旧。
    {
        const RECT rects[] = { { 40, 0, 48, 4 }, { 50, 28, 60, 32 } };
        bool fallback = false;
        CopyDirtyRegions(context.Get(), destination.Get(), source.Get(), rects, 2, 0, fallback);

        Check(ReadBack(context.Get(), destination.Get(), staging.Get(), readBack), "read back ok 2");
        Check(MatchesSource(readBack, 44, 2), "second dirty rect updated");
        Check(MatchesSource(readBack, 55, 30), "third dirty rect updated");
        Check(IsOld(readBack, 44, 10), "gap between rects untouched");
    }

    // 零面积矩形必须整个跳过。
    {
        const RECT rects[] = { { 30, 10, 30, 20 }, { 5, 5, 9, 5 } };
        bool fallback = false;
        const int oldBefore = CountOld(readBack);
        CopyDirtyRegions(context.Get(), destination.Get(), source.Get(), rects, 2, 0, fallback);

        Check(ReadBack(context.Get(), destination.Get(), staging.Get(), readBack), "read back ok 3");
        Check(CountOld(readBack) == oldBefore, "zero-area rects changed nothing");
    }

    // 有移动矩形时退化成整帧拷贝。
    {
        const RECT rects[] = { { 4, 8, 20, 24 } };
        bool fallback = false;
        CopyDirtyRegions(context.Get(), destination.Get(), source.Get(), rects, 1, 3, fallback);

        Check(fallback, "move rects set the fallback flag");
        Check(ReadBack(context.Get(), destination.Get(), staging.Get(), readBack), "read back ok 4");
        Check(CountOld(readBack) == 0, "fallback copied the whole frame");
        Check(MatchesSource(readBack, 0, 0) && MatchesSource(readBack, 63, 31),
              "every pixel matches the source after fallback");
    }

    if (failures != 0) {
        std::printf("RECT_COPY_TESTS=FAILED %d\n", failures);
        return 1;
    }

    std::printf("RECT_COPY_TESTS=OK\n");
    return 0;
}
