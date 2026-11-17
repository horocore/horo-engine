#include "EditorDirectionalShadowOptions.h"
#include "EditorVulkanShaderAssets.h"
#include "support/ShaderReflectionCookTestSupport.h"

#include <bit>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <fstream>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Render;
    using namespace Horo::Tests::ShaderCook;

    struct Bundle final {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo viewport bundle " + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        CookFixture viewport;
        CookFixture shadow{CookFixtureRole::Shadow, ShadowEvidence()};

        Bundle() {
            std::filesystem::create_directories(root / "shaders/editor");
            WriteCook("viewport-vulkan", viewport);
            WriteCook("viewport-shadow-vulkan", shadow);
        }

        ~Bundle() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path Path(std::string_view file) const {
            return root / "shaders/editor" / file;
        }

        void Write(std::string_view file, std::span<const std::uint8_t> bytes) const {
            std::ofstream output(Path(file), std::ios::binary | std::ios::trunc);
            output.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(output.good());
        }

        void WriteCook(std::string_view prefix, const CookFixture &fixture) const {
            const auto metadata = EncodeReflectedShaderArtifact(fixture.reflected);
            REQUIRE(metadata.HasValue());
            Write(std::string(prefix) + ".horoshdr", fixture.compiled.payload);
            Write(std::string(prefix) + ".reflection.json", metadata.Value());
        }
    };
}  // namespace

TEST_CASE("Packaged Vulkan viewport loading requires and owns both canonical lit and shadow artifacts", "[editor][shader-cook][assets]") {
    Bundle bundle;
    const auto loaded = LoadPackagedVulkanViewportShader(bundle.root);
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().shadow);
    CHECK(loaded.Value().metadata.cookedArtifactKey == bundle.viewport.reflected.cookedArtifactKey);
    CHECK(loaded.Value().shadow->metadata.cookedArtifactKey == bundle.shadow.reflected.cookedArtifactKey);
    CHECK(loaded.Value().shadow->payload == bundle.shadow.compiled.payload);
}

TEST_CASE("Packaged Vulkan viewport loading rejects either missing shadow artifact", "[editor][shader-cook][assets]") {
    Bundle bundle;
    const auto file = GENERATE("viewport-shadow-vulkan.horoshdr", "viewport-shadow-vulkan.reflection.json");
    std::filesystem::remove(bundle.Path(file));
    const auto result = LoadPackagedVulkanViewportShader(bundle.root);
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().code.Value() == "editor.renderer.viewport_shader_unavailable");
}

TEST_CASE("Packaged Vulkan viewport loading rejects altered foreign and oversized shadow data", "[editor][shader-cook][assets]") {
    Bundle bundle;
    const auto mutation = GENERATE(0, 1, 2);
    if (mutation == 0) {
        auto altered = bundle.shadow.compiled.payload;
        altered.back() ^= 1;
        bundle.Write("viewport-shadow-vulkan.horoshdr", altered);
    }
    if (mutation == 1)
        bundle.WriteCook("viewport-shadow-vulkan", bundle.viewport);
    if (mutation == 2) {
        std::ofstream output(bundle.Path("viewport-shadow-vulkan.horoshdr"), std::ios::binary | std::ios::trunc);
        output.seekp(8U * 1024U * 1024U);
        output.put('x');
    }
    CHECK(LoadPackagedVulkanViewportShader(bundle.root).HasError());
}

namespace {
    std::optional<Horo::Render::DirectionalShadowProfile> ParseShadowOptions(const std::initializer_list<const char *> values) {
        std::vector<std::string> storage;
        for (const auto *value : values)
            storage.emplace_back(value);
        std::vector<char *> arguments;
        for (auto &value : storage)
            arguments.push_back(value.data());
        return Horo::Editor::Detail::ParseDirectionalShadowArguments(arguments);
    }
}  // namespace

TEST_CASE("Shadow startup options admit each explicit quality and reject malformed or duplicate budgets", "[editor][shadow][options]") {
    using namespace Horo::Render;
    const auto expected = GENERATE(DirectionalShadowQuality::Off, DirectionalShadowQuality::Low, DirectionalShadowQuality::Medium,
                                   DirectionalShadowQuality::High, DirectionalShadowQuality::Ultra);
    const std::array names{"off", "low", "medium", "high", "ultra"};
    const auto preset = MakeDirectionalShadowProfile(expected);
    const auto name = names[static_cast<std::size_t>(expected)];
    const auto separate = ParseShadowOptions({"editor", "--shadow-quality", name});
    const std::string joined = std::string{"--shadow-quality="} + name;
    const auto attached = ParseShadowOptions({"editor", joined.c_str()});
    REQUIRE(separate);
    REQUIRE(attached);
    CHECK(separate->cascadeCount == preset.cascadeCount);
    CHECK(separate->tileResolution == preset.tileResolution);
    CHECK(separate->enabled == preset.enabled);
    CHECK(attached->cascadeCount == preset.cascadeCount);
    CHECK(attached->enabled == preset.enabled);
    CHECK(ParseShadowOptions({"editor"})->cascadeCount == MakeDirectionalShadowProfile(DirectionalShadowQuality::High).cascadeCount);
    CHECK_FALSE(ParseShadowOptions({"editor", "--shadow-quality"}));
    CHECK_FALSE(ParseShadowOptions({"editor", "--shadow-quality="}));
    CHECK_FALSE(ParseShadowOptions({"editor", "--shadow-quality", "unknown"}));
    CHECK_FALSE(ParseShadowOptions({"editor", "--shadow-quality=low", "--shadow-quality=high"}));
    CHECK_FALSE(ParseShadowOptions({"editor", "--shadow-quality", "low", "--shadow-quality=low"}));
}
