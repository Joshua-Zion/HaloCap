#include "d2d_font_rasterizer.h"

#include <d3d11.h>

#include <cmath>
#include <cwchar>
#include <utility>

namespace halocap::platform::win32 {

namespace {

constexpr float kLayoutBoxPx = 100000.0f;
constexpr int kCellPaddingPx = 1;

core::Rect CellRect(int x, int y, int width, int height) {
    return core::Rect{ static_cast<float>(x), static_cast<float>(y),
                       static_cast<float>(x + width), static_cast<float>(y + height) };
}

}

bool D2dFontRasterizer::Initialize() {
    lastHr_ = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf()));
    if (FAILED(lastHr_)) {
        return false;
    }

    wchar_t buffer[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH) == 0) {
        wcscpy_s(buffer, L"en-US");
    }
    locale_ = buffer;
    return true;
}

std::wstring D2dFontRasterizer::ToWide(std::string_view utf8) {
    if (utf8.empty()) {
        return std::wstring();
    }

    const int wideCount = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                              nullptr, 0);
    if (wideCount <= 0) {
        return std::wstring();
    }

    std::wstring wide(static_cast<std::size_t>(wideCount), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideCount);
    return wide;
}

D2dFontRasterizer::Page& D2dFontRasterizer::AcquirePage() {
    pages_.emplace_back();
    Page& page = pages_.back();

    ID3D11Device* device = backend_.Device();
    ID2D1DeviceContext* dc = backend_.Dc();
    if (device == nullptr || dc == nullptr) {
        lastHr_ = E_FAIL;
        return page;
    }

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = kPageSizePx;
    desc.Height = kPageSizePx;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    lastHr_ = device->CreateTexture2D(&desc, nullptr, &texture);
    if (FAILED(lastHr_)) {
        return page;
    }

    Microsoft::WRL::ComPtr<IDXGISurface> surface;
    lastHr_ = texture.As(&surface);
    if (FAILED(lastHr_)) {
        return page;
    }

    // 同一个表面做两张位图：填格子时当目标，画面上引用它时当绘制输入。
    D2D1_BITMAP_PROPERTIES1 targetProps{};
    targetProps.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    targetProps.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    targetProps.dpiX = 96.0f;
    targetProps.dpiY = 96.0f;
    targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

    lastHr_ = dc->CreateBitmapFromDxgiSurface(surface.Get(), &targetProps, &page.asTarget);
    if (FAILED(lastHr_)) {
        return page;
    }

    D2D1_BITMAP_PROPERTIES1 drawProps = targetProps;
    drawProps.bitmapOptions = D2D1_BITMAP_OPTIONS_NONE;

    lastHr_ = dc->CreateBitmapFromDxgiSurface(surface.Get(), &drawProps, &page.drawable);
    if (FAILED(lastHr_)) {
        return page;
    }

    page.handle = backend_.RegisterGpuImage(page.drawable.Get());
    return page;
}

bool D2dFontRasterizer::ReserveCell(Page& page, int cellWidth, int cellHeight, int* x, int* y) {
    if (cellWidth > kPageSizePx || cellHeight > kPageSizePx) {
        lastHr_ = E_FAIL;
        return false;
    }

    if (page.cursorX + cellWidth > kPageSizePx) {
        page.cursorX = 0;
        page.cursorY += page.rowHeight;
        page.rowHeight = 0;
    }

    if (page.cursorY + cellHeight > kPageSizePx) {
        lastHr_ = E_FAIL;
        return false;
    }

    *x = page.cursorX;
    *y = page.cursorY;
    page.cursorX += cellWidth;
    page.rowHeight = page.rowHeight > cellHeight ? page.rowHeight : cellHeight;
    return true;
}

