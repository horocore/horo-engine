#include "EditorDirectionalShadowOptions.h"
#include "EditorVulkanShaderAssets.h"
#include "EditorVulkanViewportShader.h"
#include "support/ShaderReflectionCookTestSupport.h"

#include <bit>
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

        std::filesystem::path Path(std::string_view file, std::string_view directory = "editor") const {
            return root / "shaders" / directory / file;
        }

        void Write(std::string_view file, std::span<const std::uint8_t> bytes, std::string_view directory = "editor") const {
            std::ofstream output(Path(file, directory), std::ios::binary | std::ios::trunc);
            output.write(std::bit_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            REQUIRE(output.good());
        }

        void WriteCook(std::string_view prefix, const CookFixture &fixture, std::string_view directory = "editor") const {
            const auto metadata = EncodeReflectedShaderArtifact(fixture.reflected);
            REQUIRE(metadata.HasValue());
            Write(std::string(prefix) + ".horoshdr", fixture.compiled.payload, directory);
            Write(std::string(prefix) + ".reflection.json", metadata.Value(), directory);
        }
    };
}  // namespace

TEST_CASE("Optional packaged Vulkan XR motion requires its own complete authentic bundle", "[editor][shader-cook][assets][xr]") {
    Bundle bundle;
    CHECK(LoadPackagedVulkanMultiviewMotionShader(bundle.root).HasError());
    std::filesystem::create_directories(bundle.root / "shaders/runtime");
    const CookFixture motion{CookFixtureRole::MultiviewMotion, MultiviewMotionEvidence()};
    bundle.WriteCook("multiview-motion-vulkan", motion, "runtime");
    const auto loaded = LoadPackagedVulkanMultiviewMotionShader(bundle.root);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().metadata.cookedArtifactKey == motion.reflected.cookedArtifactKey);
    CHECK(LoadPackagedVulkanMultiviewShader(bundle.root).HasError());

    const CookFixture projection{CookFixtureRole::Multiview, MultiviewEvidence()};
    bundle.WriteCook("multiview-motion-vulkan", projection, "runtime");
    CHECK(LoadPackagedVulkanMultiviewMotionShader(bundle.root).HasError());
    bundle.WriteCook("multiview-vulkan", projection, "runtime");
    CHECK(LoadPackagedVulkanMultiviewShader(bundle.root).HasValue());
}

TEST_CASE("Vulkan temporal raster jitter stays subpixel and preserves clip depth", "[editor][vulkan][temporal]") {
    using namespace Horo;
    using namespace Horo::Editor;
    constexpr Render::FramebufferExtent extent{320, 160};
    const auto first = EditorVulkanTemporalJitter(0, extent);
    CHECK(first.x == 0.0F);
    CHECK(first.y * extent.height == Catch::Approx(-1.0F / 6.0F));
    const auto wrapped = EditorVulkanTemporalJitter(EditorVulkanTemporalJitterPhases, extent);
    CHECK(wrapped.x == first.x);
    CHECK(wrapped.y == first.y);
    const auto empty = EditorVulkanTemporalJitter(2, {});
    CHECK(empty.x == 0.0F);
    CHECK(empty.y == 0.0F);
    for (std::uint32_t phase = 0; phase < EditorVulkanTemporalJitterPhases; ++phase) {
        const auto jitter = EditorVulkanTemporalJitter(phase, extent);
        CHECK(std::abs(jitter.x * extent.width) < 0.5F);
        CHECK(std::abs(jitter.y * extent.height) < 0.5F);
    }

    auto clip = Math::Mat4::Identity();
    clip.values[3] = 2.0F;
    clip.values[12] = 0.5F;
    clip.values[13] = 0.25F;
    const Math::Vec2 jitter{0.125F, -0.0625F};
    const auto raster = EditorVulkanRasterProjection(clip, jitter);
    CHECK(raster.values[0] == 1.5F);
    CHECK(raster.values[1] == -0.25F);
    CHECK(raster.values[12] == 0.75F);
    CHECK(raster.values[13] == -0.375F);
    for (std::size_t column = 0; column < 4; ++column) {
        CHECK(raster.values[column * 4 + 2] == clip.values[column * 4 + 2]);
        CHECK(raster.values[column * 4 + 3] == clip.values[column * 4 + 3]);
    }
}

