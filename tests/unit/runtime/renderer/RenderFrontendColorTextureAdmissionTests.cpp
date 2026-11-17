#include "RenderFrontendTestSupport.h"

#include <array>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;
    constexpr auto ColorRoles = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource |
                                RenderTextureUsage::CopyDestination;
    constexpr std::array ColorFormats{RenderTextureFormat::Rgba8Unorm, RenderTextureFormat::Bgra8Unorm, RenderTextureFormat::R32Float,
                                      RenderTextureFormat::Rgba16Float, RenderTextureFormat::Rgba32Float};

    /** @brief CPU metadata only; this never represents a native format probe or native pixel result. */
    RenderCapabilitySnapshot AdvertisedColors() {
        RenderCapabilitySnapshot support;
        support.deviceIncarnation = std::numeric_limits<std::uint64_t>::max();  // Backend must replace this borrowed identity.
        support.capabilityRevision = 3;
        support.synthetic = true;
        support.features.Enable(RenderCapability::TextureResources);
        support.limits.maxTextureDimension2D = 128;
        support.formats.sampleCountMask = std::uint64_t{1} << 1;
        for (const auto format : ColorFormats)
            support.formats.usages[static_cast<std::size_t>(format)] = ColorRoles;
        return support;
    }

    /** @brief Configures an explicit snapshot before the real tracking-backend initialization acquires its identity. */
    std::unique_ptr<RenderFrontend> FrontendWith(RenderCapabilitySnapshot support = AdvertisedColors()) {
        lifecycleState = {};
        lifecycleState.syntheticCapabilities = support;
        return CreateTrackingFrontend();
    }

    /** @brief Constructs a finite ordinary color descriptor using the actual public contract. */
    RenderTextureDescriptor Color(RenderTextureFormat format = RenderTextureFormat::R32Float, RenderTextureUsage usage = ColorRoles) {
        return {.extent = {4, 4}, .format = format, .usage = usage};
    }

    /** @brief Proves rejection before cost queries, upload admission and memory reservation. */
    void NoAdmission(const RenderFrontend &frontend) {
        CHECK(lifecycleState.textureMemoryCostQueries == 0);
        CHECK(lifecycleState.createTextureCount == 0);
        CHECK(frontend.UploadSnapshot().pendingRequests == 0);
        const auto memory = frontend.MemorySnapshot();
        CHECK(memory.reservationCount == 0);
        CHECK(memory.reservedUnallocatedBytes == 0);
        CHECK(memory.committedBackingBytes == 0);
    }

    /** @brief Requires a typed unsupported result while preserving all allocation and queue state. */
    void Unsupported(RenderFrontend &frontend, const RenderTextureDescriptor &descriptor) {
        const auto result = frontend.CreateTexture(descriptor);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        NoAdmission(frontend);
    }

    /** @brief Exercises queued publication and complete release for one actual admitted descriptor. */
    void AdmitAndRelease(RenderFrontend &frontend, const RenderTextureDescriptor &descriptor) {
        REQUIRE(frontend.Capabilities().support.Supports(descriptor));
        const auto result = frontend.CreateTexture(descriptor);
        REQUIRE(result.HasValue());
        CHECK(frontend.ResourceState(result.Value().handle).Value() == RenderResourceState::Pending);
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        CHECK(frontend.ResourceState(result.Value().handle).Value() == RenderResourceState::Ready);
        REQUIRE(frontend.ResourceOperationResult(result.Value().operation).HasValue());
        REQUIRE(frontend.ReleaseTexture(result.Value().handle).HasValue());
        CHECK(frontend.MemorySnapshot().reservationCount == 0);
        CHECK(frontend.MemorySnapshot().committedBackingBytes == 0);
    }
}  // namespace

TEST_CASE("Tracking color capabilities retain the actual acquired device incarnation", "[renderer][resource][color-admission]") {
    const auto advertised = AdvertisedColors();
    auto frontend = FrontendWith(advertised);
    const auto &support = frontend->Capabilities().support;
    REQUIRE(support.IsValid());
    CHECK(support.synthetic);
    CHECK(support.deviceIncarnation != advertised.deviceIncarnation);
    CHECK(support.deviceIncarnation > 0);
    CHECK(support.capabilityRevision == advertised.capabilityRevision);
}