D2dFontRasterizer::FormatEntry& D2dFontRasterizer::FormatFor(const core::FontSpec& spec) {
    for (FormatEntry& entry : formats_) {
        if (entry.spec == spec) {
            return entry;
        }
    }

    formats_.emplace_back();
    FormatEntry& entry = formats_.back();
    entry.spec = spec;

    const std::wstring family = ToWide(spec.family);
    lastHr_ = dwrite_->CreateTextFormat(family.c_str(),
                                       nullptr,
                                       static_cast<DWRITE_FONT_WEIGHT>(spec.weight),
                                       DWRITE_FONT_STYLE_NORMAL,
                                       DWRITE_FONT_STRETCH_NORMAL,
                                       spec.emSizePx,
                                       locale_.c_str(),
                                       &entry.format);
    if (FAILED(lastHr_)) {
        return entry;
    }

    const D2D1_COLOR_F color{ spec.color.r, spec.color.g, spec.color.b, spec.color.a };
    lastHr_ = backend_.Dc()->CreateSolidColorBrush(&color, nullptr, &entry.brush);
    return entry;
}

core::TextCell D2dFontRasterizer::LayOut(std::string_view text, const core::FontSpec& spec,
                                         core::Point origin) {
    for (const Entry& entry : cache_) {
        if (entry.spec == spec && entry.text == text) {
            const core::Rect destination{ origin.x, origin.y,
                                          origin.x + static_cast<float>(entry.cellWidth),
                                          origin.y + static_cast<float>(entry.cellHeight) };
            return core::TextCell{ pages_[entry.page].handle, entry.source, destination };
        }
    }

    core::TextCell invalid{};
    if (text.empty() || spec.emSizePx <= 0.0f) {
        return invalid;
    }

    FormatEntry& format = FormatFor(spec);
    if (FAILED(lastHr_) || !format.format || !format.brush) {
        return invalid;
    }

    const std::wstring wide = ToWide(text);
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    lastHr_ = dwrite_->CreateTextLayout(wide.c_str(), static_cast<UINT32>(wide.size()),
                                        format.format.Get(), kLayoutBoxPx, kLayoutBoxPx, &layout);
    if (FAILED(lastHr_)) {
        return invalid;
    }

    DWRITE_TEXT_METRICS metrics{};
    lastHr_ = layout->GetMetrics(&metrics);
    if (FAILED(lastHr_) || metrics.width <= 0.0f || metrics.height <= 0.0f) {
        return invalid;
    }

    const int cellWidth = static_cast<int>(std::ceil(metrics.width)) + kCellPaddingPx * 2;
    const int cellHeight = static_cast<int>(std::ceil(metrics.height)) + kCellPaddingPx * 2;

    if (pages_.empty() || pages_.back().handle == 0) {
        AcquirePage();
    }

    Page* page = &pages_.back();
    if (page->handle == 0) {
        lastHr_ = E_FAIL;
        return invalid;
    }

    int x = 0;
    int y = 0;
    if (!ReserveCell(*page, cellWidth, cellHeight, &x, &y)) {
        // 当前页放不下了：开新的一页，再试一次。
        page = &AcquirePage();
        if (page->handle == 0 || !ReserveCell(*page, cellWidth, cellHeight, &x, &y)) {
            return invalid;
        }
    }

    ID2D1DeviceContext* dc = backend_.Dc();
    dc->SetTarget(page->asTarget.Get());
    dc->BeginDraw();

    const core::Rect cell = CellRect(x, y, cellWidth, cellHeight);
    const D2D1_RECT_F clip{ cell.left, cell.top, cell.right, cell.bottom };
    dc->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    dc->DrawTextLayout(D2D1_POINT_2F{ static_cast<float>(x + kCellPaddingPx),
                                      static_cast<float>(y + kCellPaddingPx) },
                       layout.Get(), format.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
    dc->PopAxisAlignedClip();
    lastHr_ = dc->EndDraw();
    if (FAILED(lastHr_)) {
        return invalid;
    }

    cache_.push_back(Entry{ spec, std::string(text), static_cast<std::size_t>(page - pages_.data()),
                            cell, cellWidth, cellHeight });
    return core::TextCell{ page->handle, cell,
                           core::Rect{ origin.x, origin.y, origin.x + cellWidth, origin.y + cellHeight } };
}

}
