#include "win32_wgc_capture.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace halocap::platform::win32 {

namespace {

using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

const char* KindFor(HRESULT hr) {
    switch (hr) {
    case E_ACCESSDENIED:
        return "denied";
    case E_INVALIDARG:
    case E_POINTER:
        return "invalid";
    default:
        // WGC 在「窗口不支持 / 被保护 / 会话已结束」时给的都是这几个：
        // 0x80070005 已被上面拦掉，剩下按 hr 报，调用方据此降级。
        return "hr";
    }
}

GraphicsCaptureItem ItemForWindow(HWND window) {
    // WGC 的窗口入口在 interop 里，WinRT 投影没有直接暴露 CreateForWindow。
    auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(interop->CreateForWindow(
        window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
    return item;
}

winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice WrapDevice(ID3D11Device* device) {
    winrt::com_ptr<IDXGIDevice> dxgi;
    winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(dxgi.put())));

    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), inspectable.put()));
    return inspectable.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
}

winrt::com_ptr<ID3D11Texture2D> TextureFromSurface(
    const winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DSurface& surface) {
    // 这个接口在 interop 头里是经典 COM 接口（不在 winrt:: 命名空间里）。
    auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
    winrt::com_ptr<ID3D11Texture2D> texture;
    winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(texture.put())));
    return texture;
}

}  // namespace

struct Win32WgcCapture::State {
    GraphicsCaptureItem item{ nullptr };
    Direct3D11CaptureFramePool pool{ nullptr };
    GraphicsCaptureSession session{ nullptr };
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice device{ nullptr };
};

Win32WgcCapture::Win32WgcCapture(D2dRenderBackend& backend)
    : backend_(backend), state_(std::make_unique<State>()) {}

Win32WgcCapture::~Win32WgcCapture() { Close(); }

bool Win32WgcCapture::Open(int outputIndex) {
    // 窗口采集不是按输出序号来的：这条来源不支持 Open(index)。
    (void)outputIndex;
    errorKind_ = "invalid";
    return false;
}