TEST_CASE("Frontend admits exact advertised color formats and ordinary copy roles", "[renderer][resource][color-admission]") {
    auto frontend = FrontendWith();
    constexpr std::array roles{RenderTextureUsage::Sampled, RenderTextureUsage::RenderAttachment, RenderTextureUsage::CopySource,
                               RenderTextureUsage::CopyDestination, ColorRoles};
    for (const auto format : ColorFormats)
        for (const auto role : roles) {
            INFO(static_cast<unsigned>(format));
            INFO(static_cast<unsigned>(role));
            AdmitAndRelease(*frontend, Color(format, role));
        }
}

TEST_CASE("Frontend color additions require a valid advertised capability snapshot", "[renderer][resource][color-admission]") {
    lifecycleState = {};
    auto frontend = CreateTrackingFrontend();
    Unsupported(*frontend, Color(RenderTextureFormat::R32Float, RenderTextureUsage::Sampled));
    Unsupported(*frontend, Color(RenderTextureFormat::Bgra8Unorm, RenderTextureUsage::Sampled));
    Unsupported(*frontend, Color(RenderTextureFormat::Rgba8Unorm, RenderTextureUsage::CopySource));
}

TEST_CASE("Frontend depth observation admits only the exact advertised attachment copy source",
          "[renderer][resource][depth-copy-admission]") {
    auto support = AdvertisedColors();
    constexpr auto roles = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource;
    support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = roles;
    auto frontend = FrontendWith(support);
    AdmitAndRelease(*frontend, Color(RenderTextureFormat::Depth32Float, roles));
}

TEST_CASE("Frontend depth observation rejects unsupported facts and expanded descriptors before allocation",
          "[renderer][resource][depth-copy-admission]") {
    auto support = AdvertisedColors();
    constexpr auto roles = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource;
    support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = roles;
    auto descriptor = Color(RenderTextureFormat::Depth32Float, roles);
    SECTION("missing copy source fact") {
        support.formats.usages[static_cast<std::size_t>(descriptor.format)] = RenderTextureUsage::RenderAttachment;
    }
    SECTION("missing attachment fact") {
        support.formats.usages[static_cast<std::size_t>(descriptor.format)] = RenderTextureUsage::CopySource;
    }
    SECTION("invalid snapshot") {
        support.capabilityRevision = 0;
    }
    SECTION("another depth format") {
        descriptor.format = RenderTextureFormat::Depth24Stencil8;
        support.formats.usages[static_cast<std::size_t>(descriptor.format)] = roles;
    }
    SECTION("copy destination is separate") {
        descriptor.usage = roles | RenderTextureUsage::CopyDestination;
        support.formats.usages[static_cast<std::size_t>(descriptor.format)] = descriptor.usage;
    }
    SECTION("storage is separate") {
        descriptor.usage = roles | RenderTextureUsage::Storage;
        support.formats.usages[static_cast<std::size_t>(descriptor.format)] = descriptor.usage;
    }
    auto frontend = FrontendWith(support);
    Unsupported(*frontend, descriptor);
}

TEST_CASE("Frontend depth observation keeps texture shape and initial upload closed", "[renderer][resource][depth-copy-admission]") {
    auto support = AdvertisedColors();
    constexpr auto roles = RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopySource;
    support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Depth32Float)] = roles;
    support.formats.sampleCountMask |= std::uint64_t{1} << 2;
    auto descriptor = Color(RenderTextureFormat::Depth32Float, roles);
    SECTION("3D") {
        descriptor.dimension = RenderTextureDimension::ThreeD;
        descriptor.depth = 2;
    }
    SECTION("mips") {
        descriptor.mipCount = 2;
    }
    SECTION("layers") {
        descriptor.layerCount = 2;
    }
    SECTION("MSAA") {
        descriptor.sampleCount = 2;
    }
    SECTION("complete initial bytes") {
        auto frontend = FrontendWith(support);
        const std::array<std::byte, 64> bytes{};
        const auto result = frontend->CreateTexture(descriptor, bytes);
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
        NoAdmission(*frontend);
        return;
    }
    auto frontend = FrontendWith(support);
    const auto result = frontend->CreateTexture(descriptor);
    REQUIRE(result.HasError());
    NoAdmission(*frontend);
}

TEST_CASE("Frontend requires every requested color usage bit to be independently advertised", "[renderer][resource][color-admission]") {
    auto support = AdvertisedColors();
    for (const auto format : ColorFormats)
        support.formats.usages[static_cast<std::size_t>(format)] =
            RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment | RenderTextureUsage::CopyDestination;
    auto frontend = FrontendWith(support);
    for (const auto format : ColorFormats)
        Unsupported(*frontend, Color(format, RenderTextureUsage::Sampled | RenderTextureUsage::CopySource));
}

