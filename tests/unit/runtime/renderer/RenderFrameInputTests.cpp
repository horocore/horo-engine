#include "RenderFrameInputInternal.h"
#include "RenderParallelTestGeometry.h"
#include "RenderPayloadCopyInternal.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <type_traits>

namespace Horo::Render::Detail {
    static_assert(!std::is_copy_constructible_v<CapturedRenderPass>);
    static_assert(!std::is_move_constructible_v<CapturedRenderFrame>);

    TEST_CASE("Captured pass ownership rebinds short material strings and every mesh span across reallocation and assignment",
              "[unit][renderer][parallel][capture][lifetime]") {
        CapturedRenderPass original;
        original.descriptor = {.id = {1}, .staticMesh = StaticMeshPassDescriptor{.target = {{1}, 1, 1}, .extent = {8, 8}}};
        original.meshes.emplace_back();
        original.meshes[0].vertices = Test::MakeParallelTriangle();
        original.meshes[0].indices = {0, 1, 2};
        const RenderMeshSourceHandle mesh{{7}, 1};
        original.resources.push_back({.handle = mesh, .localBounds = {{0, 0, 0}, {1, 1, 0}}});
        original.instances = {{.mesh = mesh}, {.mesh = mesh}};
        original.materials = {"short", "long-material-with-owned-heap-storage-and-no-producer-lifetime"};
        original.Rebind();
        std::vector<CapturedRenderPass> passes;
        passes.push_back(std::move(original));
        CHECK_FALSE(original.descriptor.staticMesh.has_value());
        for (std::size_t index = 0; index < 32; ++index)
            passes.emplace_back();
        CapturedRenderPass assigned;
        assigned = std::move(passes.front());
        passes.clear();
        REQUIRE(assigned.descriptor.staticMesh.has_value());
        const auto &scene = assigned.descriptor.staticMesh->scene;
        CHECK(scene.meshResources.data() == assigned.resources.data());
        CHECK(scene.meshResources[0].vertices.data() == assigned.meshes[0].vertices.data());
        CHECK(scene.meshResources[0].indices.data() == assigned.meshes[0].indices.data());
        CHECK(scene.instances.data() == assigned.instances.data());
        CHECK(scene.instances[0].material.value == "short");
        CHECK(scene.instances[0].material.value.data() == assigned.materials[0].data());
        CHECK(scene.instances[1].material.value.data() == assigned.materials[1].data());
        REQUIRE(ValidateCapturedRenderPass(assigned, {}).HasValue());
        assigned.meshes[0].indices[0] = 99;
        REQUIRE(ValidateCapturedRenderPass(assigned, {}).HasError());
    }

    /** @brief Cancels deterministically from an element copy rather than a timing-dependent sleeping thread. */
    struct CancelDuringCopy final {
        CancellationSource *cancellation{};
        std::size_t *copied{};
        CancelDuringCopy() = default;

        CancelDuringCopy(const CancelDuringCopy &source) : cancellation(source.cancellation), copied(source.copied) {
            if (++*copied == 300)
                cancellation->RequestCancellation();
        }

        CancelDuringCopy &operator=(const CancelDuringCopy &) = default;
    };

    TEST_CASE("Capture payload cancellation interrupts within one admitted chunk and never reports partial success",
              "[unit][renderer][parallel][capture][cancellation]") {
        CancellationSource cancellation;
        std::size_t copied = 0;
        std::vector<CancelDuringCopy> source(2'048);
        for (auto &element : source) {
            element.cancellation = &cancellation;
            element.copied = &copied;
        }
        std::vector<CancelDuringCopy> destination;
        const auto result = CopyCapturedPayload(destination, std::span<const CancelDuringCopy>{source}, cancellation.Token());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.parallel_work.cancelled");
        CHECK(copied >= 300);
        CHECK(copied <= 300 + 4'096 / sizeof(CancelDuringCopy));
        CHECK(destination.size() < source.size());
    }

    TEST_CASE("Whole frame capture checks hard envelopes cancellation and duplicate IDs before publishing",
              "[unit][renderer][parallel][capture][admission]") {
        const std::array passes{RenderPassDescriptor{.id = {1}}, RenderPassDescriptor{.id = {2}}};
        const auto accepted = CaptureRenderFrameInputs({1}, passes, {}, {});
        REQUIRE(accepted.HasValue());
        CHECK(accepted.Value()->passes.size() == 2);
        CHECK(accepted.Value()->chargedBytes <= RenderParallelWorkLimits{}.maximumBytes);
        REQUIRE(CaptureRenderFrameInputs({}, passes, {}, {}).HasError());
        REQUIRE(CaptureRenderFrameInputs({1}, passes, {.maximumPasses = 1}, {}).HasError());
        REQUIRE(CaptureRenderFrameInputs({1}, passes, {.maximumBytes = 1}, {}).HasError());
        REQUIRE(CaptureRenderFrameInputs({1}, passes, {.maximumBytes = RenderParallelWorkLimits::HardMaximumBytes + 1}, {}).HasError());
        const std::array duplicates{passes[0], passes[0]};
        REQUIRE(CaptureRenderFrameInputs({1}, duplicates, {}, {}).HasError());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        const auto cancelled = CaptureRenderFrameInputs({1}, passes, {}, cancellation.Token());
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == "render.parallel_work.cancelled");
    }

    TEST_CASE("Owned material byte copying uses the admitted chunk path and rejects cancellation before publication",
              "[unit][renderer][parallel][capture][material]") {
        const std::string producer(8'193, 'm');
        const std::span<const char> bytes{producer.data(), producer.size()};
        std::string captured;
        REQUIRE(CopyCapturedPayload(captured, bytes, {}).HasValue());
        CHECK(captured == producer);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        std::string cancelled;
        const auto result = CopyCapturedPayload(cancelled, bytes, cancellation.Token());
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.parallel_work.cancelled");
        CHECK(cancelled.empty());
    }
}  // namespace Horo::Render::Detail
