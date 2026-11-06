#pragma once

#include "d2d_render_backend.h"
#include "font_rasterizer.h"

#include <dwrite.h>

#include <string>
#include <vector>

namespace halocap::platform::win32 {

// IFontRasterizer 的 DirectWrite 实现：每个「字体+字重+字号+颜色」一组图集页，
// 一个文本串占一格，栅格就在 GPU 上完成（画进页纹理），之后显示列表里只是普通 blit。
//
// 为什么不是「一个字形一格」：这个 SDK 的 IDWriteTextLayout 不对外暴露字形序列，
// 拿字形只能走已废弃的 IDWriteTextAnalyzer，那等于自己重做一遍排版。
class D2dFontRasterizer final : public core::IFontRasterizer {
public:
    static constexpr int kPageSizePx = 1024;

    explicit D2dFontRasterizer(D2dRenderBackend& backend) : backend_(backend) {}

    bool Initialize();

    HRESULT LastHr() const { return lastHr_; }

    int PageCount() const { return static_cast<int>(pages_.size()); }

    core::TextCell LayOut(std::string_view text, const core::FontSpec& spec, core::Point origin) override;

private:
    struct Page {
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> asTarget;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> drawable;
        core::ImageHandle handle = 0;
        int cursorX = 0;
        int cursorY = 0;
        int rowHeight = 0;
    };

    struct Entry {
        core::FontSpec spec;
        std::string text;
        std::size_t page = 0;
        core::Rect source;
        int cellWidth = 0;
        int cellHeight = 0;
    };

    struct FormatEntry {
        core::FontSpec spec;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    };

    Page& AcquirePage();

    bool ReserveCell(Page& page, int cellWidth, int cellHeight, int* x, int* y);

    FormatEntry& FormatFor(const core::FontSpec& spec);

    static std::wstring ToWide(std::string_view utf8);

    D2dRenderBackend& backend_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    // CreateTextFormat 的 localeName 是必填（_In_z_），传 nullptr 直接 E_INVALIDARG。
    std::wstring locale_;
    std::vector<Page> pages_;
    std::vector<Entry> cache_;
    std::vector<FormatEntry> formats_;
    HRESULT lastHr_ = S_OK;
};

}
