#include "EditorViewportForwardLighting.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <vector>

namespace Horo::Editor {
    namespace {
        struct ForwardScene final {
            std::array<Render::MeshVertex, 3> vertices{
                {{.position = {-0.5F, -0.5F, 0}}, {.position = {0.5F, -0.5F, 0}}, {.position = {0, 0.5F, 0}}}};
            std::array<std::uint32_t, 3> indices{0, 1, 2};
            Math::Aabb bounds{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}};
            std::array<Render::RenderMeshResourceView, 1> meshes;
            std::array<Render::RenderStaticMeshInstance, 2> draws;
            std::vector<Render::RenderLight> lights;

            explicit ForwardScene(std::size_t count) : lights(count) {
                const Render::RenderMeshSourceHandle handle{{1}, 1};
                meshes[0] = {handle, vertices, indices, bounds};
                for (std::size_t draw = 0; draw < draws.size(); ++draw) {
                    draws[draw].mesh = handle;
                    draws[draw].localBounds = bounds;
                    draws[draw].localToWorld = Math::Transform{.translation = {draw == 0 ? -20.0F : 20.0F, 0, -5}}.ToMatrix();
                }
                for (std::size_t index = 0; index < lights.size(); ++index) {
                    lights[index] = {.kind = Render::RenderLightKind::Point,
                                     .position = {index < count / 2 ? -20.0F : 20.0F, 0, -5},
                                     .range = 2,
                                     .stableIdentity = index + 1};
                }
            }

            Render::RenderSceneView View() const {
                return {.meshResources = meshes, .instances = draws, .lights = lights, .lightSceneDomain = 71, .lightSnapshotRevision = 91};
            }
        };

        EditorViewportLightingProfile CpuProfile(bool complete = true) {
            EditorViewportLightingProfile result;
            result.requested = Render::LightPreparationMode::BaselineForwardCpu;
            result.budget.requireCompleteCoverage = complete;
            return result;
        }

