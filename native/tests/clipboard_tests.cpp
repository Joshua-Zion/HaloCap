// 剪贴板贴图：颜色解析、CF_HTML 取文本、快照优先级、文件批次与粘贴序列。
#include "clipboard.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace clip = halocap::core::clipboard;

int failures = 0;
int checks = 0;

void CheckTrue(bool actual, const char* what) {
    ++checks;
    if (!actual) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

void CheckEqual(long long actual, long long expected, const char* what) {
    ++checks;
    if (actual != expected) {
        ++failures;
        std::printf("FAIL %s: got %lld want %lld\n", what, actual, expected);
    }
}

void CheckColor(const clip::Color& actual, const clip::Color& expected, const char* what) {
    ++checks;
    if (!(actual == expected)) {
        ++failures;
        std::printf("FAIL %s: got %u,%u,%u,%u want %u,%u,%u,%u\n", what, actual.r, actual.g,
                    actual.b, actual.a, expected.r, expected.g, expected.b, expected.a);
    }
}

void TestIntegerColors() {
    for (int channel = 0; channel <= 255; ++channel) {
        const std::string text = std::to_string(channel) + " " +
                                 std::to_string(255 - channel) + " " + std::to_string(channel);
        clip::Color color;
        CheckTrue(clip::ParseColor(text, &color), "integer RGB parses");
        CheckColor(color, clip::Color{ static_cast<std::uint8_t>(channel),
                                       static_cast<std::uint8_t>(255 - channel),
                                       static_cast<std::uint8_t>(channel), 255 },
                   "integer channels stay in 0..255");
    }

    const char* variants[] = { "12 34 255", "12\t34\t255", "12,34,255", "12, 34, 255",
                               "12，34，255", "RGB 12 34 255", "rgb(12,34,255)",
                               "RgB: 12，34，255", " RGB：（12, 34, 255) " };
    for (const char* text : variants) {
        clip::Color color;
        const bool parsed = clip::ParseColor(text, &color);
        if (!parsed) {
            std::printf("DEBUG variant failed: %s\n", text);
        }
        CheckTrue(parsed, "RGB separator or prefix parses");
        CheckColor(color, clip::Color{ 12, 34, 255, 255 }, "separator must not change channels");
    }

    clip::Color small;
    CheckTrue(clip::ParseColor("0 1 1", &small), "small integers parse");
    CheckColor(small, clip::Color{ 0, 1, 1, 255 }, "integer-only RGB is not normalized");
}

void TestFractionalColors() {
    struct Case {
        const char* text;
        clip::Color expected;
    } cases[] = {
        { "0.0 0.5 1.0", { 0, 128, 255, 255 } },
        { "0, .5, 1", { 0, 128, 255, 255 } },
        { "RGB(0，.5，1)", { 0, 128, 255, 255 } },
        { "rgb: 1 0 .5", { 255, 0, 128, 255 } },
        { ".25 .75 0", { 64, 191, 0, 255 } },
        { "1. 0 1", { 255, 0, 255, 255 } },
        { "0.0 0 0", { 0, 0, 0, 255 } },
    };
    for (const Case& test : cases) {
        clip::Color color;
        CheckTrue(clip::ParseColor(test.text, &color), "fractional RGB parses");
        CheckColor(color, test.expected, "fractional channels round half away from zero");
    }
}

void TestHexColors() {
    struct Case {
        const char* text;
        clip::Color expected;
    } cases[] = {
        { "#123", { 0x11, 0x22, 0x33, 255 } },
        { "#1234", { 0x11, 0x22, 0x33, 0x44 } },
        { "#12aBcD", { 0x12, 0xAB, 0xCD, 255 } },
        { " #12aBcD80 ", { 0x12, 0xAB, 0xCD, 0x80 } },
        { "#F000", { 0xFF, 0x00, 0x00, 0x00 } },
        { "#FFFFFF00", { 0xFF, 0xFF, 0xFF, 0x00 } },
    };
    for (const Case& test : cases) {
        clip::Color color;
        CheckTrue(clip::ParseColor(test.text, &color), "CSS hex parses");
        CheckColor(color, test.expected, "CSS hex alpha belongs at the end");
    }
}

void TestAlphaColors() {
    struct Case {
        const char* text;
        clip::Color expected;
    } cases[] = {
        { "rgba(14, 165, 233, 0.5)", { 14, 165, 233, 128 } },
        { "RGBA(14,165,233,128)", { 14, 165, 233, 128 } },
        { "rgba(14 165 233 / 0.5)", { 14, 165, 233, 128 } },
        { "rgb(14,165,233,0.25)", { 14, 165, 233, 64 } },
        { "14 165 233 0.5", { 14, 165, 233, 128 } },
        { "rgba(14,165,233,1)", { 14, 165, 233, 255 } },
        { "rgba(14,165,233,0)", { 14, 165, 233, 0 } },
        { "rgba(14,165,233,255)", { 14, 165, 233, 255 } },
    };
    for (const Case& test : cases) {
        clip::Color color;
        CheckTrue(clip::ParseColor(test.text, &color), "RGB/RGBA alpha parses");
        CheckColor(color, test.expected, "alpha is the fourth component");
    }
}

void TestInvalidColors() {
    const char* invalid[] = { "#12", "#12345", "#1234567", "#123456789", "#GG0000", "red",
                              "普通文字", "256 0 0", "0 256 0", "0 0 256", "-1 0 0",
                              "0.5 1.01 0", "0.5 128 0", "NaN 0 0", "Infinity 0 0", "1e2 0 0",
                              "RGB(1,2)", "RGB(1,2,3", "rgba(1,2,3,1.5)", "rgba(1,2,3,256)",
                              "rgba(1,2,3,4,5)" };
    for (const char* text : invalid) {
        clip::Color color;
        CheckTrue(!clip::ParseColor(text, &color), "invalid color is rejected");
    }
}

void TestHtmlText() {
    const std::string fragment =
        "<head><title>HIDDEN HEAD</title><link href='x'></head>"
        "<script>HIDDEN SCRIPT</script><style>HIDDEN STYLE</style>"
        "<iframe src='x'>HIDDEN FRAME</iframe><object data='x'>HIDDEN OBJECT</object>"
        "<!--HIDDEN COMMENT-->"
        "<div>可见 &amp; &#x4E2D;&#25991;<br/>第二行 &lt;文字&gt;</div>"
        "<img src='x' alt='HIDDEN ALT'><img src='file:///x'>"
        "<table><tr><td>第三列</td><td>42</td></tr></table>";
    const std::string expected = "可见 & 中文\n第二行 <文字>\n第三列\t42";

    const std::string fragmentText = clip::HtmlToText(fragment);
    CheckTrue(fragmentText == expected, "HTML keeps only visible text");

    const std::string wrapped =
        "Version:1.0\r\nStartHTML:0000000000\r\n<html><body>OUTSIDE BEFORE<!--StartFragment-->" +
        fragment + "<!--EndFragment-->OUTSIDE AFTER</body></html>";
    CheckTrue(clip::HtmlToText(wrapped) == expected, "fragment markers win over outside content");

    // UTF-8 字节偏移的 CF_HTML：中文前缀让字节偏移与字符偏移不同。
    const std::string prefix = "<html><head><title>标题</title></head><body>前缀中文";
    const std::string body = "<div>片段中文 &amp; café<br>第二行</div>";
    const std::string suffix = "尾部不可见</body></html>";
    const std::string headerTemplate =
        "Version:1.0\r\nStartHTML:{0}\r\nEndHTML:{1}\r\nStartFragment:{2}\r\nEndFragment:{3}\r\n";
    const std::size_t headerLength = [&] {
        std::string probe = headerTemplate;
        probe.replace(probe.find("{0}"), 3, "0000000000");
        probe.replace(probe.find("{1}"), 3, "0000000000");
        probe.replace(probe.find("{2}"), 3, "0000000000");
        probe.replace(probe.find("{3}"), 3, "0000000000");
        return probe.size();
    }();
    const std::size_t startFragment = headerLength + prefix.size();
    const std::size_t endFragment = startFragment + body.size();
    const std::size_t endHtml = endFragment + suffix.size();

    char buffer[16] = {};
    const auto fixed = [&](std::size_t value) {
        std::snprintf(buffer, sizeof(buffer), "%010zu", value);
        return std::string(buffer);
    };
    std::string header = headerTemplate;
    header.replace(header.find("{0}"), 3, fixed(headerLength));
    header.replace(header.find("{1}"), 3, fixed(endHtml));
    header.replace(header.find("{2}"), 3, fixed(startFragment));
    header.replace(header.find("{3}"), 3, fixed(endFragment));

    const std::string html = header + prefix + body + suffix;
    CheckTrue(clip::HtmlToText(html) == "片段中文 & café\n第二行",
              "CF_HTML byte offsets are UTF-8 bytes, not characters");
}

void TestSnapshotPriority() {
    clip::SnapshotInput fileInput;
    fileInput.sequence = 30;
    fileInput.filePaths = { "C:\\a.png", " ", "C:\\b.txt" };
    fileInput.hasImage = true;
    fileInput.hasHtml = true;
    fileInput.html = "<p>HTML should lose</p>";
    fileInput.hasText = true;
    fileInput.text = "text should lose";
    const clip::Snapshot files = clip::NormalizeSnapshot(fileInput);
    CheckTrue(files.filePaths.size() == 2, "blank file entries are dropped");
    CheckTrue(!files.hasImage && files.text.empty(), "files outrank bitmap and text");
    CheckTrue(files.HasContent(), "files enable pinning");

    clip::SnapshotInput imageInput;
    imageInput.sequence = 19;
    imageInput.hasImage = true;
    imageInput.hasText = true;
    imageInput.text = "text must lose";
    const clip::Snapshot image = clip::NormalizeSnapshot(imageInput);
    CheckTrue(image.hasImage && image.text.empty(), "bitmap outranks text");

    clip::SnapshotInput htmlInput;
    htmlInput.sequence = 23;
    htmlInput.hasHtml = true;
    htmlInput.html = "<div>可见</div><script>hidden</script>";
    htmlInput.hasText = true;
    htmlInput.text = "lower priority";
    const clip::Snapshot html = clip::NormalizeSnapshot(htmlInput);
    CheckTrue(html.text == "可见", "HTML visible text outranks plain text");

    clip::SnapshotInput invisible;
    invisible.hasHtml = true;
    invisible.html = "<head>hidden</head><script>hidden</script><style>hidden</style>";
    CheckTrue(!clip::NormalizeSnapshot(invisible).HasContent(),
              "invisible HTML has no content");

    clip::SnapshotInput invisibleWithText;
    invisibleWithText.hasHtml = true;
    invisibleWithText.html = "<head>hidden</head>";
    invisibleWithText.hasText = true;
    invisibleWithText.text = "fallback text";
    CheckTrue(clip::NormalizeSnapshot(invisibleWithText).text == "fallback text",
              "invisible HTML falls back to plain text");

    clip::SnapshotInput nulText;
    nulText.hasText = true;
    nulText.text = "  第一行\r\n第二行 café  \0\0";
    const clip::Snapshot text = clip::NormalizeSnapshot(nulText);
    CheckTrue(text.text == "  第一行\r\n第二行 café  ", "trailing NULs are trimmed, content kept");

    clip::SnapshotInput blank;
    blank.hasText = true;
    blank.text = " \t\r\n\0\0";
    CheckTrue(!clip::NormalizeSnapshot(blank).HasContent(), "whitespace-only text has no content");
}

void TestPaths() {
    CheckTrue(clip::LooksLikeLocalAbsolutePath("C:\\a.png"), "drive path");
    CheckTrue(clip::LooksLikeLocalAbsolutePath("C:/a.png"), "forward slash drive path");
    CheckTrue(!clip::LooksLikeLocalAbsolutePath("\\\\server\\share\\a.png"), "UNC is rejected");
    CheckTrue(!clip::LooksLikeLocalAbsolutePath("relative.png"), "relative is rejected");
    CheckTrue(!clip::LooksLikeLocalAbsolutePath(""), "empty is rejected");
}

void TestPlanner() {
    clip::Snapshot twoFiles;
    twoFiles.sequence = 30;
    twoFiles.filePaths = { "C:\\one.png", "C:\\two.png" };

    clip::PinPlanner planner;
    const auto both = planner.PlanFiles(twoFiles, {}, [](const std::string&) { return true; });
    CheckTrue(both.size() == 2, "two images produce two pins");
    CheckTrue(both[0].kind == clip::PinKind::FileImage && both[0].hasImage, "file image pin");
    CheckTrue(both[0].text == "C:\\one.png", "file image keeps its path as text");

    const auto pathsOnly =
        planner.PlanFiles(twoFiles, clip::PinOptions{ false, 16 }, [](const std::string&) {
            return true;
        });
    CheckTrue(pathsOnly.size() == 1 && pathsOnly[0].kind == clip::PinKind::FilePath,
              "conversion off gives one path pin");
    CheckTrue(pathsOnly[0].text == "C:\\one.png\r\nC:\\two.png", "paths are joined with CRLF");

    // 17 个可解码文件：前 16 个是图片，第 17 个回退成路径。
    clip::Snapshot many;
    many.sequence = 33;
    for (int i = 0; i < 17; ++i) {
        many.filePaths.push_back("C:\\image" + std::to_string(i) + ".png");
    }
    const auto batch = planner.PlanFiles(many, {}, [](const std::string&) { return true; });
    CheckEqual(static_cast<long long>(batch.size()), 17, "16 images plus one path pin");
    int images = 0;
    for (const clip::PinPlan& pin : batch) {
        if (pin.kind == clip::PinKind::FileImage) {
            ++images;
        }
    }
    CheckEqual(images, 16, "file image pins are capped at 16");
    CheckTrue(batch.back().kind == clip::PinKind::FilePath, "the 17th file falls back to a path");

    // 粘贴序列：只有真正的 FileImage 粘贴才消耗。
    clip::PinPlanner sequencePlanner;
    const auto first = sequencePlanner.PlanFiles(twoFiles, {}, [](const std::string&) {
        return true;
    });
    CheckTrue(!sequencePlanner.FileImagePasteConsumed(twoFiles.sequence), "fresh sequence");
    sequencePlanner.MarkPasted(clip::PinPlan{ clip::PinKind::Text, "x", false, 30 });
    CheckTrue(!sequencePlanner.FileImagePasteConsumed(30), "text paste does not consume");
    sequencePlanner.MarkPasted(first[0]);
    CheckTrue(sequencePlanner.FileImagePasteConsumed(30), "file image paste consumes");
    const auto repeated =
        sequencePlanner.PlanFiles(twoFiles, {}, [](const std::string&) { return true; });
    CheckTrue(repeated.size() == 1 && repeated[0].kind == clip::PinKind::FilePath,
              "consumed sequence forces paths");
    clip::Snapshot next = twoFiles;
    next.sequence = 31;
    const auto fresh =
        sequencePlanner.PlanFiles(next, {}, [](const std::string&) { return true; });
    CheckTrue(fresh[0].kind == clip::PinKind::FileImage, "a new sequence converts again");
}

void TestContentPlan() {
    clip::PinPlanner planner;

    clip::Snapshot image;
    image.sequence = 19;
    image.hasImage = true;
    const auto images = planner.PlanContent(image);
    CheckTrue(images.size() == 1 && images[0].kind == clip::PinKind::Image, "bitmap is an image pin");

    clip::Snapshot colorText;
    colorText.sequence = 17;
    colorText.text = "#1234";
    const auto colors = planner.PlanContent(colorText);
    CheckTrue(colors.size() == 1 && colors[0].kind == clip::PinKind::Color, "color text is a color pin");
    CheckTrue(colors[0].text == "#1234", "color pin keeps the original text");

    clip::Snapshot ordinary;
    ordinary.text = "普通文字";
    const auto texts = planner.PlanContent(ordinary);
    CheckTrue(texts.size() == 1 && texts[0].kind == clip::PinKind::Text, "plain text is a text pin");

    clip::Snapshot blank;
    blank.text = "  ";
    CheckTrue(planner.PlanContent(blank).empty(), "blank text produces no pin");
}

void TestCardBounds() {
    const clip::TextCardMetrics small = clip::TextCardBounds("x", 1.0);
    CheckEqual(small.width, 160, "small text card width");
    CheckEqual(small.height, 56, "small text card height");

    const clip::TextCardMetrics large = clip::TextCardBounds(std::string(40000, 'A'), 2.0);
    CheckTrue(large.width <= 672 * 2, "card width is bounded");
    CheckTrue(large.height <= 2832 * 2, "card height is bounded");
}

}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    TestIntegerColors();
    TestFractionalColors();
    TestAlphaColors();
    TestHexColors();
    TestInvalidColors();
    TestHtmlText();
    TestSnapshotPriority();
    TestPaths();
    TestPlanner();
    TestContentPlan();
    TestCardBounds();

    if (failures != 0) {
        std::printf("CLIPBOARD_TESTS=FAILED %d/%d\n", failures, checks);
        return 1;
    }
    std::printf("CLIPBOARD_TESTS=OK checks=%d\n", checks);
    return 0;
}
