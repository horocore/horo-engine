#include "Horo/Runtime/Render/ImageProviderRegistry.h"
#include "Horo/Runtime/Render/NullBackendModule.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "support/renderer/RenderBackendDrainTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    template <typename T> T RequireValue(Result<T> result) {
        INFO((result.HasError() ? result.ErrorValue().message : "supported"));
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    Sha256Digest Digest(std::uint8_t marker) {
        Sha256Digest result;
        result.bytes.front() = marker;
        return result;
    }

    ImageProviderDescriptor Provider(ImageProviderCategory category = ImageProviderCategory::Reconstruction) {
        return {.route = {category, "fixture.provider", "test-mode"}, .api = {1, 2}};
    }

    RenderBackendCapabilities SelectedDevice() {
        RenderBackendCapabilities backend{.backend = RenderBackendId{"vulkan"},
                                          .supportsExplicitGraphSubmissions = true,
                                          .supportsSubmissionObservation = true};
        backend.support.deviceIncarnation = 37;
        backend.support.capabilityRevision = 4;
        backend.support.features.Enable(RenderCapability::NativeReconstruction);
        backend.support.features.Enable(RenderCapability::TextureResources);
        backend.support.queues.compute = true;
        backend.support.limits.maxTextureDimension2D = 256;
        backend.support.formats.sampleCountMask = 1 << 1;
        backend.support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Rgba32Float)] =
            RenderTextureUsage::Sampled | RenderTextureUsage::Storage;
        return backend;
    }
}  // namespace

TEST_CASE("Image provider discovery seals the bounded independent category routes", "[renderer][image-provider]") {
    ImageProviderRegistry providers;
    const auto reconstruction = Provider();
    REQUIRE(providers.Register(Provider(ImageProviderCategory::Latency)).HasValue());
    REQUIRE(providers.Register(reconstruction).HasValue());
    CHECK(providers.Find(reconstruction.route).ErrorValue().code.Value() == ImageProviderRegistryErrors::RegistryUnsealed.code.Value());
    CHECK(providers.Descriptors().empty());
    CHECK(providers.Register(reconstruction).ErrorValue().code.Value() == ImageProviderRegistryErrors::DuplicateRoute.code.Value());
    auto malformed = Provider();
    malformed.route.mode = "../unknown";
    CHECK(providers.Register(malformed).ErrorValue().code.Value() == ImageProviderRegistryErrors::InvalidDescriptor.code.Value());
    malformed = Provider();
    malformed.requiredCookedArtifacts = {Digest(1), Digest(1)};
    CHECK(providers.Register(malformed).ErrorValue().code.Value() == ImageProviderRegistryErrors::InvalidDescriptor.code.Value());

    providers.Seal();
    providers.Seal();
    REQUIRE(providers.Descriptors().size() == 2);
    CHECK(providers.Descriptors().front().route == reconstruction.route);
    CHECK(providers.Descriptors().back().route.category == ImageProviderCategory::Latency);
    const auto unrelated = Provider(ImageProviderCategory::Denoising);
    CHECK(providers.Find(unrelated.route).ErrorValue().code.Value() == ImageProviderRegistryErrors::UnknownRoute.code.Value());
    CHECK(providers.Register(unrelated).ErrorValue().code.Value() == ImageProviderRegistryErrors::RegistrySealed.code.Value());
}

