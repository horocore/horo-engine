#include "editor/renderer/metal/EditorViewportRendererMetal.h"
#include "renderer/MetalEditorViewportColorFixture.h"

#include <algorithm>
#include <array>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace Horo::Editor::Test {
    namespace {
        using namespace Render;
        constexpr std::array Recipes{ViewportColorLighting::Baseline, ViewportColorLighting::Clustered};
        constexpr Math::Vec3 Tint{.125F, .5F, .25F};
        // Independent IEC 61966-2-1 inverse transfer of authored UI sRGB code values (.125, .5, .25).
        // These constants do not call the product decoder and do not reinterpret UI tint as scene/AP1 color.
        constexpr std::array<float, 3> TintDisplayLinear{.014349874706802563F, .21404114048223255F, .05087608817155679F};

        /** @brief Real immutable CPU mesh sources; frontend/native identities remain issued by the production resource owner. */
        struct Scene final {
            std::array<MeshVertex, 4> front{
                {{{-1, -1, 0}, {0, 0, 1}, {}}, {{1, -1, 0}, {0, 0, 1}, {}}, {{1, 1, 0}, {0, 0, 1}, {}}, {{-1, 1, 0}, {0, 0, 1}, {}}}};
            std::array<MeshVertex, 4> rear;
            std::array<std::uint32_t, 6> indices{0, 1, 2, 0, 2, 3};
            std::array<RenderMeshResourceView, 2> meshes;
            std::array<RenderStaticMeshInstance, 2> instances;
            std::array<RenderLight, 1> lights;
            RenderCameraView camera{.position = {0, 0, 3}, .target = {0, 0, 0}};
            bool lit{};

            /** @brief Builds two genuinely overlapping source meshes at distinct world depths. */
            Scene() : rear(front) {
                for (auto &vertex : rear)
                    vertex.position.z = -.5F;
                Rebind();
            }

            /** @brief Keeps CPU source spans and real local bounds exact after deliberate geometry authoring. */
            void Rebind() {
                const Math::Aabb bounds{{-1, -1, -1}, {1, 1, 1}};
                const RenderMeshSourceHandle nearId{{83941}, 1}, farId{{83942}, 1};
                meshes[0] = {nearId, front, indices, bounds};
                meshes[1] = {farId, rear, indices, bounds};
                instances[0] = {.mesh = nearId, .localBounds = bounds};
                instances[1] = {.mesh = farId, .localBounds = bounds};
            }

            /** @brief Places an opaque horizontal mesh above the real XZ grid, whose center axis must remain depth-occluded. */
            void AboveGrid() {
                for (auto &vertex : front) {
                    const auto vertical = vertex.position.y;
                    vertex.position = {vertex.position.x, 1, -vertical};
                    vertex.normal = {0, 1, 0};
                }
                rear = front;
                for (auto &vertex : rear)
                    vertex.position.y = .5F;
                camera = {.position = {0, 4, 5}, .target = {0, 1, 0}};
                Rebind();
            }

            /** @brief Supplies one actual canonical directional HDR light, without a CPU lighting or tone evaluator. */
            void Hdr() {
                lit = true;
                lights[0] = {.kind = RenderLightKind::Directional,
                             .direction = {0, 0, -1},
                             .color = {1, 1, 1},
                             .intensity = 8,
                             .stableIdentity = 1};
            }

            /** @brief Borrows the exact CPU source and content-derived light revision for production frame preparation. */
            RenderSceneView View() const {
                const auto current = lit ? std::span<const RenderLight>{lights} : std::span<const RenderLight>{};
                return {.camera = camera,
                        .meshResources = meshes,
                        .instances = instances,
                        .lights = current,
                        .lightSceneDomain = 8394,
                        .lightSnapshotRevision = ComputeEditorViewportLightRevision(current)};
            }
        };

        /** @brief Reads portable output from the actual independent pinned OCIO processor, not a product shader mirror. */
        nlohmann::json Oracle() {
            const auto path =
                std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/renderer/color/viewport-ambient-reference.json";
            REQUIRE(std::filesystem::file_size(path) < 64U * 1024U);
            std::ifstream input(path, std::ios::binary);
            REQUIRE(input);
            const auto value = nlohmann::json::parse(input);
            REQUIRE(value.at("configurationDigest") == "9e3ec773a7fbc0bb6666e428dedd8ccec4f59b8b6d40506c825cc7a8c02ff2ff");
            return value;
        }

        /** @brief Evaluates only the declared final transfer, after independent analytic/LUT observations. */
        float Encode(float linear) {
            linear = std::clamp(linear, 0.0F, 1.0F);
            return linear <= .0031308F ? linear * 12.92F : 1.055F * std::pow(linear, 1.0F / 2.4F) - .055F;
        }

        /** @brief Bounds final bytes from independent display-linear output, measured FP16 storage, and exact center Bayer phase. */
        void CheckFinal(const ViewportColorPixels &pixels, const std::array<float, 3> &display) {
            const auto actual = pixels.Final(32, 32);
            constexpr float noise = -.46875F / 255;
            for (std::size_t channel = 0; channel < display.size(); ++channel) {
                int exponent{};
                std::frexp(std::abs(display[channel]), &exponent);
                const float store =
                    std::abs(display[channel]) < std::ldexp(1.0F, -14) ? std::ldexp(1.0F, -24) : std::ldexp(1.0F, exponent - 11);
                const auto low = std::clamp(Encode(display[channel] - store - 2e-5F) + noise, 0.0F, 1.0F);
                const auto high = std::clamp(Encode(display[channel] + store + 2e-5F) + noise, 0.0F, 1.0F);
                const float observed = float(actual[channel]) / 255;
                CHECK(observed >= low - 1.0F / 255);
                CHECK(observed <= high + 1.0F / 255);
            }
            CHECK(actual[3] == 255);
        }

        /** @brief Preserves the strict independent ambient/raw storage check before checking the product's sole final transform. */
        void CheckAmbient(const ViewportColorPixels &pixels, const nlohmann::json &oracle) {
            const auto raw = oracle.at("rawSceneRgb").get<std::array<float, 3>>();
            const auto actual = pixels.Scene(32, 32);
            for (std::size_t channel = 0; channel < raw.size(); ++channel)
                CHECK(actual[channel] == Catch::Approx(raw[channel]).epsilon(0).margin(std::ldexp(1.0, -15) + 2e-7));
            CheckFinal(pixels, oracle.at("displayLinearRgb").get<std::array<float, 3>>());
        }

        /** @brief Counts genuinely changed final pixels without using their colors as a fabricated renderer result. */
        std::size_t Changed(const ViewportColorPixels &before, const ViewportColorPixels &after) {
            REQUIRE(before.encoded.size() == after.encoded.size());
            std::size_t changed{};
            for (std::size_t offset = 0; offset < before.encoded.size(); offset += 4)
                if (!std::equal(before.encoded.begin() + offset, before.encoded.begin() + offset + 4, after.encoded.begin() + offset))
                    ++changed;
            return changed;
        }
    }  // namespace

    TEST_CASE("Actual baseline and clustered mesh graphs preserve raw FP16 and apply the independent output processor once",
              "[renderer][metal][editor-viewport-color]") {
        const auto oracle = Oracle();
        for (const auto recipe : Recipes) {
            INFO(static_cast<unsigned>(recipe));
            MetalEditorViewportColorFixture fixture{recipe};
            Scene scene;
            fixture.Render(scene.View());
            CheckAmbient(fixture.Read(), oracle);
            scene.Hdr();
            fixture.Render(scene.View());
            const auto hdr = fixture.Read();
            const auto raw = hdr.Scene(32, 32);
            for (const auto value : raw) {
                CHECK(std::isfinite(value));
                CHECK(value > 1);
            }
            CHECK(hdr.Final(32, 32)[3] == 255);
        }
    }

    TEST_CASE("Actual viewport selection preserves scene metering pixels and rejects a selected mesh hidden by scene depth",
              "[renderer][metal][editor-viewport-color]") {
        for (const auto recipe : Recipes) {
            INFO(static_cast<unsigned>(recipe));
            MetalEditorViewportColorFixture fixture{recipe};
            Scene scene;
            fixture.Render(scene.View());
            const auto plain = fixture.Read();
            scene.instances[1].presentation = {.tint = Tint, .tintStrength = 1};
            fixture.Render(scene.View());
            const auto hidden = fixture.Read();
            CHECK(hidden.rawScene == plain.rawScene);
            CHECK(hidden.Final(32, 32) == plain.Final(32, 32));
            scene.instances[1].presentation.tintStrength = 0;
            scene.instances[0].presentation = {.tint = Tint, .tintStrength = 1};
            fixture.Render(scene.View());
            const auto visible = fixture.Read();
            CHECK(visible.rawScene == plain.rawScene);
            CHECK(Changed(plain, visible) > 0);
            CheckFinal(visible, TintDisplayLinear);
            CHECK(fixture.Viewport().ColorDiagnostics().acceptedSubmission == 3);
        }
    }

    TEST_CASE("Actual mesh viewport consumes enabled nonidentity look once before the independent output processor",
              "[renderer][metal][editor-viewport-color]") {
        const auto oracle = Oracle();
        for (const auto recipe : Recipes) {
            for (const auto interpolation : std::array{ColorLutInterpolation::Trilinear, ColorLutInterpolation::Tetrahedral}) {
                INFO(static_cast<unsigned>(recipe));
                INFO(static_cast<unsigned>(interpolation));
                MetalEditorViewportColorFixture fixture{recipe, interpolation};
                Scene scene;
                fixture.Render(scene.View());
                const auto pixels = fixture.Read();
                const auto name = interpolation == ColorLutInterpolation::Trilinear ? "Trilinear" : "Tetrahedral";
                const auto found = std::ranges::find_if(oracle.at("looks"), [=](const auto &entry) {
                    return entry.at("interpolation") == name;
                });
                REQUIRE(found != oracle.at("looks").end());
                CheckFinal(pixels, found->at("displayLinearRgb").template get<std::array<float, 3>>());
            }
        }
    }

    TEST_CASE("Actual post-color viewport grid changes visible pixels while loading the opaque scene depth and excluding the meter",
              "[renderer][metal][editor-viewport-color]") {
        for (const auto recipe : Recipes) {
            INFO(static_cast<unsigned>(recipe));
            MetalEditorViewportColorFixture fixture{recipe};
            Scene scene;
            scene.AboveGrid();
            fixture.Render(scene.View(), false);
            const auto plain = fixture.Read();
            fixture.Render(scene.View(), true);
            const auto grid = fixture.Read();
            CHECK(grid.rawScene == plain.rawScene);
            CHECK(Changed(plain, grid) > 8);
            for (std::uint32_t y = 31; y <= 33; ++y)
                for (std::uint32_t x = 31; x <= 33; ++x)
                    CHECK(grid.Final(x, y) == plain.Final(x, y));
        }
    }
}  // namespace Horo::Editor::Test
