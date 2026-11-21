#include "Horo/Runtime/Render/ImageProviderRegistry.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderFrontendErrors.h"

#include <algorithm>
#include <new>
#include <string_view>
#include <utility>

namespace Horo::Render {
    namespace {
        constexpr std::size_t MaximumIdentityBytes = 128;
        constexpr std::size_t MaximumDriverBytes = 256;
        const ErrorDomainId Domain{"render.image_provider"};

        /** @brief Validates one portable, path-free route or package identifier. */
        bool ValidId(std::string_view id) noexcept {
            const auto alphanumeric = [](char value) {
                return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9');
            };
            return !id.empty() && id.size() <= MaximumIdentityBytes && alphanumeric(id.front()) && alphanumeric(id.back()) &&
                   std::ranges::all_of(id, [&](char value) {
                return alphanumeric(value) || value == '.' || value == '-' || value == '_';
            });
        }

        /** @brief Preserves exact owner-reported versions without parsing native driver conventions. */
        bool ValidReportedIdentity(std::string_view id, std::size_t maximum) noexcept {
            return !id.empty() && id.size() <= maximum && std::ranges::all_of(id, [](unsigned char value) {
                return value >= 32 && value <= 126;
            });
        }

        /** @brief Rejects a missing digest instead of treating it as wildcard evidence. */
        bool HasDigest(const Sha256Digest &value) noexcept {
            return value != Sha256Digest{};
        }

        /** @brief Validates a category-local route in the persistent product identifier grammar. */
        bool ValidRoute(const ImageProviderRoute &route) noexcept {
            return route.category < ImageProviderCategory::Count && ValidId(route.provider) && ValidId(route.mode);
        }

        /** @brief Checks bounded reviewed SDK data without probing for libraries. */
        bool ValidSdk(const ImageProviderSdk &sdk) noexcept {
            return ValidId(sdk.id) && ValidReportedIdentity(sdk.version, MaximumIdentityBytes) && HasDigest(sdk.binary);
        }

        /** @brief Requires exact reviewed package, binary and notice identities in any packaged declaration. */
        bool ValidComponent(const ImageProviderComponent &component) noexcept {
            return ValidId(component.package) && ValidReportedIdentity(component.version, MaximumIdentityBytes) &&
                   ValidId(component.variant) && HasDigest(component.packageManifest) && HasDigest(component.binary) &&
                   ValidReportedIdentity(component.license, MaximumIdentityBytes) && HasDigest(component.licenseNotices) &&
                   (!component.sdk || ValidSdk(*component.sdk));
        }

        /** @brief Checks the exact backend-provided driver/adapter cache identity without selecting a device. */
        bool ValidDevice(const ImageProviderDeviceRequirement &device) noexcept {
            return device.adapter.IsValid() && HasDigest(device.compatibility) &&
                   ValidReportedIdentity(device.driverIdentity, MaximumDriverBytes) &&
                   ValidReportedIdentity(device.driverVersion, MaximumDriverBytes);
        }

        /** @brief Rejects malformed or ambiguous bounded lists of demanded cooked descriptors. */
        bool ValidArtifacts(std::vector<Sha256Digest> &artifacts) noexcept {
            if (artifacts.size() > ImageProviderRegistry::MaximumArtifacts || !std::ranges::all_of(artifacts, HasDigest))
                return false;
            std::ranges::sort(artifacts);
            return std::ranges::adjacent_find(artifacts) == artifacts.end();
        }

        /** @brief Validates inert supplied resource and evidence bounds before any owner state changes. */
        bool ValidDescriptor(ImageProviderDescriptor &descriptor) noexcept {
            if (!ValidRoute(descriptor.route) || descriptor.api.major == 0 || !descriptor.requiredFeatures.IsValid())
                return false;
            if ((descriptor.backend && !descriptor.backend->IsValid()) ||
                (descriptor.queue && *descriptor.queue > RenderQueueKind::Present))
                return false;
            if ((descriptor.component && !ValidComponent(*descriptor.component)) || (descriptor.device && !ValidDevice(*descriptor.device)))
                return false;
            return descriptor.minimumCookedArtifacts <= ImageProviderRegistry::MaximumArtifacts &&
                   ValidArtifacts(descriptor.requiredCookedArtifacts);
        }

