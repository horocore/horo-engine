#include "Horo/Extensions/EditorSvgIcon.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace Horo::Extensions::Tests {
    TEST_CASE("Packaged vector SVG icons rasterize to owned bounded pixels", "[Extensions][EditorSurface][Activity][Svg]") {
        const auto icon = RasterizeEditorSvgIcon(
            R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24"><path fill="#ffffff" d="M2 2L22 2L22 22L2 22Z"/></svg>)");
        REQUIRE(icon.HasValue());
        CHECK(icon.Value().width == 48);
        CHECK(icon.Value().height == 48);
        REQUIRE(icon.Value().pixels.size() == 48U * 48U * 4U);
        CHECK(std::ranges::any_of(icon.Value().pixels, [](std::uint8_t pixel) {
            return pixel != 0;
        }));
    }

    TEST_CASE("SVG decimal grammar is bounded and independent of deployment conversion APIs", "[Extensions][Activity][Svg]") {
        CHECK(RasterizeEditorSvgIcon("<svg width='2.4e1' height='+24' viewBox='-1 -1 2.4E1 24'><path d='M+.5 -.5 L2e1 20 L.5 20 Z'/></svg>")
                  .HasValue());
        for (const std::string number : {"1e", "1e+", ".", "--1", "1e33", "1e-33", "nan", "inf", "12345678901234567890123456789012345"})
            CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24'><path d='M0 0 L" + number + " 2'/></svg>").HasError());
    }

    TEST_CASE("SVG icons reject active external recursive and oversized resources before decoding",
              "[Extensions][EditorSurface][Activity][Svg]") {
        for (const std::string body :
             {"<image href='file:///etc/passwd'/>", "<script>alert(1)</script>", "<use href='#self'/>", "<filter/>", "<mask/>",
              "<foreignObject/>", "<path fill='url(https://example.com)' d='M0 0L1 1'/>", "<path transform='scale(100000)' d='M0 0L1 1'/>",
              "<path d='M0 0L1e20 1'/>", "<rect onclick='alert(1)' width='24' height='24'/>", "<text>font access</text>"})
            CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24'>" + body + "</svg>").HasError());
        CHECK(RasterizeEditorSvgIcon("<!DOCTYPE svg [<!ENTITY x SYSTEM 'file:///etc/passwd'>]><svg width='24' height='24'>&x;</svg>")
                  .HasError());
        CHECK(RasterizeEditorSvgIcon(std::string(65537, ' ')).HasError());
        CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24' viewBox='0 0 0.00001 0.00001'><path d='M0 0L10 10'/></svg>").HasError());
        CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24'><g></svg>").HasError());
        CHECK(RasterizeEditorSvgIcon("<svg width='24' width='48' height='24'/>").HasError());
        CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24'><path d='m0 0l24 24'/></svg>").HasError());
        std::string commands;
        for (int index = 0; index < 257; ++index)
            commands += "M0 0";
        CHECK(RasterizeEditorSvgIcon("<svg width='24' height='24'><path d='" + commands + "'/></svg>").HasError());
    }
}  // namespace Horo::Extensions::Tests
