#include "ColorPipelineTestSupport.h"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Test;

    /** @brief Checks the typed integrity rejection without treating it as a shader-cook decision. */
    void Rejected(const ColorOutputProgramFixture &fixture) {
        const auto result = fixture.Create();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ColorPipelineErrors::InvalidTransform.code.Value());
    }
}  // namespace

TEST_CASE("Color output program owns copied source metadata and both table representations", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    const auto source = fixture.source;
    const auto descriptor = fixture.descriptor;
    const auto original = fixture.original;
    const auto upload = fixture.upload;
    const auto created = fixture.Create();
    REQUIRE(created.HasValue());
    fixture.source[0] = 'x';
    fixture.tables[0].textureName = "changed";
    fixture.original[0][0] ^= std::byte{1};
    fixture.upload[1][0] ^= std::byte{1};
    fixture.descriptor.configurationDigest = {};
    const auto &program = *created.Value();
    CHECK(program.Source() == source);
    CHECK(program.Descriptor().configurationDigest == descriptor.configurationDigest);
    CHECK(program.Descriptor().sourceDigest == descriptor.sourceDigest);
    CHECK(CookedColorOutputProgram::FunctionName() == "HoroColorOutput");
    REQUIRE(program.Table(0));
    CHECK(program.Table(0)->textureName == "horo_color_reach_m_table_0");
    for (std::size_t i = 0; i < original.size(); ++i) {
        CHECK(std::ranges::equal(program.TableSource(i), original[i]));
        CHECK(std::ranges::equal(program.TableUpload(i), upload[i]));
    }
    CHECK(program.PayloadBytes() == source.size() + 1448 + 1448 + 4344 + 5792);
    CHECK(program.Table(2) == nullptr);
    CHECK(program.TableSource(2).empty());
    CHECK(program.TableUpload(std::numeric_limits<std::size_t>::max()).empty());
}

TEST_CASE("Color output source uses the exact finite source budget including its boundary", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    fixture.source.resize(CookedColorOutputProgram::MaximumSourceBytes, ' ');
    fixture.Rehash();
    REQUIRE(fixture.Create().HasValue());
    fixture.source.push_back(' ');
    fixture.Rehash();
    Rejected(fixture);
}

TEST_CASE("Color output source must be present and match its exact byte digest", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    SECTION("missing") {
        fixture.source.clear();
        fixture.Rehash();
    }
    SECTION("corruption") {
        fixture.source[0] ^= 1;
    }
    SECTION("missing digest") {
        fixture.descriptor.sourceDigest = {};
    }
    Rejected(fixture);
}

TEST_CASE("Color output requires the exact closed schema profile and producer version metadata", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    SECTION("schema") {
        fixture.descriptor.schemaVersion = 2;
    }
    SECTION("profile") {
        fixture.descriptor.profile = static_cast<ColorOutputProfile>(255);
    }
    SECTION("configuration release") {
        fixture.descriptor.configurationRelease = 39999;
    }
    SECTION("producer version") {
        fixture.descriptor.ocioVersion = 20400;
    }
    SECTION("missing configuration") {
        fixture.descriptor.configurationDigest = {};
    }
    SECTION("foreign configuration") {
        fixture.descriptor.configurationDigest.bytes[0] ^= 1;
    }
    SECTION("missing options") {
        fixture.descriptor.optionsDigest = {};
    }
    SECTION("foreign options") {
        fixture.descriptor.optionsDigest.bytes[0] ^= 1;
    }
    SECTION("missing reference") {
        fixture.descriptor.referenceDigest = {};
    }
    Rejected(fixture);
}

TEST_CASE("Color output requires exactly two tables in their original ordinal order", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    auto inputs = fixture.Inputs();
    SECTION("missing table") {
        REQUIRE(CookedColorOutputProgram::Create(fixture.descriptor, fixture.source, std::span{inputs}.first(1)).HasError());
    }
    SECTION("extra table") {
        const std::array extra{inputs[0], inputs[1], inputs[0]};
        REQUIRE(CookedColorOutputProgram::Create(fixture.descriptor, fixture.source, extra).HasError());
    }
    SECTION("reordered") {
        std::swap(inputs[0], inputs[1]);
        REQUIRE(CookedColorOutputProgram::Create(fixture.descriptor, fixture.source, inputs).HasError());
    }
    SECTION("duplicate ordinal") {
        fixture.tables[1].ordinal = 0;
        Rejected(fixture);
    }
}

