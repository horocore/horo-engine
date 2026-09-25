#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderCapabilities.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

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
        snapshot.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] =
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
    CHECK(snapshot.Supports(RenderTextureDescriptor{.extent = {128, 128},
                                                    .format = RenderTextureFormat::Depth32Float,
                                                    .sampleCount = 1,
                                                    .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment}));
}

TEST_CASE("Render capability bitsets cover every public capability and reject reserved values", "[unit][runtime][renderer][capabilities]") {
    RenderCapabilitySet capabilities;
    for (std::uint16_t value = 0; value < RenderCapabilitySet::CapabilityCount; ++value) {
        capabilities.Enable(static_cast<RenderCapability>(value));
    }

    REQUIRE(capabilities.IsValid());
    CHECK(capabilities.bits == RenderCapabilitySet::KnownBits);
    for (std::uint16_t value = 0; value < RenderCapabilitySet::CapabilityCount; ++value) {
        CHECK(capabilities.Supports(static_cast<RenderCapability>(value)));
    }

    const auto invalid = RenderCapability::Count;
    CHECK(RenderCapabilitySet::Bit(invalid) == 0);
    CHECK_FALSE(capabilities.Supports(invalid));
    capabilities.Enable(invalid);
    CHECK(capabilities.bits == RenderCapabilitySet::KnownBits);

    if constexpr (RenderCapabilitySet::CapabilityCount < std::numeric_limits<std::uint16_t>::digits) {
        const auto firstReservedBit = static_cast<std::uint16_t>(std::uint32_t{1} << RenderCapabilitySet::CapabilityCount);
        capabilities.bits = static_cast<std::uint16_t>(RenderCapabilitySet::KnownBits | firstReservedBit);
        CHECK_FALSE(capabilities.IsValid());
    }
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

TEST_CASE("Render resource admission diagnostics cover typed usage and limit failures", "[unit][runtime][renderer][capabilities]") {
    const RenderCapabilitySnapshot snapshot = MakeSnapshot();
    const RenderBufferDescriptor buffer{.byteSize = 65,
                                        .usage = RenderBufferUsage::Vertex | RenderBufferUsage::Index | RenderBufferUsage::CopySource |
                                                 RenderBufferUsage::CopyDestination | RenderBufferUsage::Uniform |
                                                 RenderBufferUsage::Storage | RenderBufferUsage::Indirect,
                                        .access = RenderBufferAccess::HostVisible};
    CHECK(DescribeRenderBufferRequest(buffer) == "buffer bytes=65 usage=vertex|index|copy-source|copy-destination|uniform|storage|indirect "
                                                 "access=host-visible");
    CHECK(DescribeRenderBufferAdmissionFailure(buffer, snapshot).find("size exceeds max 64 bytes") != std::string::npos);

    const RenderTextureDescriptor texture{.dimension = RenderTextureDimension::TwoD,
                                          .extent = {64, 32},
                                          .format = RenderTextureFormat::Rgba8Unorm,
                                          .mipCount = 1,
                                          .layerCount = 1,
                                          .sampleCount = 4,
                                          .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment |
                                                   RenderTextureUsage::CopySource | RenderTextureUsage::CopyDestination |
                                                   RenderTextureUsage::Storage,
                                          .depth = 1};
    CHECK(DescribeRenderTextureRequest(texture).find("usage=sampled|attachment|copy-source|copy-destination|storage") != std::string::npos);
    const std::string textureFailure = DescribeRenderTextureAdmissionFailure(texture, snapshot);
    CHECK(textureFailure.find("usage unavailable for rgba8-unorm") != std::string::npos);
    CHECK(textureFailure.find("sample count 4 unavailable (supported: 1|2)") != std::string::npos);

    const RenderTextureDescriptor noUsage{.extent = {16, 16},
                                          .format = RenderTextureFormat::Depth24Stencil8,
                                          .usage = RenderTextureUsage::Sampled};
    CHECK(DescribeRenderTextureAdmissionFailure(noUsage, snapshot).find("depth24-stencil8 has no admitted usages") != std::string::npos);

    const RenderTextureDescriptor unknownFormat{.extent = {16, 16},
                                                .format = static_cast<RenderTextureFormat>(255),
                                                .usage = RenderTextureUsage::Sampled};
    CHECK(DescribeRenderTextureAdmissionFailure(unknownFormat, snapshot).find("format is unknown") != std::string::npos);

    const RenderBufferDescriptor emptyBuffer{};
    RenderCapabilitySnapshot unavailable = snapshot;
    unavailable.features = {};
    unavailable.limits.maxBufferBytes = 0;
    const std::string emptyBufferFailure = DescribeRenderBufferAdmissionFailure(emptyBuffer, unavailable);
    CHECK(emptyBufferFailure.find("descriptor is structurally invalid") != std::string::npos);
    CHECK(emptyBufferFailure.find("buffer resources are unavailable") != std::string::npos);
    CHECK(emptyBufferFailure.find("no buffer bytes are admitted") != std::string::npos);
}