TEST_CASE("Frontend rejects invalid revision feature and sample support before color reservation",
          "[renderer][resource][color-admission]") {
    auto support = AdvertisedColors();
    SECTION("missing revision") {
        support.capabilityRevision = 0;
    }
    SECTION("missing resource feature") {
        support.features = {};
    }
    SECTION("missing sample support") {
        support.formats.sampleCountMask = 0;
    }
    SECTION("invalid bounded limits") {
        support.limits.maxFramesInFlight = 9;
    }
    auto frontend = FrontendWith(support);
    Unsupported(*frontend, Color());
}

TEST_CASE("Frontend color path keeps mip layer sample storage and format extensions closed", "[renderer][resource][color-admission]") {
    auto support = AdvertisedColors();
    support.formats.sampleCountMask |= std::uint64_t{1} << 2;
    support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::R32Float)] = ColorRoles | RenderTextureUsage::Storage;
    support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::R16Float)] = ColorRoles;
    auto frontend = FrontendWith(support);
    std::array<RenderTextureDescriptor, 5> rejected{Color(), Color(), Color(), Color(), Color(RenderTextureFormat::R16Float)};
    rejected[0].mipCount = 2;
    rejected[1].layerCount = 2;
    rejected[2].sampleCount = 2;
    rejected[3].usage = RenderTextureUsage::Storage;
    for (const auto &descriptor : rejected) {
        REQUIRE(descriptor.IsValid());
        REQUIRE(frontend->Capabilities().support.Supports(descriptor));
        Unsupported(*frontend, descriptor);
    }
}

TEST_CASE("Frontend color path observes advertised extent limits and rejects foreign dimensions", "[renderer][resource][color-admission]") {
    auto frontend = FrontendWith();
    auto tooWide = Color();
    tooWide.extent.width = 129;
    auto tooHigh = Color();
    tooHigh.extent.height = 129;
    auto one = Color();
    one.dimension = RenderTextureDimension::OneD;
    one.extent.height = 1;
    auto three = Color();
    three.dimension = RenderTextureDimension::ThreeD;
    three.depth = 2;
    for (const auto &descriptor : std::array{tooWide, tooHigh, one, three}) {
        REQUIRE(descriptor.IsValid());
        Unsupported(*frontend, descriptor);
    }
}

TEST_CASE("Frontend generic texture-resource capability remains authoritative over color format metadata",
          "[renderer][resource][color-admission]") {
    lifecycleState = {};
    lifecycleState.syntheticCapabilities = AdvertisedColors();
    lifecycleState.supportsTextureResources = false;
    auto frontend = CreateTrackingFrontend();
    REQUIRE(frontend->Capabilities().support.Supports(Color()));
    Unsupported(*frontend, Color());
}