bool Win32WgcCapture::StartItem(const winrt::Windows::Graphics::Capture::GraphicsCaptureItem& item,
                                POINT origin) {
    try {
        state_->item = item;
        state_->device = WrapDevice(backend_.Device());

        const auto size = state_->item.Size();
        state_->pool = Direct3D11CaptureFramePool::Create(
            state_->device, DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        state_->session = state_->pool.CreateCaptureSession(state_->item);

        // 光标：录制默认要带上（与 1.x 的设置项一致）。
        try {
            state_->session.IsCursorCaptureEnabled(cursorCapture_);
        } catch (...) {
        }
        // 黄色采集边框是 Win11 的隐私提示，能关就关；系统不支持时保持默认。
        try {
            state_->session.IsBorderRequired(false);
        } catch (...) {
        }

        state_->session.StartCapture();

        // 对外报的尺寸就是会被录下来的尺寸：奇数窗口按偶数额裁掉一像素（NV12 的要求），
        // 所以这里先裁，别让调用方拿奇数去开编码器再被拒。
        const int width = size.Width & ~1;
        const int height = size.Height & ~1;
        // 显示器录制要把显示器在屏幕上的原点带上：调用方的选区是屏幕坐标，编码器的裁剪
        // 是「源纹理内」坐标，多屏时用 (0,0) 作为原点会把副屏的选区算错。
        originX_ = origin.x;
        originY_ = origin.y;
        bounds_ = core::IntRect{ origin.x, origin.y, origin.x + width, origin.y + height };
        widthPx_ = width;
        heightPx_ = height;
        errorKind_ = "ok";
        return true;
    } catch (const winrt::hresult_error& error) {
        lastError_ = static_cast<std::int32_t>(error.code().value);
        errorKind_ = KindFor(error.code().value);
        Close();
        return false;
    } catch (...) {
        lastError_ = 0;
        errorKind_ = "hr";
        Close();
        return false;
    }
}

bool Win32WgcCapture::OpenWindow(core::NativeHandle window) {
    if (window == nullptr) {
        errorKind_ = "invalid";
        return false;
    }

    try {
        // 同一线程重复调用会抛 RPC_E_CHANGED_MODE / RPC_E_INVALID_STATE；宿主已经初始化过
        // WinRT 时那不是错误。
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    try {
        if (!GraphicsCaptureSession::IsSupported()) {
            errorKind_ = "notsupported";
            return false;
        }
        return StartItem(ItemForWindow(static_cast<HWND>(window)));
    } catch (const winrt::hresult_error& error) {
        lastError_ = static_cast<std::int32_t>(error.code().value);
        errorKind_ = KindFor(error.code().value);
        Close();
        return false;
    } catch (...) {
        lastError_ = 0;
        errorKind_ = "hr";
        Close();
        return false;
    }
}

bool Win32WgcCapture::OpenMonitor(int outputIndex) {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }

    try {
        if (!GraphicsCaptureSession::IsSupported()) {
            errorKind_ = "notsupported";
            return false;
        }

        struct MonitorPick {
            int target = 0;
            int index = 0;
            HMONITOR monitor = nullptr;
        } pick{ outputIndex, 0, nullptr };
        EnumDisplayMonitors(
            nullptr, nullptr,
            [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
                auto* state = reinterpret_cast<MonitorPick*>(param);
                if (state->index == state->target) {
                    state->monitor = monitor;
                    return FALSE;
                }
                ++state->index;
                return TRUE;
            },
            reinterpret_cast<LPARAM>(&pick));
        if (pick.monitor == nullptr) {
            errorKind_ = "invalid";
            return false;
        }

        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        POINT origin{ 0, 0 };
        if (GetMonitorInfoW(pick.monitor, &monitorInfo)) {
            origin = POINT{ monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top };
        }

        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        GraphicsCaptureItem item{ nullptr };
        winrt::check_hresult(interop->CreateForMonitor(
            pick.monitor, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(item)));
        return StartItem(item, origin);
    } catch (const winrt::hresult_error& error) {
        lastError_ = static_cast<std::int32_t>(error.code().value);
        errorKind_ = KindFor(error.code().value);
        Close();
        return false;
    } catch (...) {
        lastError_ = 0;
        errorKind_ = "hr";
        Close();
        return false;
    }
}
void Win32WgcCapture::Close() {
    try {
        if (state_->session) {
            state_->session.Close();
            state_->session = nullptr;
        }
        if (state_->pool) {
            state_->pool.Close();
            state_->pool = nullptr;
        }
    } catch (...) {
    }
    state_->item = nullptr;
    state_->device = nullptr;
    ReleaseResident();
}

void Win32WgcCapture::ReleaseResident() {
    if (residentHandle_ != 0) {
        backend_.ReleaseImage(residentHandle_);
        residentHandle_ = 0;
    }
    resident_.Reset();
    widthPx_ = 0;
    heightPx_ = 0;
}

bool Win32WgcCapture::EnsureResident(int widthPx, int heightPx) {
    if (resident_ && widthPx == widthPx_ && heightPx == heightPx_) {
        return true;
    }

    ReleaseResident();

    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(widthPx);
    description.Height = static_cast<UINT>(heightPx);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    // 编码器读它（ShaderResource），WGC 往它拷（CopyResource 只要求目标不是 staging）。
    // RenderTarget 是 D2D 从表面建位图的前置条件——上屏路径记过同一条实测。
    description.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    const HRESULT createHr = backend_.Device()->CreateTexture2D(&description, nullptr, &resident_);
    if (FAILED(createHr)) {
        lastError_ = static_cast<std::int32_t>(createHr);
        errorKind_ = "hr";
        return false;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    const HRESULT surfaceHr = resident_.As(&surface);
    if (FAILED(surfaceHr)) {
        lastError_ = static_cast<std::int32_t>(surfaceHr);
        errorKind_ = "hr";
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 properties{};
    properties.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE);
    // 这张纹理只被采样（编码器读），不往它上面画：NONE 就是「可采样」。
    properties.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

    Microsoft::WRL::ComPtr<ID2D1Bitmap1> bitmap;
    HRESULT bitmapHr = backend_.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &properties,
                                                                  &bitmap);
    if (FAILED(bitmapHr)) {
        // 有的驱动不接受「RT 纹理 + NONE」，退回 TARGET（实测路径，§5.1 上屏那一节）。
        properties.bitmapOptions =
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        bitmap.Reset();
        bitmapHr = backend_.Dc()->CreateBitmapFromDxgiSurface(surface.Get(), &properties, &bitmap);
    }
    if (FAILED(bitmapHr)) {
        lastError_ = static_cast<std::int32_t>(bitmapHr);
        errorKind_ = "hr";
        return false;
    }

    residentHandle_ = backend_.RegisterGpuImage(bitmap.Get());
    if (residentHandle_ == 0) {
        lastError_ = static_cast<std::int32_t>(backend_.LastHr());
        errorKind_ = "hr";
        return false;
    }

    widthPx_ = widthPx;
    heightPx_ = heightPx;
    bounds_ = core::IntRect{ originX_, originY_, originX_ + widthPx, originY_ + heightPx };
    return true;
}

bool Win32WgcCapture::WaitForUpdate(std::int64_t timeoutMs, core::FrameUpdate& out) {
    if (!state_->pool) {
        errorKind_ = "invalid";
        return false;
    }

    Direct3D11CaptureFrame frame{ nullptr };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(std::max<std::int64_t>(0, timeoutMs));

    // TryGetNextFrame 是非阻塞的：WGC 没有事件可等，只能轮询。窗口静止时这里会
    // 按时超时，与桌面复制的「没有新帧」是同一种「不是错误」的分支。
    while (true) {
        try {
            frame = state_->pool.TryGetNextFrame();
        } catch (const winrt::hresult_error& error) {
            lastError_ = static_cast<std::int32_t>(error.code().value);
            errorKind_ = KindFor(error.code().value);
            return false;
        }

        if (frame) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            errorKind_ = "timeout";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    try {
        const auto texture = TextureFromSurface(frame.Surface());
        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);

        // 窗口内容是奇数尺寸是常态（946×533 这种）。NV12 的 4:2:0 划分不允许奇数，
        // 所以这里按偶数裁掉最右/最下一像素，而不是把「录不了」丢给调用方。
        const int width = static_cast<int>(description.Width) & ~1;
        const int height = static_cast<int>(description.Height) & ~1;
        if (width < 2 || height < 2) {
            frame.Close();
            errorKind_ = "invalid";
            return false;
        }

        if (!EnsureResident(width, height)) {
            frame.Close();
            return false;
        }

        D3D11_BOX box{};
        box.left = 0;
        box.top = 0;
        box.front = 0;
        box.right = static_cast<UINT>(width);
        box.bottom = static_cast<UINT>(height);
        box.back = 1;
        backend_.D3dContext()->CopySubresourceRegion(resident_.Get(), 0, 0, 0, 0,
                                                     texture.get(), 0, &box);
        backend_.D3dContext()->Flush();

        out = core::FrameUpdate{};
        out.timestampUs = frame.SystemRelativeTime().count() / 10;   // 100ns → µs
        out.accumulatedFrames = 1;
        out.contentChanged = true;
        out.target = residentHandle_;
        out.widthPx = widthPx_;
        out.heightPx = heightPx_;
        out.desktopBounds = bounds_;
        // WGC 没有脏矩形：一帧就是整窗。
        out.dirtyRects.assign(1, core::IntRect{ 0, 0, widthPx_, heightPx_ });

        frame.Close();
        errorKind_ = "ok";
        return true;
    } catch (const winrt::hresult_error& error) {
        lastError_ = static_cast<std::int32_t>(error.code().value);
        errorKind_ = KindFor(error.code().value);
        return false;
    }
}

int WgcMonitorIndexForRect(const core::IntRect& screenRect) {
    const int centerX = (screenRect.left + screenRect.right) / 2;
    const int centerY = (screenRect.top + screenRect.bottom) / 2;

    // 第一遍：中心点落在哪块显示器里就用哪块（与 OpenMonitor 同一个枚举顺序）。
    struct CenterPick {
        int x = 0;
        int y = 0;
        int index = 0;
        int found = -1;
    } center{ centerX, centerY, 0, -1 };
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<CenterPick*>(param);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info) && state->x >= info.rcMonitor.left &&
                state->x < info.rcMonitor.right && state->y >= info.rcMonitor.top &&
                state->y < info.rcMonitor.bottom) {
                state->found = state->index;
                return FALSE;
            }
            ++state->index;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&center));
    if (center.found >= 0) {
        return center.found;
    }

    // 中心点落在显示器间隙时，取与选区重叠面积最大的一块；都没有就退回 0。
    struct AreaPick {
        core::IntRect rect{};
        int index = 0;
        int best = 0;
        long long bestArea = 0;
    } area{ screenRect, 0, 0, 0 };
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
            auto* state = reinterpret_cast<AreaPick*>(param);
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            if (GetMonitorInfoW(monitor, &info)) {
                const int left = std::max(state->rect.left, static_cast<int>(info.rcMonitor.left));
                const int top = std::max(state->rect.top, static_cast<int>(info.rcMonitor.top));
                const int right = std::min(state->rect.right, static_cast<int>(info.rcMonitor.right));
                const int bottom =
                    std::min(state->rect.bottom, static_cast<int>(info.rcMonitor.bottom));
                const long long width = right - left;
                const long long height = bottom - top;
                const long long overlap = (width > 0 && height > 0) ? width * height : 0;
                if (overlap > state->bestArea) {
                    state->bestArea = overlap;
                    state->best = state->index;
                }
            }
            ++state->index;
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&area));
    return area.best;
}

}