TEST_CASE("Image provider discovery keeps installation separate from runtime compatibility", "[renderer][image-provider]") {
    auto descriptor = Provider();
    descriptor.backend = RenderBackendId{"vulkan"};
    descriptor.queue = RenderQueueKind::Compute;
    descriptor.requiredFeatures.Enable(RenderCapability::NativeReconstruction);
    descriptor.requiresExplicitGraph = true;
    descriptor.requiresCompletionObservation = true;
    descriptor.component = ImageProviderComponent{.package = "fixture.component",
                                                  .version = "2.0.0",
                                                  .variant = "linux-x64",
                                                  .packageManifest = Digest(1),
                                                  .binary = Digest(2),
                                                  .license = "LicenseRef-Test",
                                                  .licenseNotices = Digest(3),
                                                  .sdk = ImageProviderSdk{"fixture-runtime", "3.1", Digest(4)}};
    descriptor.device = ImageProviderDeviceRequirement{RenderAdapterId{"fixture-adapter"}, Digest(5), "fixture-driver", "31.8"};
    descriptor.minimumCookedArtifacts = 1;
    descriptor.requiredCookedArtifacts = {Digest(6)};
    ImageProviderRegistry providers;
    REQUIRE(providers.Register(descriptor).HasValue());
    providers.Seal();

    const std::array textures{
        RenderTextureDescriptor{.extent = {64, 64}, .format = RenderTextureFormat::Rgba32Float, .usage = RenderTextureUsage::Storage}};
    const std::array cooked{Digest(6)};
    auto selected = SelectedDevice();
    auto device = RenderPipelineDeviceIdentity{.backend = selected.backend,
                                               .backendModuleVersion = 1,
                                               .adapter = descriptor.device->adapter,
                                               .deviceCompatibilityDigest = descriptor.device->compatibility,
                                               .driverIdentity = descriptor.device->driverIdentity,
                                               .driverVersion = descriptor.device->driverVersion};
    auto install = ImageProviderInstallation{*descriptor.component, 11, descriptor.component->packageManifest};
    const ImageProviderEvidence fullyAdmitted{.hostApi = {1, 2},
                                              .installation = &install,
                                              .availableSdk = &*descriptor.component->sdk,
                                              .admittedCookedArtifacts = cooked,
                                              .textures = textures};
    const auto admitted = RequireValue(providers.Query(descriptor.route, selected, &device, fullyAdmitted));
    CHECK(admitted.installation == ImageProviderInstallStatus::Verified);
    CHECK(admitted.readiness == ImageProviderReadiness::Ready);
    auto evidence = fullyAdmitted;
    auto expectedInstall = ImageProviderInstallStatus::Verified;
    auto expected = ImageProviderReadiness::Ready;

    SECTION("Uninstalled component") {
        evidence.installation = nullptr;
        expectedInstall = ImageProviderInstallStatus::Missing;
        expected = ImageProviderReadiness::ComponentUnavailable;
    }
    SECTION("Different license notice digest in same package and variant") {
        install.component.licenseNotices = Digest(7);
        expectedInstall = ImageProviderInstallStatus::Mismatch;
        expected = ImageProviderReadiness::ComponentUnavailable;
    }
    SECTION("Exact component still requires its actual SDK runtime") {
        evidence.availableSdk = nullptr;
        expected = ImageProviderReadiness::SdkUnavailable;
    }
    SECTION("Same API major requires the admitted minor") {
        evidence.hostApi.minor = 1;
        expected = ImageProviderReadiness::ApiMismatch;
    }
    SECTION("Wrong actual selected driver revision") {
        device.driverVersion = "31.9";
        expected = ImageProviderReadiness::DeviceMismatch;
    }
    SECTION("A selected renderer name alone cannot authorize a mode") {
        selected.supportsSubmissionObservation = false;
        expected = ImageProviderReadiness::CapabilityUnavailable;
    }
    SECTION("Actual storage texture format outside the selected device support") {
        selected.support.formats.usages[static_cast<std::size_t>(RenderTextureFormat::Rgba32Float)] = RenderTextureUsage::Sampled;
        expected = ImageProviderReadiness::TextureUnsupported;
    }
    SECTION("Missing installed cook never selects another provider") {
        evidence.admittedCookedArtifacts = {};
        expected = ImageProviderReadiness::CookedArtifactMissing;
    }
    const auto result = RequireValue(providers.Query(descriptor.route, selected, &device, evidence));
    CHECK(result.installation == expectedInstall);
    CHECK(result.readiness == expected);
    CHECK(result.deviceIncarnation == selected.support.deviceIncarnation);
    CHECK(result.capabilityRevision == selected.support.capabilityRevision);
    CHECK(result.installRevision == (expectedInstall == ImageProviderInstallStatus::Verified ? install.revision : 0));
}

TEST_CASE("Native reconstruction is discoverable without a vendor or imaginary cooked variants", "[renderer][image-provider]") {
    using namespace Horo::Render;
    auto temporal = RequireValue(DescribeNativeReconstructionImageProvider(ReconstructionKind::Temporal, {}));
    auto spatial = RequireValue(DescribeNativeReconstructionImageProvider(ReconstructionKind::Spatial, {}));
    CHECK(temporal.minimumCookedArtifacts == 2);
    CHECK(spatial.minimumCookedArtifacts == 1);
    CHECK_FALSE(temporal.component.has_value());
    ImageProviderRegistry providers;
    const auto route = temporal.route;
    REQUIRE(providers.Register(std::move(temporal)).HasValue());
    REQUIRE(providers.Register(std::move(spatial)).HasValue());
    providers.Seal();

    const ImageProviderEvidence context{.hostApi = {1, 0}};
    const auto capabilities = SelectedDevice();
    const auto absent = RequireValue(providers.Query(route, capabilities, nullptr, context));
    CHECK(absent.installation == ImageProviderInstallStatus::Linked);
    CHECK(absent.readiness == ImageProviderReadiness::CookedArtifactMissing);

    RenderBackendRegistry backends;
    REQUIRE(RegisterNullRenderBackend(backends).HasValue());
    REQUIRE(backends.Seal().HasValue());
    auto frontend = RequireValue(RenderFrontend::Create(backends, RenderBackendId{"null"}, {}));
    const auto unsupported = RequireValue(frontend->DiscoverImageProvider(providers, route, context));
    CHECK(unsupported.installation == ImageProviderInstallStatus::Linked);
    CHECK(unsupported.readiness == ImageProviderReadiness::CapabilityUnavailable);
    auto frame = RequireValue(frontend->BeginFrame({.frameNumber = 1, .outputExtent = {16, 16}}));
    const auto active = frontend->DiscoverImageProvider(providers, route, context);
    REQUIRE(active.HasError());
    CHECK(active.ErrorValue().code.Value() == ImageProviderRegistryErrors::InvalidQuery.code.Value());
    frame.Cancel();
    Test::RequireDrainedOwner(*frontend);
}