        /** @brief Determines installed component evidence separately from resource/backend support. */
        ImageProviderInstallStatus InstallStatus(const ImageProviderDescriptor &descriptor,
                                                 const ImageProviderInstallation *installation) noexcept {
            using enum ImageProviderInstallStatus;
            if (!descriptor.component)
                return Linked;
            if (installation == nullptr)
                return Missing;
            const auto &wanted = *descriptor.component;
            return installation->revision != 0 && installation->authenticatedManifest == wanted.packageManifest &&
                           installation->component == wanted
                       ? Verified
                       : Mismatch;
        }

        /** @brief Matches the exact selected backend and already reported driver/adapter revision to host policy. */
        bool MatchesDevice(const ImageProviderDeviceRequirement &wanted, const RenderBackendCapabilities &backend,
                           const RenderPipelineDeviceIdentity *device) noexcept {
            return device != nullptr && device->backend == backend.backend && device->adapter == wanted.adapter &&
                   device->deviceCompatibilityDigest == wanted.compatibility && device->driverIdentity == wanted.driverIdentity &&
                   device->driverVersion == wanted.driverVersion;
        }

        /** @brief Checks the selected backend's actual queued work and completion capabilities. */
        bool MatchesOperations(const ImageProviderDescriptor &descriptor, const RenderBackendCapabilities &backend) noexcept {
            const auto &support = backend.support;
            return (support.features.bits & descriptor.requiredFeatures.bits) == descriptor.requiredFeatures.bits &&
                   (!descriptor.queue || support.queues.Supports(*descriptor.queue)) &&
                   (!descriptor.requiresExplicitGraph || backend.supportsExplicitGraphSubmissions) &&
                   (!descriptor.requiresCompletionObservation || backend.supportsSubmissionObservation);
        }

        /** @brief Checks the declared API and host-supplied component/runtime evidence. */
        ImageProviderReadiness HostReadiness(const ImageProviderDescriptor &descriptor, ImageProviderInstallStatus install,
                                             const ImageProviderEvidence &evidence) noexcept {
            using enum ImageProviderReadiness;
            if (evidence.hostApi.major != descriptor.api.major || evidence.hostApi.minor < descriptor.api.minor)
                return ApiMismatch;
            if (install == ImageProviderInstallStatus::Missing || install == ImageProviderInstallStatus::Mismatch)
                return ComponentUnavailable;
            if (descriptor.component && descriptor.component->sdk &&
                (!evidence.availableSdk || *evidence.availableSdk != *descriptor.component->sdk))
                return SdkUnavailable;
            return Ready;
        }

        /** @brief Checks actual selected renderer, device, operation and image support. */
        ImageProviderReadiness DeviceReadiness(const ImageProviderDescriptor &descriptor, const RenderBackendCapabilities &backend,
                                               const RenderPipelineDeviceIdentity *device,
                                               std::span<const RenderTextureDescriptor> textures) noexcept {
            using enum ImageProviderReadiness;
            if (descriptor.backend && *descriptor.backend != backend.backend)
                return BackendMismatch;
            if (descriptor.device && !MatchesDevice(*descriptor.device, backend, device))
                return DeviceMismatch;
            if (!MatchesOperations(descriptor, backend))
                return CapabilityUnavailable;
            if (!std::ranges::all_of(textures, [&](const auto &texture) {
                return backend.support.Supports(texture);
            }))
                return TextureUnsupported;
            return Ready;
        }

        /** @brief Admits the complete required cooked set for this route. */
        bool HasCookedVariants(const ImageProviderDescriptor &descriptor, std::span<const Sha256Digest> cooked) noexcept {
            return descriptor.requiredCookedArtifacts.size() >= descriptor.minimumCookedArtifacts &&
                   std::ranges::all_of(descriptor.requiredCookedArtifacts, [&](const Sha256Digest &required) {
                return std::ranges::find(cooked, required) != cooked.end();
            });
        }
    }  // namespace

