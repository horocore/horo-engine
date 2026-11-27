/** @file EditorVulkanXrMirrorTests.cpp
 * @brief CPU regressions over the actual private viewport's fixed mirror-receipt storage.
 */
#include "EditorVulkanXrMirrorProjection.h"
#include "EditorVulkanXrViewportAccess.h"

#include <catch2/catch_test_macros.hpp>
#include <type_traits>

namespace Horo::Tests {
    using Editor::EditorVulkanXrMirrorProjection;
    using Render::RenderTextureViewHandle;
    static_assert(std::is_trivially_copyable_v<EditorVulkanXrMirrorProjection>);

    TEST_CASE("XR cold GUI projection has no acquired native view", "[renderer][vulkan][xr][mirror]") {
        const EditorVulkanXrMirrorProjection projection;
        const RenderTextureViewHandle eye{Render::RenderResourceOwnerId{11}, 2, 3};
        REQUIRE(projection.LogicalIdentity() != 0);
        CHECK_FALSE(projection.Borrow(1, eye).IsValid());
        CHECK_FALSE(projection.Borrow(0, {}).IsValid());
    }

    TEST_CASE("XR GUI mirror requires the exact frame and acquired-eye generation", "[renderer][vulkan][xr][mirror]") {
        EditorVulkanXrMirrorProjection projection;
        const auto identity = projection.LogicalIdentity();
        const RenderTextureViewHandle eye{Render::RenderResourceOwnerId{11}, 2, 3};
        REQUIRE(projection.Bind(17, eye));
        CHECK(projection.LogicalIdentity() == identity);
        CHECK(projection.Borrow(17, eye) == eye);
        CHECK_FALSE(projection.Borrow(16, eye).IsValid());
        CHECK_FALSE(projection.Borrow(18, eye).IsValid());
        CHECK_FALSE(projection.Borrow(17, {eye.owner, eye.slot, eye.generation + 1}).IsValid());
        CHECK_FALSE(projection.Borrow(17, {eye.owner, eye.slot + 1, eye.generation}).IsValid());
        CHECK_FALSE(projection.Borrow(17, {Render::RenderResourceOwnerId{12}, eye.slot, eye.generation}).IsValid());
        CHECK_FALSE(projection.Bind(0, eye));
        CHECK_FALSE(projection.Bind(18, {}));
        CHECK(projection.Borrow(17, eye) == eye);
        projection.ClearAcquisition();
        CHECK_FALSE(projection.Borrow(17, eye).IsValid());
        const RenderTextureViewHandle replacement{eye.owner, eye.slot, eye.generation + 1};
        REQUIRE(projection.Bind(18, replacement));
        CHECK_FALSE(projection.Borrow(17, eye).IsValid());
        CHECK(projection.Borrow(18, replacement) == replacement);
    }

    TEST_CASE("XR loss revokes mirror borrowing while retaining selected-mode intent", "[renderer][vulkan][xr][mirror]") {
        EditorVulkanXrMirrorProjection projection;
        const RenderTextureViewHandle eye{Render::RenderResourceOwnerId{11}, 2, 3};
        REQUIRE(projection.Bind(17, eye));
        const auto identity = projection.LogicalIdentity();
        projection.Revoke();  // The exact Scene's production loss/retirement path uses this operation.
        CHECK(projection.LogicalIdentity() == identity);
        CHECK_FALSE(projection.Borrow(17, eye).IsValid());
        const RenderTextureViewHandle replacement{eye.owner, eye.slot, eye.generation + 1};
        CHECK_FALSE(projection.Bind(18, replacement));
        CHECK_FALSE(projection.Borrow(18, replacement).IsValid());
    }

    TEST_CASE("XR terminal mirror retirement cannot revive a prior frame or selection", "[renderer][vulkan][xr][mirror]") {
        EditorVulkanXrMirrorProjection projection;
        const RenderTextureViewHandle eye{Render::RenderResourceOwnerId{11}, 2, 3};
        REQUIRE(projection.Bind(17, eye));
        projection.Retire();
        CHECK(projection.LogicalIdentity() == 0);
        CHECK_FALSE(projection.Borrow(17, eye).IsValid());
        CHECK_FALSE(projection.Bind(18, eye));
        projection.ClearAcquisition();
        projection.Retire();
        CHECK(projection.LogicalIdentity() == 0);
        CHECK_FALSE(projection.Borrow(18, eye).IsValid());
    }

    TEST_CASE("XR projection alpha proof requires exact completed graphics work and acquisition", "[renderer][vulkan][xr][alpha]") {
        using Editor::EditorVulkanXrProjectionAlphaState;
        using Editor::EditorXrPremultipliedProjection;
        using Editor::EditorXrProjectionAlpha;
        using Render::MultiviewPublication;
        using Render::RenderTextureHandle;
        using Render::RenderTimelinePoint;
        static_assert(!std::is_default_constructible_v<EditorXrPremultipliedProjection>);
        static_assert(
            !std::is_constructible_v<EditorXrPremultipliedProjection, MultiviewPublication, RenderTextureHandle, RenderTimelinePoint>);

        const MultiviewPublication publication{11, 17, 19, 23, 29, 31};
        const RenderTextureHandle color{Render::RenderResourceOwnerId{37}, 3, 5};
        const RenderTimelinePoint submitted{{41}, 3};
        EditorVulkanXrProjectionAlphaState alpha;

        alpha.Bind(EditorXrProjectionAlpha::Opaque, publication, color);
        CHECK_FALSE(alpha.IsPremultiplied());
        alpha.Submit(submitted);
        CHECK_FALSE(alpha.Take(publication, color, submitted));

        alpha.Bind(EditorXrProjectionAlpha::Premultiplied, publication, color);
        REQUIRE(alpha.IsPremultiplied());
        CHECK_FALSE(alpha.Take(publication, color, submitted));  // Successful bind is not a queue submission.
        alpha.Submit(submitted);
        CHECK_FALSE(alpha.Take(publication, color, {}));
        CHECK_FALSE(alpha.Take(publication, color, {{41}, 2}));
        CHECK_FALSE(alpha.Take(publication, color, {{42}, 3}));
        auto stale = publication;
        ++stale.originRevision;
        CHECK_FALSE(alpha.Take(stale, color, submitted));
        CHECK_FALSE(alpha.Take(publication, {color.owner, color.slot, color.generation + 1}, submitted));
        CHECK(alpha.Take(publication, color, submitted) == submitted);
        CHECK_FALSE(alpha.Take(publication, color, submitted));

        alpha.Bind(EditorXrProjectionAlpha::Premultiplied, publication, color);
        alpha.Submit(submitted);
        alpha.Reset();  // The viewport uses this on acquisition replacement, graph loss and queue failure.
        CHECK_FALSE(alpha.Take(publication, color, submitted));
        ++stale.frame;
        alpha.Bind(EditorXrProjectionAlpha::Premultiplied, stale, color);
        CHECK_FALSE(alpha.Take(stale, color, submitted));
    }
}  // namespace Horo::Tests