TEST_CASE("Selected XR projection rejects incomplete and foreign environment programs", "[editor][shader-cook][assets][xr]") {
    Bundle bundle;
    constexpr std::string_view environment = "multiview-environment-vulkan";
    std::filesystem::create_directories(bundle.root / "shaders/runtime");
    const CookFixture projection{CookFixtureRole::Multiview, MultiviewEvidence()};
    bundle.WriteCook("multiview-vulkan", projection, "runtime");
    const auto baseline = LoadPackagedVulkanXrProjection(bundle.root);
    REQUIRE(baseline.HasValue());
    CHECK(baseline.Value().program == EditorVulkanMultiviewProgram::Projection);

    bundle.Write("multiview-environment-vulkan.horoshdr", projection.compiled.payload, "runtime");
    CHECK(LoadPackagedVulkanXrProjection(bundle.root).HasError());
    bundle.WriteCook(environment, projection, "runtime");
    CHECK(LoadPackagedVulkanXrProjection(bundle.root).HasError());
}

TEST_CASE("Packaged Vulkan stereo coverage binds the separate reflected artifact", "[editor][shader-cook][assets][xr]") {
    Bundle bundle;
    constexpr std::string_view prefix = "multiview-coverage-vulkan";
    CHECK(LoadPackagedVulkanMultiviewCoverageShader(bundle.root).HasError());
    std::filesystem::create_directories(bundle.root / "shaders/runtime");
    const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VSCoverage", {1}},
                            ShaderArtifactStage{ShaderStage::Fragment, "PSCoverage", {2}}};
    const CookFixture coverage{CookFixtureRole::MultiviewCoverage, MultiviewCoverageEvidence(), stages};
    bundle.WriteCook(prefix, coverage, "runtime");
    const auto loaded = LoadPackagedVulkanMultiviewCoverageShader(bundle.root);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().metadata.cookedArtifactKey == coverage.reflected.cookedArtifactKey);
    bundle.WriteCook(prefix, CookFixture{CookFixtureRole::MultiviewMotion, MultiviewMotionEvidence()}, "runtime");
    CHECK(LoadPackagedVulkanMultiviewCoverageShader(bundle.root).HasError());
}

TEST_CASE("XR comfort accepts only its complete fullscreen shader bundle", "[editor][shader-cook][assets][xr]") {
    Bundle bundle;
    constexpr std::string_view prefix = "xr-comfort-vulkan";
    std::filesystem::create_directories(bundle.root / "shaders/runtime");
    CHECK(LoadPackagedVulkanXrComfortShader(bundle.root).HasError());

    auto reflection = XrComfortEvidence();
    std::array<ShaderArtifactStage, 2> stages;
    if (const auto *directory = std::getenv("HORO_TEST_XR_COMFORT_SPIRV_DIR")) {
        constexpr std::array names{"vs", "ps"};
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto base = std::filesystem::path{directory} / names[i];
            std::ifstream json(base.string() + ".json", std::ios::binary);
            std::ifstream spirv(base.string() + ".spv", std::ios::binary);
            REQUIRE(json.good());
            REQUIRE(spirv.good());
            reflection[i].json.assign(std::istreambuf_iterator<char>{json}, {});
            stages[i] = {reflection[i].stage,
                         reflection[i].entryPoint,
                         {std::istreambuf_iterator<char>{spirv}, std::istreambuf_iterator<char>{}}};
            REQUIRE_FALSE(stages[i].bytes.empty());
        }
    }
    const auto native =
        stages.front().bytes.empty() ? std::span<const ShaderArtifactStage>{} : std::span<const ShaderArtifactStage>{stages};
    const CookFixture comfort{CookFixtureRole::XrComfort, reflection, native};
    bundle.Write("xr-comfort-vulkan.horoshdr", comfort.compiled.payload, "runtime");
    CHECK(LoadPackagedVulkanXrComfortShader(bundle.root).HasError());
    bundle.WriteCook(prefix, comfort, "runtime");
    const auto loaded = LoadPackagedVulkanXrComfortShader(bundle.root);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().metadata.cookedArtifactKey == comfort.reflected.cookedArtifactKey);

    const CookFixture scene{CookFixtureRole::Multiview, MultiviewEvidence()};
    bundle.WriteCook(prefix, scene, "runtime");
    CHECK(LoadPackagedVulkanXrComfortShader(bundle.root).HasError());
}