TEST_CASE("Color output rejects foreign table names extents channels and formats", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    const auto index = GENERATE(std::size_t{0}, std::size_t{1});
    auto &table = fixture.tables[index];
    SECTION("texture name") {
        table.textureName += "_foreign";
    }
    SECTION("sampler name") {
        table.samplerName += "_foreign";
    }
    SECTION("width") {
        table.extent.width = 361;
    }
    SECTION("height") {
        table.extent.height = 2;
    }
    SECTION("channels") {
        table.sourceChannels = 4;
    }
    SECTION("format") {
        table.format = RenderTextureFormat::Rgba16Float;
    }
    Rejected(fixture);
}

TEST_CASE("Color output rejects sampler reinterpretation rather than silently changing interpolation", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    auto &sampler = fixture.tables[GENERATE(std::size_t{0}, std::size_t{1})].sampler;
    SECTION("minification") {
        sampler.minFilter = RenderSamplerFilter::Linear;
    }
    SECTION("magnification") {
        sampler.magFilter = RenderSamplerFilter::Linear;
    }
    SECTION("mipmaps") {
        sampler.mipmapMode = RenderSamplerMipmapMode::Linear;
    }
    SECTION("address U") {
        sampler.addressU = RenderSamplerAddressMode::Repeat;
    }
    SECTION("address V") {
        sampler.addressV = RenderSamplerAddressMode::Repeat;
    }
    SECTION("address W") {
        sampler.addressW = RenderSamplerAddressMode::Repeat;
    }
    SECTION("minimum lod") {
        sampler.minimumLod = 1;
    }
    SECTION("maximum lod") {
        sampler.maximumLod = 1;
    }
    SECTION("anisotropy") {
        sampler.maximumAnisotropy = 2;
    }
    SECTION("comparison") {
        sampler.compareEnabled = true;
    }
    Rejected(fixture);
}

TEST_CASE("Color output detects original and upload corruption and missing digest metadata", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    const auto index = GENERATE(std::size_t{0}, std::size_t{1});
    SECTION("source corruption") {
        fixture.original[index][0] ^= std::byte{1};
    }
    SECTION("upload corruption") {
        fixture.upload[index][0] ^= std::byte{1};
    }
    SECTION("source digest") {
        fixture.tables[index].sourceDigest = {};
    }
    SECTION("upload digest") {
        fixture.tables[index].uploadDigest = {};
    }
    SECTION("source length") {
        fixture.original[index].pop_back();
        fixture.Rehash();
    }
    SECTION("upload length") {
        fixture.upload[index].pop_back();
        fixture.Rehash();
    }
    Rejected(fixture);
}

TEST_CASE("Color output validates every payload float even when integrity hashes match", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    const auto index = GENERATE(std::size_t{0}, std::size_t{1});
    const auto value =
        GENERATE(std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity());
    SECTION("original") {
        WriteColorFloat(fixture.original[index], fixture.original[index].size() - 4, value);
    }
    SECTION("upload") {
        WriteColorFloat(fixture.upload[index], fixture.upload[index].size() - 4, value);
    }
    fixture.Rehash();
    Rejected(fixture);
}

TEST_CASE("Color output requires bit-preserving RGB and alpha-one padding beyond matching hashes", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    SECTION("scalar table") {
        WriteColorFloat(fixture.upload[0], 0, 2);
    }
    SECTION("RGB table") {
        WriteColorFloat(fixture.upload[1], 0, 2);
    }
    SECTION("first padding") {
        WriteColorFloat(fixture.upload[1], 12, 0);
    }
    SECTION("last padding") {
        WriteColorFloat(fixture.upload[1], fixture.upload[1].size() - 4, 0);
    }
    SECTION("signed-zero bit preservation") {
        WriteColorFloat(fixture.original[1], 0, -0.0F);
        WriteColorFloat(fixture.upload[1], 0, 0.0F);
    }
    fixture.Rehash();
    Rejected(fixture);
}

TEST_CASE("Color output integrity admission does not claim source semantics or cooked native trust", "[renderer][color-program]") {
    ColorOutputProgramFixture fixture;
    fixture.source = "opaque synthetic integrity bytes; not a cooked shader";
    fixture.Rehash();
    const auto result = fixture.Create();
    REQUIRE(result.HasValue());
    CHECK(result.Value()->Source() == fixture.source);
}
