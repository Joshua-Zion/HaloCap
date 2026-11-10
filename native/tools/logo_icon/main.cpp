// 生成应用图标 .ico：与运行期的托盘/窗口图标同一套渲染（win32_tray 里的 RenderLogoPixels）。
// 用法：logo_icon <out.ico>
// 生成的是 BMP（32bpp）+ AND 掩码形式的多尺寸 ICO，16..256 都能被资源管理器/任务栏挑到。
#include "win32_tray.h"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

void AppendLe16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendLe32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
    }
}

struct Entry {
    int size = 0;
    std::vector<std::uint8_t> payload;
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: logo_icon <out.ico>\n");
        return 2;
    }

    const int sizes[] = { 16, 24, 32, 48, 64, 128, 256 };
    std::vector<Entry> entries;
    entries.reserve(sizeof(sizes) / sizeof(sizes[0]));

    for (const int size : sizes) {
        const std::vector<std::uint8_t> pixels =
            halocap::platform::win32::RenderHalocapLogoBgra(size, false);
        if (pixels.size() != static_cast<std::size_t>(size) * size * 4) {
            std::printf("render failed at %d\n", size);
            return 3;
        }

        Entry entry;
        entry.size = size;
        // BITMAPINFOHEADER
        AppendLe32(entry.payload, 40);
        AppendLe32(entry.payload, static_cast<std::uint32_t>(size));       // biWidth
        AppendLe32(entry.payload, static_cast<std::uint32_t>(size * 2));   // XOR + AND
        AppendLe16(entry.payload, 1);                                     // planes
        AppendLe16(entry.payload, 32);                                    // bpp
        AppendLe32(entry.payload, 0);                                     // BI_RGB
        AppendLe32(entry.payload, static_cast<std::uint32_t>(size * size * 4));
        AppendLe32(entry.payload, 0);   // x ppm
        AppendLe32(entry.payload, 0);   // y ppm
        AppendLe32(entry.payload, 0);   // clr used
        AppendLe32(entry.payload, 0);   // clr important

        // XOR：自下而上的 BGRA。
        for (int y = size - 1; y >= 0; --y) {
            const std::uint8_t* row = &pixels[static_cast<std::size_t>(y) * size * 4];
            entry.payload.insert(entry.payload.end(), row, row + static_cast<std::size_t>(size) * 4);
        }

        // AND 掩码：1bpp、行按 4 字节对齐、自下而上；透明像素置 1。
        const int maskStride = ((size + 31) / 32) * 4;
        std::vector<std::uint8_t> mask(static_cast<std::size_t>(maskStride) * size, 0);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const std::uint8_t alpha =
                    pixels[(static_cast<std::size_t>(y) * size + x) * 4 + 3];
                if (alpha < 128) {
                    mask[static_cast<std::size_t>(y) * maskStride + x / 8] |=
                        static_cast<std::uint8_t>(1u << (7 - (x % 8)));
                }
            }
        }
        for (int y = size - 1; y >= 0; --y) {
            entry.payload.insert(entry.payload.end(),
                                 mask.begin() + static_cast<std::ptrdiff_t>(y) * maskStride,
                                 mask.begin() +
                                     static_cast<std::ptrdiff_t>(y + 1) * maskStride);
        }

        entries.push_back(std::move(entry));
    }

    std::vector<std::uint8_t> file;
    AppendLe16(file, 0);   // reserved
    AppendLe16(file, 1);   // type icon
    AppendLe16(file, static_cast<std::uint16_t>(entries.size()));

    std::uint32_t offset = 6 + 16 * static_cast<std::uint32_t>(entries.size());
    for (const Entry& entry : entries) {
        file.push_back(entry.size >= 256 ? 0 : static_cast<std::uint8_t>(entry.size));
        file.push_back(entry.size >= 256 ? 0 : static_cast<std::uint8_t>(entry.size));
        file.push_back(0);   // color count
        file.push_back(0);   // reserved
        AppendLe16(file, 1);   // planes
        AppendLe16(file, 32);  // bpp
        AppendLe32(file, static_cast<std::uint32_t>(entry.payload.size()));
        AppendLe32(file, offset);
        offset += static_cast<std::uint32_t>(entry.payload.size());
    }
    for (const Entry& entry : entries) {
        file.insert(file.end(), entry.payload.begin(), entry.payload.end());
    }

    FILE* output = nullptr;
    if (fopen_s(&output, argv[1], "wb") != 0 || output == nullptr) {
        std::printf("cannot write %s\n", argv[1]);
        return 4;
    }
    std::fwrite(file.data(), 1, file.size(), output);
    std::fclose(output);
    std::printf("wrote %s (%zu bytes, %zu sizes)\n", argv[1], file.size(), entries.size());
    return 0;
}
