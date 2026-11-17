#include "Horo/Runtime/Render/DirectionalShadow.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    struct Scene final {
        std::array<MeshVertex, 3> vertices{
            {{{-1, -1, 0}, {0, 0, 1}, {0, 0}}, {{1, -1, 0}, {0, 0, 1}, {1, 0}}, {{0, 1, 0}, {0, 0, 1}, {0.5F, 1}}}};
        std::array<std::uint32_t, 3> indices{0, 1, 2};
        std::array<RenderMeshResourceView, 1> resources{{{{{1}, 1}, vertices, indices, {{-1, -1, 0}, {1, 1, 0}}}}};
        std::array<RenderStaticMeshInstance, 1> instances{{{.mesh = {{1}, 1}, .localBounds = {{-1, -1, 0}, {1, 1, 0}}}}};
        std::array<RenderLight, 1> lights{{{.direction = Math::Normalize(Math::Vec3{-0.5F, -1, -0.4F})}}};
        RenderCameraView camera{.position = {0, 0, 4}, .target = {0, 0, 0}};

        RenderSceneView View() const {
            return {camera, resources, instances, lights};
        }

        DirectionalShadowFrame Frame(DirectionalShadowProfile profile = {}) const {
            const auto result = BuildDirectionalShadowFrame(View(), 1.5F, Math::ClipDepthRange::ZeroToOne, profile);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value());
            return *result.Value();
        }
    };

    void RequireInside(const DirectionalShadowCascade &cascade, Math::Vec3 point, Math::ClipDepthRange depth) {
        const auto clip = Math::TransformHomogeneous(cascade.viewProjection, {point.x, point.y, point.z, 1});
        REQUIRE(std::isfinite(clip.w));
        REQUIRE(clip.w != 0);
        CHECK(std::abs(clip.x / clip.w) <= 1.00001F);
        CHECK(std::abs(clip.y / clip.w) <= 1.00001F);
        CHECK(clip.z / clip.w >= (depth == Math::ClipDepthRange::ZeroToOne ? -0.00001F : -1.00001F));
        CHECK(clip.z / clip.w <= 1.00001F);
    }

    void CheckReceiverCorners(const RenderCameraView &camera, const DirectionalShadowCascade &cascade, Math::ClipDepthRange depth,
                              Math::Vec3 normalOffset = {}) {
        const auto forward = Math::Normalize(camera.target - camera.position);
        const auto right = Math::Normalize(Math::Cross(forward, camera.up));
        const auto up = Math::Cross(right, forward);
        for (unsigned index = 0; index < 8; ++index) {
            const auto distance = (index & 4U) ? cascade.splitFar : cascade.splitNear;
            const float halfHeight = camera.projection.kind == RenderProjectionKind::Perspective
                                         ? std::tan(camera.projection.verticalFovRadians * 0.5F) * distance
                                         : camera.projection.orthographicHeight * 0.5F;
            const auto point = camera.position + forward * distance + right * ((index & 1U ? 1.0F : -1.0F) * halfHeight * 1.5F) +
                               up * ((index & 2U ? 1.0F : -1.0F) * halfHeight);
            RequireInside(cascade, point + normalOffset, depth);
        }
    }
}  // namespace

TEST_CASE("Directional cascades fit actual perspective and orthographic receiver frusta in both clip ranges", "[renderer][shadow]") {
    Scene scene;
    const auto depth = GENERATE(Math::ClipDepthRange::ZeroToOne, Math::ClipDepthRange::NegativeOneToOne);
    scene.camera.projection.kind = GENERATE(RenderProjectionKind::Perspective, RenderProjectionKind::Orthographic);
    const auto result = BuildDirectionalShadowFrame(scene.View(), 1.5F, depth, {});
    REQUIRE(result.HasValue());
    REQUIRE(result.Value());
    const auto &frame = *result.Value();
    CHECK(frame.IsValid(scene.View()));
    CHECK(frame.cascadeCount == 4);
    for (const auto &cascade : frame.cascades)
        CheckReceiverCorners(scene.camera, cascade, depth);
}