    namespace ImageProviderRegistryErrors {
        const ErrorCodeDescriptor InvalidDescriptor{Domain, ErrorCode{"render.image_provider.invalid_descriptor"}, ErrorSeverity::Error,
                                                    "Image provider descriptor is malformed.", "Contribute a bounded canonical route."};
        const ErrorCodeDescriptor InvalidQuery{Domain, ErrorCode{"render.image_provider.invalid_query"}, ErrorSeverity::Error,
                                               "Image provider query is malformed.",
                                               "Supply the selected device and bounded current evidence."};
        const ErrorCodeDescriptor DuplicateRoute{Domain, ErrorCode{"render.image_provider.duplicate_route"}, ErrorSeverity::Error,
                                                 "Image provider route is already declared.", "Register each category route once."};
        const ErrorCodeDescriptor CapacityExceeded{Domain, ErrorCode{"render.image_provider.capacity"}, ErrorSeverity::Error,
                                                   "Image provider route capacity is exhausted.", "Review the admitted product providers."};
        const ErrorCodeDescriptor RegistrySealed{Domain, ErrorCode{"render.image_provider.sealed"}, ErrorSeverity::Error,
                                                 "Image provider registration has closed.",
                                                 "Compose a new product registry before changing providers."};
        const ErrorCodeDescriptor RegistryUnsealed{Domain, ErrorCode{"render.image_provider.unsealed"}, ErrorSeverity::Error,
                                                   "Image provider registry is still open.", "Seal the registry before discovery."};
        const ErrorCodeDescriptor UnknownRoute{Domain, ErrorCode{"render.image_provider.unknown_route"}, ErrorSeverity::Error,
                                               "The exact image provider route is unregistered.",
                                               "Review the configured category and provider."};
        const ErrorCodeDescriptor AllocationFailed{Domain, ErrorCode{"render.image_provider.allocation"}, ErrorSeverity::Error,
                                                   "Image provider descriptor could not be retained.", "Reduce startup memory pressure."};
    }  // namespace ImageProviderRegistryErrors