        void Configure(EditorViewportForwardLighting &owner, bool complete = true) {
            REQUIRE(owner.Configure(CpuProfile(complete), {.mode = Render::LightPreparationMode::BaselineForwardCpu}).HasValue());
        }
    }  // namespace

    TEST_CASE("Forward viewport fallback keeps more than sixteen source lights across disjoint real draw bounds",
              "[unit][renderer][lighting][forward]") {
        ForwardScene fixture{32};
        auto scene = fixture.View();
        EditorViewportForwardLighting owner;
        Configure(owner);
        REQUIRE(owner.Prepare(scene).HasValue());
        std::array<Render::RenderLight, Render::MaximumForwardLights> output;
        const auto first = owner.SelectDraw(scene, fixture.draws[0], output);
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().count == 16);
        CHECK(output[0].stableIdentity == 1);
        const auto second = owner.SelectDraw(scene, fixture.draws[1], output);
        REQUIRE(second.HasValue());
        REQUIRE(second.Value().count == 16);
        CHECK(output[0].stableIdentity == 17);
        CHECK(owner.Diagnostics().sourceLightCount == 32);
        CHECK(owner.Diagnostics().shadedReferences == 32);
        CHECK(owner.Diagnostics().omittedReferences == 0);
    }

    TEST_CASE("Forward viewport completeness overflow fails before any draw while authored degradation reports omissions",
              "[unit][renderer][lighting][forward]") {
        ForwardScene fixture{17};
        for (auto &light : fixture.lights) {
            light.position = {0, 0, -5};
            light.range = 100;
        }
        auto scene = fixture.View();
        EditorViewportForwardLighting owner;
        Configure(owner);
        const auto complete = owner.Prepare(scene);
        REQUIRE(complete.HasError());
        CHECK(complete.ErrorValue().code.Value() == Render::LightCullingErrors::Coverage.code.Value());
        Configure(owner, false);
        REQUIRE(owner.Prepare(scene).HasValue());
        CHECK(owner.Diagnostics().sourceLightCount == 17);
        CHECK(owner.Diagnostics().shadedReferences == 32);
        CHECK(owner.Diagnostics().omittedReferences == 2);
    }

    TEST_CASE("Forward viewport shadow remapping reaches global light4095 and disables a genuinely omitted source",
              "[unit][renderer][lighting][forward][shadow]") {
        ForwardScene fixture{4096};
        for (auto &light : fixture.lights)
            light.intensity = 0;
        fixture.lights.back().kind = Render::RenderLightKind::Directional;
        fixture.lights.back().intensity = 1;
        auto scene = fixture.View();
        EditorViewportForwardLighting owner;
        Configure(owner);
        REQUIRE(owner.Prepare(scene).HasValue());
        const auto shadow =
            Render::BuildDirectionalShadowFrame(scene, 1, Math::ClipDepthRange::ZeroToOne,
                                                Render::MakeDirectionalShadowProfile(Render::DirectionalShadowQuality::Low));
        REQUIRE(shadow.HasValue());
        REQUIRE(shadow.Value());
        REQUIRE(shadow.Value()->lightIndex == 4095);
        std::array<Render::RenderLight, Render::MaximumForwardLights> output;
        const auto selection = owner.SelectDraw(scene, fixture.draws[0], output);
        REQUIRE(selection.HasValue());
        REQUIRE(selection.Value().count == 1);
        const auto remapped = owner.RemapShadow(shadow.Value(), selection.Value(), scene);
        REQUIRE(remapped.HasValue());
        REQUIRE(remapped.Value());
        CHECK(remapped.Value()->lightIndex == 0);
        CHECK(remapped.Value()->fingerprint == shadow.Value()->fingerprint);
        fixture.lights.resize(17);
        for (auto &light : fixture.lights) {
            light.intensity = 1;
            light.position = {0, 0, -5};
            light.range = 100;
        }
        fixture.lights.back().kind = Render::RenderLightKind::Directional;
        scene = fixture.View();
        ++scene.lightSnapshotRevision;
        Configure(owner, false);
        REQUIRE(owner.Prepare(scene).HasValue());
        const auto omittedShadow =
            Render::BuildDirectionalShadowFrame(scene, 1, Math::ClipDepthRange::ZeroToOne,
                                                Render::MakeDirectionalShadowProfile(Render::DirectionalShadowQuality::Low));
        REQUIRE(omittedShadow.HasValue());
        REQUIRE(omittedShadow.Value());
        const auto retained = owner.SelectDraw(scene, fixture.draws[0], output);
        REQUIRE(retained.HasValue());
        const auto disabled = owner.RemapShadow(omittedShadow.Value(), retained.Value(), scene);
        REQUIRE(disabled.HasValue());
        CHECK_FALSE(disabled.Value());
    }

    TEST_CASE("Forward viewport preparation rejects anonymous snapshots work overflow cancellation and stale consumers",
              "[unit][renderer][lighting][forward]") {
        ForwardScene fixture{32};
        auto scene = fixture.View();
        EditorViewportForwardLighting owner;
        Configure(owner);
        fixture.lights[0].stableIdentity = 0;
        CHECK(owner.Prepare(scene).HasError());
        fixture.lights[0].stableIdentity = 1;
        auto bounded = CpuProfile();
        bounded.maximumForwardLightTests = 1;
        REQUIRE(owner.Configure(bounded, {}).HasValue());
        CHECK(owner.Prepare(scene).HasError());
        Configure(owner);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto cancelled = owner.Prepare(scene, cancellation.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == Render::LightCullingErrors::Cancelled.code.Value());
        REQUIRE(owner.Prepare(scene).HasValue());
        ++scene.lightSnapshotRevision;
        std::array<Render::RenderLight, Render::MaximumForwardLights> output;
        CHECK(owner.SelectDraw(scene, fixture.draws[0], output).HasError());
    }

    TEST_CASE("Forward viewport welcome scene clears with no invented light domain while active geometry requires its producer",
              "[unit][renderer][lighting][forward]") {
        EditorViewportForwardLighting owner;
        Configure(owner);
        const Render::RenderSceneView welcome;
        REQUIRE(owner.Prepare(welcome).HasValue());
        CHECK(owner.Diagnostics().sourceLightCount == 0);
        CHECK(owner.Diagnostics().shadedReferences == 0);
        CHECK(owner.Diagnostics().omittedReferences == 0);
        CHECK(owner.Diagnostics().completedLightRevision == 0);
        CHECK_FALSE(owner.Diagnostics().gpuDiagnosticsPending);
        ForwardScene fixture{0};
        auto active = fixture.View();
        active.lightSceneDomain = 0;
        CHECK(owner.Prepare(active).HasError());
        active.lightSceneDomain = 71;
        active.lightSnapshotRevision = 0;
        CHECK(owner.Prepare(active).HasError());
        active.lightSnapshotRevision = 91;
        CHECK(owner.Prepare(active).HasValue());
    }
}  // namespace Horo::Editor
