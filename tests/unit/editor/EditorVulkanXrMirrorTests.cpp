/** @file EditorVulkanXrMirrorTests.cpp
 * @brief CPU regressions over the actual private viewport's fixed mirror-receipt storage.
 */
#include "EditorVulkanXrMirrorProjection.h"

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
}  // namespace Horo::Tests
