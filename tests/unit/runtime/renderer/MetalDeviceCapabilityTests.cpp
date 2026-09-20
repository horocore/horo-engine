#include "runtime/renderer/modules/metal/MetalDeviceCapabilities.h"
#include "runtime/renderer/modules/metal/MetalRenderBackendErrors.h"

#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::Detail;

    [[nodiscard]] MetalDeviceFacts SupportedFacts() {
        MetalFormatCapabilities formats;
        const auto sampledAttachment = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Rgba8Unorm)] = sampledAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Bgra8Unorm)] = sampledAttachment;
        formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = RenderTextureUsage::RenderAttachment;
        formats.sampleCountMask = (std::uint64_t{1} << 1U) | (std::uint64_t{1} << 4U);
        return {
            .adapter =
                RenderAdapterProperties{
                    .id = RenderAdapterId{"metal:0000000000000001"},
                    .displayName = "Synthetic Apple7 adapter",
                    .kind = RenderAdapterKind::Integrated,
                    .availability = RenderAdapterAvailability::Available,
                    .supportsPresentation = true,
                },
            .discoveryRevision = 7,
            .operatingSystemMajor = 14,
            .architecture = MetalHostArchitecture::Arm64,
            .supportsApple7 = true,
            .commandQueueAvailable = true,
            .maxBufferLength = 1U << 30U,
            .maxTextureDimension2D = 16'384,
            .formats = formats,
        };
    }

    void CheckError(const Result<MetalDeviceCapabilities> &result, const ErrorCodeDescriptor &descriptor) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
        CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
    }
}  // namespace

TEST_CASE("Metal device admission publishes queried identity limits formats and implemented truth",
          "[runtime][renderer][metal][capabilities]") {
    const MetalDeviceFacts facts = SupportedFacts();
    const auto admitted =
        AdmitMetalDevice(facts, {.adapter = facts.adapter.id, .discoveryRevision = facts.discoveryRevision, .requirePresentation = true});
    REQUIRE(admitted.HasValue());
    CHECK(admitted.Value().IsValid());
    CHECK(admitted.Value().adapter.id == facts.adapter.id);
    CHECK(admitted.Value().maxBufferLength == facts.maxBufferLength);
    CHECK(admitted.Value().implemented.supportsOffscreenTargets);
    CHECK(admitted.Value().implemented.support.IsValid());
    CHECK(admitted.Value().implemented.support.features.Supports(RenderCapability::TextureResources));
    CHECK(admitted.Value().implemented.support.queues.Supports(RenderQueueKind::Graphics));
    CHECK_FALSE(admitted.Value().implemented.support.queues.Supports(RenderQueueKind::Compute));
    CHECK(admitted.Value().implemented.support.limits.maxBufferBytes == facts.maxBufferLength);
    CHECK_FALSE(admitted.Value().implemented.supportsCompute);
    CHECK_FALSE(admitted.Value().implemented.supportsTimestampQueries);
    CHECK_FALSE(admitted.Value().implemented.supportsBindlessResources);
    CHECK_FALSE(admitted.Value().implemented.supportsRayTracing);

    const RenderTextureDescriptor supported{
        .extent = {4096, 4096},
        .format = RenderTextureFormat::Rgba8Unorm,
        .sampleCount = 4,
        .usage = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment,
    };
    CHECK(admitted.Value().SupportsTexture(supported));
    auto unsupportedCombination = supported;
    unsupportedCombination.usage = supported.usage | RenderTextureUsage::Storage;
    CHECK_FALSE(admitted.Value().SupportsTexture(unsupportedCombination));
    auto unsupportedSamples = supported;
    unsupportedSamples.sampleCount = 2;
    CHECK_FALSE(admitted.Value().SupportsTexture(unsupportedSamples));
    auto oversized = supported;
    oversized.extent.width = admitted.Value().maxTextureDimension2D + 1;
    CHECK_FALSE(admitted.Value().SupportsTexture(oversized));

    CHECK(admitted.Value().SupportsBuffer(RenderBufferDescriptor{
        .byteSize = static_cast<std::size_t>(admitted.Value().maxBufferLength),
        .usage = RenderBufferUsage::Vertex,
    }));
    CHECK_FALSE(admitted.Value().SupportsBuffer(RenderBufferDescriptor{
        .byteSize = static_cast<std::size_t>(admitted.Value().maxBufferLength) + 1,
        .usage = RenderBufferUsage::Vertex,
    }));
}

TEST_CASE("Metal admission rejects old hosts and architecture-specific family mismatches", "[runtime][renderer][metal][capabilities]") {
    auto facts = SupportedFacts();
    facts.operatingSystemMajor = 13;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::UnsupportedHost);

    facts = SupportedFacts();
    facts.supportsApple7 = false;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::UnsupportedDeviceFamily);

    facts = SupportedFacts();
    facts.architecture = MetalHostArchitecture::X86_64;
    facts.supportsApple7 = false;
    facts.supportsMac2 = true;
    CHECK(AdmitMetalDevice(facts, {}).HasValue());

    facts.supportsMac2 = false;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::UnsupportedDeviceFamily);
}

TEST_CASE("Metal explicit adapter selection is match-or-fail and rejects stale discovery", "[runtime][renderer][metal][capabilities]") {
    const MetalDeviceFacts facts = SupportedFacts();
    CheckError(AdmitMetalDevice(facts, {.adapter = RenderAdapterId{"metal:other"}, .discoveryRevision = 7}),
               MetalBackendErrors::AdapterNotFound);
    CheckError(AdmitMetalDevice(facts, {.adapter = facts.adapter.id, .discoveryRevision = 6}), MetalBackendErrors::StaleAdapterSnapshot);
}

TEST_CASE("Metal admission fails closed for unavailable presentation queue and required formats",
          "[runtime][renderer][metal][capabilities]") {
    auto facts = SupportedFacts();
    facts.adapter.supportsPresentation = false;
    CheckError(AdmitMetalDevice(facts, {.requirePresentation = true}), MetalBackendErrors::PresentationUnsupported);

    facts = SupportedFacts();
    facts.commandQueueAvailable = false;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::CommandQueueCreationFailed);

    facts = SupportedFacts();
    facts.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = RenderTextureUsage::None;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::RequiredFormatUnsupported);
}

TEST_CASE("Metal capability snapshots reject incomplete native facts", "[runtime][renderer][metal][capabilities]") {
    auto facts = SupportedFacts();
    facts.maxBufferLength = 0;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::InvalidDeviceFacts);

    facts = SupportedFacts();
    facts.adapter.availability = RenderAdapterAvailability::Unavailable;
    CheckError(AdmitMetalDevice(facts, {}), MetalBackendErrors::AdapterUnavailable);
}