TEST_CASE("Directional split modes use bounded mixed and logarithmic camera distances with overlap", "[renderer][shadow]") {
    Scene scene;
    scene.camera.projection.nearPlane = 1;
    scene.camera.projection.farPlane = 100;
    auto profile = MakeDirectionalShadowProfile(DirectionalShadowQuality::Medium);
    profile.splitMode = DirectionalShadowSplitMode::Logarithmic;
    const auto logarithmic = scene.Frame(profile);
    CHECK(Math::NearlyEqual(logarithmic.cascades[0].splitFar, 10.0F, 0.0001F));
    CHECK(Math::NearlyEqual(logarithmic.cascades[1].splitFar, 100.0F, 0.0001F));
    CHECK(logarithmic.cascades[0].blendStart < logarithmic.cascades[0].splitFar);
    profile.splitMode = DirectionalShadowSplitMode::Mixed;
    profile.splitLambda = 0;
    CHECK(Math::NearlyEqual(scene.Frame(profile).cascades[0].splitFar, 50.5F, 0.0001F));
}

TEST_CASE("Directional quality profiles produce bounded guarded atlas tiles", "[renderer][shadow]") {
    Scene scene;
    for (const auto quality : {DirectionalShadowQuality::Low, DirectionalShadowQuality::Medium, DirectionalShadowQuality::High,
                               DirectionalShadowQuality::Ultra}) {
        const auto profile = MakeDirectionalShadowProfile(quality);
        const auto frame = scene.Frame(profile);
        CHECK(frame.atlasWidth <= 4096);
        CHECK(frame.atlasHeight <= 4096);
        for (unsigned index = 0; index < frame.cascadeCount; ++index) {
            const auto &rect = frame.cascades[index].viewport;
            CHECK(rect.width == profile.tileResolution - 4);
            CHECK(rect.height == profile.tileResolution - 4);
            CHECK(rect.x % profile.tileResolution == 2);
            CHECK(rect.y % profile.tileResolution == 2);
            CHECK(rect.x + rect.width <= frame.atlasWidth);
            CHECK(rect.y + rect.height <= frame.atlasHeight);
        }
    }
    const auto disabled = MakeDirectionalShadowProfile(DirectionalShadowQuality::Off);
    CHECK_FALSE(BuildDirectionalShadowFrame(scene.View(), 1.5F, Math::ClipDepthRange::ZeroToOne, disabled).Value());
    CHECK(ComputeDirectionalShadowAtlasExtent(disabled).Value() == DirectionalShadowAtlasExtent{});
}

TEST_CASE("Directional cache observes actual geometry edits without fabricated mesh generations", "[renderer][shadow]") {
    Scene scene;
    const auto first = scene.Frame();
    const auto cached = CommitDirectionalShadowRender(first, 17, {});
    REQUIRE(cached.HasValue());
    CHECK(cached.Value().generation == 1);
    CHECK_FALSE(DirectionalShadowRenderRequired(scene.Frame(), 17, cached.Value()));
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5);
    if (mutation == 0)
        scene.vertices[0].position.x += 0.125F;
    if (mutation == 1)
        scene.indices[0] = 1;
    if (mutation == 2)
        scene.instances[0].localToWorld = Math::TranslationMatrix({1, 0, 0});
    if (mutation == 3) {
        scene.camera.position.x += 1;
        scene.camera.target.x += 1;
    }
    if (mutation == 4)
        scene.lights[0].direction = Math::Normalize(Math::Vec3{1, -1, 0});
    if (mutation == 5)
        scene.instances[0].localBounds.maximum.x += 1;
    const auto next = scene.Frame();
    CHECK(next.fingerprint != first.fingerprint);
    CHECK(DirectionalShadowRenderRequired(next, 17, cached.Value()));
    CHECK(DirectionalShadowRenderRequired(first, 18, cached.Value()));
    CHECK(CommitDirectionalShadowRender(next, 17, cached.Value()).Value().generation == 2);
}

TEST_CASE("Directional cache conservatively rerenders when actual caster geometry is unavailable", "[renderer][shadow]") {
    Scene scene;
    auto view = scene.View();
    view.meshResources = {};
    const auto result = BuildDirectionalShadowFrame(view, 1.5F, Math::ClipDepthRange::ZeroToOne, {});
    REQUIRE(result.HasValue());
    REQUIRE(result.Value());
    CHECK_FALSE(result.Value()->cacheable);
    const auto committed = CommitDirectionalShadowRender(*result.Value(), 1, {});
    REQUIRE(committed.HasValue());
    CHECK(DirectionalShadowRenderRequired(*result.Value(), 1, committed.Value()));
}

