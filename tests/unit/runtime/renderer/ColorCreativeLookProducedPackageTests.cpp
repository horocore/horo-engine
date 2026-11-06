#include "ColorCreativeLookPackage.h"

#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

TEST_CASE("Creative look loader accepts original pinned OCIO producer generations without changing sample bits",
          "[renderer][color][look-package][produced]") {
    using namespace Horo::Render;
    using namespace Horo::Render::Internal;
    const auto mode = GENERATE("trilinear", "tetrahedral");
    const auto root = std::filesystem::path{HORO_CREATIVE_LOOK_FIXTURE_ROOT} / mode;
    std::ifstream input(root / "source.json");
    REQUIRE(input.good());
    const auto source = nlohmann::json::parse(input);
    const auto loaded = LoadColorCreativeLookPackage(root);
    REQUIRE(loaded.HasValue());
    const auto &look = *loaded.Value().look;
    CHECK(look.Descriptor().schemaVersion == 2);
    CHECK(look.Descriptor().dimension == 2);
    CHECK(look.Descriptor().configurationRelease == 40000);
    CHECK(FormatSha256(look.Descriptor().configurationDigest) == "sha256:9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff");
    CHECK(look.Descriptor().interpolation ==
          (std::string_view{mode} == "trilinear" ? ColorLutInterpolation::Trilinear : ColorLutInterpolation::Tetrahedral));
    REQUIRE(look.Rgb().size() == source["rgb"].size() * 3);
    for (std::size_t i = 0; i < look.Rgb().size(); ++i) {
        const auto expected = source["rgb"][i / 3][i % 3].get<float>();
        CHECK(std::bit_cast<std::uint32_t>(look.Rgb()[i]) == std::bit_cast<std::uint32_t>(expected));
    }
    const auto &bytes = loaded.Value().files[2];
    const auto reference = nlohmann::json::parse(bytes.begin(), bytes.end());
    CHECK(reference["cases"].size() == 13);
    CHECK(reference["processor"] == "direct-OCIO-CPU-optimization-none-Lut3D-then-output");
}