    /** @copydoc DescribeNativeReconstructionImageProvider */
    Result<ImageProviderDescriptor> DescribeNativeReconstructionImageProvider(ReconstructionKind kind,
                                                                              std::span<const CookedNativeReconstructionKernel> cooked) {
        using Outcome = Result<ImageProviderDescriptor>;
        using Entry = NativeReconstructionEntry;
        if (kind != ReconstructionKind::Spatial && kind != ReconstructionKind::Temporal)
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::InvalidDescriptor));
        if (cooked.size() > static_cast<std::size_t>(Entry::TemporalHistory) + 1)
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::InvalidDescriptor));

        const bool temporal = kind == ReconstructionKind::Temporal;
        const std::array required{temporal ? Entry::TemporalSeed : Entry::Spatial, Entry::TemporalHistory};
        ImageProviderDescriptor descriptor{.route = {ImageProviderCategory::Reconstruction, "horo.reconstruction",
                                                     temporal ? "taau" : "spatial"},
                                           .api = {1, 0},
                                           .queue = RenderQueueKind::Compute,
                                           .requiresExplicitGraph = true,
                                           .requiresCompletionObservation = temporal,
                                           .minimumCookedArtifacts = temporal ? required.size() : 1};
        descriptor.requiredFeatures.Enable(RenderCapability::NativeReconstruction);
        try {
            for (const auto entry : std::span{required}.first(descriptor.minimumCookedArtifacts)) {
                const auto artifact = std::ranges::find(cooked, entry, &CookedNativeReconstructionKernel::entry);
                if (artifact == cooked.end())
                    continue;
                if (std::ranges::find(artifact + 1, cooked.end(), entry, &CookedNativeReconstructionKernel::entry) != cooked.end())
                    return Outcome::Failure(MakeError(ImageProviderRegistryErrors::InvalidDescriptor));
                const auto admitted = AdmitNativeReconstructionShader(entry, artifact->target, artifact->artifact);
                if (admitted.HasError())
                    return Outcome::Failure(admitted.ErrorValue());
                descriptor.requiredCookedArtifacts.push_back(artifact->artifact.artifactKey);
            }
            return Outcome::Success(std::move(descriptor));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::AllocationFailed));
        }
    }

    /** @copydoc ImageProviderRegistry::Register */
    Result<void> ImageProviderRegistry::Register(ImageProviderDescriptor descriptor) {
        using namespace ImageProviderRegistryErrors;
        if (sealed_)
            return Result<void>::Failure(MakeError(RegistrySealed));
        if (!ValidDescriptor(descriptor))
            return Result<void>::Failure(MakeError(InvalidDescriptor));
        if (std::ranges::find(descriptors_, descriptor.route, &ImageProviderDescriptor::route) != descriptors_.end())
            return Result<void>::Failure(MakeError(DuplicateRoute));
        if (descriptors_.size() == MaximumRoutes)
            return Result<void>::Failure(MakeError(CapacityExceeded));
        try {
            descriptors_.push_back(std::move(descriptor));
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AllocationFailed));
        }
        return Result<void>::Success();
    }

    /** @copydoc ImageProviderRegistry::Seal */
    void ImageProviderRegistry::Seal() noexcept {
        if (sealed_)
            return;
        std::ranges::sort(descriptors_, {}, &ImageProviderDescriptor::route);
        sealed_ = true;
    }

    /** @copydoc ImageProviderRegistry::Descriptors */
    std::span<const ImageProviderDescriptor> ImageProviderRegistry::Descriptors() const noexcept {
        return sealed_ ? std::span<const ImageProviderDescriptor>{descriptors_} : std::span<const ImageProviderDescriptor>{};
    }

    /** @copydoc ImageProviderRegistry::Find */
    Result<const ImageProviderDescriptor *> ImageProviderRegistry::Find(const ImageProviderRoute &route) const {
        using Outcome = Result<const ImageProviderDescriptor *>;
        using namespace ImageProviderRegistryErrors;
        if (!sealed_)
            return Outcome::Failure(MakeError(RegistryUnsealed));
        if (!ValidRoute(route))
            return Outcome::Failure(MakeError(InvalidQuery));
        const auto exact = std::ranges::lower_bound(descriptors_, route, {}, &ImageProviderDescriptor::route);
        if (exact == descriptors_.end() || exact->route != route)
            return Outcome::Failure(MakeError(UnknownRoute));
        return Outcome::Success(&*exact);
    }

    /** @copydoc ImageProviderRegistry::Query */
    Result<ImageProviderAvailability> ImageProviderRegistry::Query(const ImageProviderRoute &route,
                                                                   const RenderBackendCapabilities &backend,
                                                                   const RenderPipelineDeviceIdentity *device,
                                                                   const ImageProviderEvidence &evidence) const {
        using Outcome = Result<ImageProviderAvailability>;
        if (!backend.backend.IsValid() || !backend.support.IsValid() || evidence.hostApi.major == 0 ||
            evidence.textures.size() > MaximumTextures || evidence.admittedCookedArtifacts.size() > MaximumArtifacts)
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::InvalidQuery));
        const auto descriptor = Find(route);
        if (descriptor.HasError())
            return Outcome::Failure(descriptor.ErrorValue());
        const auto &exact = *descriptor.Value();
        const auto install = InstallStatus(exact, evidence.installation);
        using enum ImageProviderReadiness;
        auto ready = HostReadiness(exact, install, evidence);
        if (ready == Ready)
            ready = DeviceReadiness(exact, backend, device, evidence.textures);
        if (ready == Ready && !HasCookedVariants(exact, evidence.admittedCookedArtifacts))
            ready = CookedArtifactMissing;
        const auto installRevision = install == ImageProviderInstallStatus::Verified ? evidence.installation->revision : 0;
        return Outcome::Success({install, ready, backend.support.deviceIncarnation, backend.support.capabilityRevision, installRevision});
    }

    /** @copydoc RenderFrontend::DiscoverImageProvider */
    Result<ImageProviderAvailability> RenderFrontend::DiscoverImageProvider(const ImageProviderRegistry &providers,
                                                                            const ImageProviderRoute &route,
                                                                            const ImageProviderEvidence &evidence) const {
        using Outcome = Result<ImageProviderAvailability>;
        if (const auto owner = RequireOpenOwner(); owner.HasError())
            return Outcome::Failure(owner.ErrorValue());
        if (activeFrameScope_ != nullptr)
            return Outcome::Failure(MakeError(ImageProviderRegistryErrors::InvalidQuery));
        const auto availability = providers.Query(route, Capabilities(), nullptr, evidence);
        if (availability.HasError() || availability.Value().readiness != ImageProviderReadiness::DeviceMismatch)
            return availability;
        const auto device = QueryPipelineDeviceIdentity();
        if (device.HasError())
            return Outcome::Failure(device.ErrorValue());
        return providers.Query(route, Capabilities(), &device.Value(), evidence);
    }
}  // namespace Horo::Render