TEST_CASE("Directional profiles reject invalid budgets split filters and cache overflow", "[renderer][shadow]") {
    Scene scene;
    DirectionalShadowProfile profile;
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5);
    if (mutation == 0)
        profile.cascadeCount = 5;
    if (mutation == 1)
        profile.tileResolution = 4096;
    if (mutation == 2)
        profile.splitLambda = std::numeric_limits<float>::quiet_NaN();
    if (mutation == 3)
        profile.blendFraction = 0.6F;
    if (mutation == 4)
        profile.filterRadius = 3;
    if (mutation == 5)
        profile.normalBias = -1;
    CHECK(BuildDirectionalShadowFrame(scene.View(), 1.5F, Math::ClipDepthRange::ZeroToOne, profile).HasError());
    const auto frame = scene.Frame();
    CHECK(CommitDirectionalShadowRender(frame, 0, {}).HasError());
    CHECK(CommitDirectionalShadowRender(frame, 1, {0, 0, std::numeric_limits<std::uint64_t>::max(), false}).HasError());
}

TEST_CASE("Unknown directional quality cannot silently select a valid shadow budget", "[renderer][shadow]") {
    const auto profile = MakeDirectionalShadowProfile(static_cast<DirectionalShadowQuality>(255));
    CHECK_FALSE(profile.IsValid());
    CHECK(ComputeDirectionalShadowAtlasExtent(profile).HasError());
}

TEST_CASE("Directional stabilized cascades preserve subtexel translation and rotation invariant receiver scale", "[renderer][shadow]") {
    Scene scene;
    const auto first = scene.Frame();
    scene.camera.position.x += 0.000001F;
    scene.camera.target.x += 0.000001F;
    const auto moved = scene.Frame();
    const auto originalClip = Math::TransformHomogeneous(first.cascades[0].viewProjection, {0, 0, 0, 1});
    const auto movedClip = Math::TransformHomogeneous(moved.cascades[0].viewProjection, {0, 0, 0, 1});
    CHECK(Math::NearlyEqual(originalClip.x, movedClip.x, 0.00001F));
    CHECK(Math::NearlyEqual(originalClip.y, movedClip.y, 0.00001F));
    scene.camera.target.x += 0.5F;
    const auto rotated = scene.Frame();
    for (unsigned index = 0; index < first.cascadeCount; ++index)
        CHECK(Math::NearlyEqual(first.cascades[index].worldUnitsPerTexel, rotated.cascades[index].worldUnitsPerTexel, 0.00001F));
}

TEST_CASE("Directional blending fits the next cascade across the actual preceding transition interval", "[renderer][shadow]") {
    Scene scene;
    const auto frame = scene.Frame();
    for (unsigned index = 1; index < frame.cascadeCount; ++index) {
        auto overlap = frame.cascades[index];
        overlap.splitNear = frame.cascades[index - 1].blendStart;
        CheckReceiverCorners(scene.camera, overlap, Math::ClipDepthRange::ZeroToOne);
    }
}

TEST_CASE("Directional normal bias remains inside the admitted cascade coverage at its maximum", "[renderer][shadow]") {
    Scene scene;
    DirectionalShadowProfile profile;
    profile.normalBias = 10.0F;
    const auto depth = GENERATE(Math::ClipDepthRange::ZeroToOne, Math::ClipDepthRange::NegativeOneToOne);
    const auto result = BuildDirectionalShadowFrame(scene.View(), 1.5F, depth, profile);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value());
    constexpr std::array offsets{Math::Vec3{10, 0, 0},  Math::Vec3{-10, 0, 0}, Math::Vec3{0, 10, 0},
                                 Math::Vec3{0, -10, 0}, Math::Vec3{0, 0, 10},  Math::Vec3{0, 0, -10}};
    for (const auto &cascade : result.Value()->cascades)
        for (const auto offset : offsets)
            CheckReceiverCorners(scene.camera, cascade, depth, offset);
}

TEST_CASE("Directional shadow boundary rejects unknown clip depth ranges", "[renderer][shadow]") {
    Scene scene;
    CHECK(BuildDirectionalShadowFrame(scene.View(), 1.5F, static_cast<Math::ClipDepthRange>(255), {}).HasError());
}
