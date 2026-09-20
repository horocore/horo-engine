#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>

namespace {
    using namespace Horo::Render;

    RenderCapabilitySnapshot MakeSnapshot() {
        RenderCapabilitySnapshot snapshot{
            .deviceIncarnation = 4,
            .capabilityRevision = 9,
            .features = {},
            .queues = {.graphics = true, .compute = false, .copy = true, .present = false},
            .limits = {.maxBufferBytes = 64,
                       .maxTextureDimension2D = 128,
                       .maxColorAttachments = 4,
                       .maxVertexAttributes = 8,
                       .maxFramesInFlight = 3},
            .formats = {},
        };
        snapshot.features.Enable(RenderCapability::BufferResources);
        snapshot.features.Enable(RenderCapability::TextureResources);
        snapshot.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Rgba8Unorm)] =
            RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
        snapshot.formats.sampleCountMask = (std::uint64_t{1} << 1U) | (std::uint64_t{1} << 2U);
        return snapshot;
    }
}  // namespace

TEST_CASE("Render capability snapshots keep feature, queue, limit, and format predicates typed",
          "[unit][runtime][renderer][capabilities]") {
    const RenderCapabilitySnapshot snapshot = MakeSnapshot();

    REQUIRE(snapshot.IsValid());
    CHECK(snapshot.features.Supports(RenderCapability::BufferResources));
    CHECK_FALSE(snapshot.features.Supports(RenderCapability::Compute));
    CHECK(snapshot.queues.Supports(RenderQueueKind::Graphics));
    CHECK_FALSE(snapshot.queues.Supports(RenderQueueKind::Present));
    CHECK(snapshot.Supports(RenderBufferDescriptor{.byteSize = 64, .usage = RenderBufferUsage::Vertex}));
    CHECK_FALSE(snapshot.Supports(RenderBufferDescriptor{.byteSize = 65, .usage = RenderBufferUsage::Vertex}));
    CHECK(snapshot.Supports(RenderTextureDescriptor{.extent = {128, 128},
                                                    .format = RenderTextureFormat::Rgba8Unorm,
                                                    .sampleCount = 2,
                                                    .usage = RenderTextureUsage::Sampled}));
    CHECK_FALSE(snapshot.Supports(RenderTextureDescriptor{.extent = {128, 128},
                                                          .format = RenderTextureFormat::Rgba8Unorm,
                                                          .sampleCount = 4,
                                                          .usage = RenderTextureUsage::Sampled}));
    CHECK_FALSE(snapshot.Supports(
        RenderTextureDescriptor{.extent = {129, 128}, .format = RenderTextureFormat::Rgba8Unorm, .usage = RenderTextureUsage::Sampled}));
}

TEST_CASE("Null backend publishes a synthetic bounded capability snapshot", "[unit][runtime][renderer][capabilities]") {
    RenderBackendRegistry registry;
    REQUIRE(RegisterNullRenderBackend(registry).HasValue());
    REQUIRE(registry.Seal().HasValue());
    auto backend = registry.Create(RenderBackendId{"null"});
    REQUIRE(backend.HasValue());
    REQUIRE(backend.Value()->Initialize(RenderBackendConfig{}).HasValue());

    const RenderCapabilitySnapshot &snapshot = backend.Value()->Capabilities().support;
    REQUIRE(snapshot.IsValid());
    CHECK(snapshot.synthetic);
    CHECK_FALSE(snapshot.features.Supports(RenderCapability::Presentation));
    CHECK(snapshot.features.Supports(RenderCapability::TextureResources));
    CHECK(snapshot.queues.Supports(RenderQueueKind::Graphics));
    CHECK_FALSE(snapshot.queues.Supports(RenderQueueKind::Present));

    const RenderBufferDescriptor oversized{.byteSize = 256U * 1024U * 1024U + 1U, .usage = RenderBufferUsage::Vertex};
    CHECK(backend.Value()->QueryBufferMemoryCost(oversized).ErrorValue().code.Value() == "render.null.unsupported_resource_operation");
}