TEST_CASE("Frontend admitted color uploads publish Ready views and retain exact parent ownership",
          "[renderer][resource][color-admission]") {
    for (const auto format : std::array{RenderTextureFormat::R32Float, RenderTextureFormat::Bgra8Unorm}) {
        auto frontend = FrontendWith();
        auto descriptor = Color(format);
        descriptor.extent = {2, 2};
        std::array<std::byte, 16> input{std::byte{0x3a}};
        const auto original = input;
        const auto texture = frontend->CreateTexture(descriptor, input);
        REQUIRE(texture.HasValue());
        input[0] = std::byte{0xff};
        CHECK(frontend->ResourceState(texture.Value().handle).Value() == RenderResourceState::Pending);
        const auto early =
            frontend->CreateTextureView({.texture = texture.Value().handle, .format = format, .aspect = RenderTextureAspect::Color});
        REQUIRE(early.HasError());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(frontend->ResourceState(texture.Value().handle).Value() == RenderResourceState::Ready);
        CHECK(lifecycleState.lastTextureInitialData == std::vector<std::byte>(original.begin(), original.end()));
        const auto view =
            frontend->CreateTextureView({.texture = texture.Value().handle, .format = format, .aspect = RenderTextureAspect::Color});
        REQUIRE(view.HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        CHECK(frontend->ResourceState(view.Value().handle).Value() == RenderResourceState::Ready);
        REQUIRE(frontend->ReleaseTexture(texture.Value().handle).HasValue());
        CHECK(lifecycleState.destroyTextureCount == 0);
        REQUIRE(frontend->ReleaseTextureView(view.Value().handle).HasValue());
        CHECK(lifecycleState.destroyTextureViewCount == 1);
        CHECK(lifecycleState.destroyTextureCount == 1);
        CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    }
}

TEST_CASE("Frontend admitted color views reject incompatible format and foreign parent handles", "[renderer][resource][color-admission]") {
    auto owner = FrontendWith();
    const auto texture = owner->CreateTexture(Color());
    REQUIRE(texture.HasValue());
    REQUIRE(owner->ProcessResourceRequests().HasValue());
    CHECK(owner
              ->CreateTextureView(
                  {.texture = texture.Value().handle, .format = RenderTextureFormat::Rgba32Float, .aspect = RenderTextureAspect::Color})
              .HasError());
    auto other = CreateTrackingFrontend();
    CHECK(other
              ->CreateTextureView(
                  {.texture = texture.Value().handle, .format = RenderTextureFormat::R32Float, .aspect = RenderTextureAspect::Color})
              .HasError());
    CHECK(other->UploadSnapshot().pendingRequests == 0);
    CHECK(other->MemorySnapshot().reservationCount == 0);
    CHECK(lifecycleState.createTextureViewCount == 0);
    REQUIRE(owner->ReleaseTexture(texture.Value().handle).HasValue());
}

TEST_CASE("Frontend color cost failures preserve backend typed errors and roll back all reservations",
          "[renderer][resource][color-admission]") {
    auto frontend = FrontendWith();
    const auto expected = MakeError(RenderMemoryBudgetErrors::UnsupportedAllocation, "Synthetic color cost query rejected exact usage.");
    lifecycleState.textureMemoryCostFailure = expected;
    const auto result = frontend->CreateTexture(Color());
    REQUIRE(result.HasError());
    CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
    CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    CHECK(result.ErrorValue().message == expected.message);
    CHECK(lifecycleState.textureMemoryCostQueries == 1);
    CHECK(lifecycleState.createTextureCount == 0);
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->MemorySnapshot().reservationCount == 0);
    CHECK(frontend->MemorySnapshot().reservedUnallocatedBytes == 0);
    CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
    lifecycleState.textureMemoryCostFailure.reset();
    AdmitAndRelease(*frontend, Color());
}

TEST_CASE("Frontend exposure-only capabilities admit closed device storage without VFX simulation",
          "[renderer][resource][color-admission][exposure]") {
    auto support = AdvertisedColors();
    support.features.Enable(RenderCapability::ExposureGpu);
    support.features.Enable(RenderCapability::BufferResources);
    support.limits.maxBufferBytes = 4096;
    auto frontend = FrontendWith(support);
    REQUIRE(frontend->Capabilities().support.features.Supports(RenderCapability::ExposureGpu));
    CHECK_FALSE(frontend->Capabilities().support.features.Supports(RenderCapability::VfxGpuSimulation));
    const RenderBufferDescriptor mixed{.byteSize = 32,
                                       .usage = RenderBufferUsage::Storage | RenderBufferUsage::Uniform,
                                       .access = RenderBufferAccess::DeviceLocal};
    const auto denied = frontend->CreateBuffer(mixed, {});
    REQUIRE(denied.HasError());
    CHECK(denied.ErrorValue().code.Value() == "render.frontend.resource.unsupported");
    CHECK(frontend->UploadSnapshot().pendingRequests == 0);
    CHECK(frontend->MemorySnapshot().reservationCount == 0);
    CHECK(lifecycleState.createBufferCount == 0);
    const RenderBufferDescriptor descriptor{.byteSize = 32,
                                            .usage = RenderBufferUsage::Storage | RenderBufferUsage::CopySource |
                                                     RenderBufferUsage::CopyDestination,
                                            .access = RenderBufferAccess::DeviceLocal};
    REQUIRE(frontend->Capabilities().support.Supports(descriptor));
    const auto buffer = frontend->CreateBuffer(descriptor, {});
    REQUIRE(buffer.HasValue());
    CHECK(frontend->ResourceState(buffer.Value().handle).Value() == RenderResourceState::Pending);
    REQUIRE(frontend->ProcessResourceRequests().HasValue());
    CHECK(frontend->ResourceState(buffer.Value().handle).Value() == RenderResourceState::Ready);
    REQUIRE(frontend->ResourceOperationResult(buffer.Value().operation).HasValue());
    CHECK(lifecycleState.createBufferCount == 1);
    REQUIRE(frontend->ReleaseBuffer(buffer.Value().handle).HasValue());
    CHECK(frontend->MemorySnapshot().reservationCount == 0);
    CHECK(frontend->MemorySnapshot().committedBackingBytes == 0);
}