TEST_CASE("Packaged Vulkan full-eye reduction requires the closed compute bundle", "[editor][shader-cook][assets][xr]") {
    Bundle bundle;
    CHECK(LoadPackagedVulkanCoverageReductionShader(bundle.root).HasError());
    std::filesystem::create_directories(bundle.root / "shaders/runtime");
    constexpr std::string_view prefix = "multiview-coverage-reduction-vulkan";
    const CookFixture motion{CookFixtureRole::MultiviewMotion, MultiviewMotionEvidence()};
    bundle.WriteCook(prefix, motion, "runtime");
    CHECK(LoadPackagedVulkanCoverageReductionShader(bundle.root).HasError());
}

TEST_CASE("Packaged Vulkan scene raster admission requires its distinct compiled stage and matched reflection",
          "[editor][shader-cook][assets][reconstruction]") {
    Bundle bundle;
    constexpr std::string_view prefix = "viewport-scene-linear-vulkan";
    CHECK(LoadPackagedVulkanSceneLinearShader(bundle.root).HasError());
    bundle.WriteCook(prefix, bundle.viewport);
    CHECK(LoadPackagedVulkanSceneLinearShader(bundle.root).HasError());

    auto reflected = Evidence();
    reflected[1].entryPoint = "PSSceneLinear";
    ReplaceEvidence(reflected[1].json, "PSMain", "PSSceneLinear");
    const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VSMain", {1}},
                            ShaderArtifactStage{ShaderStage::Fragment, "PSSceneLinear", {2}}};
    const CookFixture scene{CookFixtureRole::SceneLinear, reflected, stages};
    bundle.WriteCook(prefix, scene);
    const auto loaded = LoadPackagedVulkanSceneLinearShader(bundle.root);
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().metadata.cookedArtifactKey == scene.reflected.cookedArtifactKey);
    CHECK(LoadEditorVulkanViewportCook(scene.compiled.payload, EncodeReflectedShaderArtifact(scene.reflected).Value()).HasError());

    const CookFixture incorrectNativeStage{CookFixtureRole::SceneLinear, reflected};
    bundle.WriteCook(prefix, incorrectNativeStage);
    CHECK(LoadPackagedVulkanSceneLinearShader(bundle.root).HasError());
}

TEST_CASE("Packaged Vulkan sampled canvas binds its exact Vulkan consumer artifact", "[editor][shader-cook][assets][canvas]") {
    const auto *configured = std::getenv("HORO_VULKAN_UI_PAINT_ASSET_ROOT");
    if (!configured || !*configured)
        SKIP("Supply the actual offline Vulkan canvas shader package.");
    const std::filesystem::path assets{configured};
    REQUIRE(assets.is_absolute());
    const auto loaded = LoadPackagedVulkanWorldCanvasShader(assets);
    REQUIRE(loaded.HasValue());
    const auto metadata = EncodeReflectedShaderArtifact(loaded.Value().metadata);
    REQUIRE(metadata.HasValue());
    CHECK(LoadEditorVulkanWorldCanvasCook(loaded.Value().payload, metadata.Value()).HasValue());
    CHECK(LoadEditorVulkanCameraCrossFadeCook(loaded.Value().payload, metadata.Value()).HasError());
}

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
