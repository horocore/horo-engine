#pragma once

/** @file ImageProviderRegistry.h @brief Sealed renderer image-provider discovery and compatibility. */
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

#include <compare>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Horo::Render {
    /** @brief Independently owned image or host scheduling operation types. */
    enum class ImageProviderCategory : std::uint8_t {
        Reconstruction,
        Denoising,
        FrameGeneration,
        Latency,
        Count
    };

    /** @brief Stable category-qualified provider and mode; the IDs contain no native SDK values. */
    struct ImageProviderRoute final {
        ImageProviderCategory category{};
        std::string provider;
        std::string mode;
        auto operator<=>(const ImageProviderRoute &) const noexcept = default;
    };

    /** @brief Required API contract per category, independent of the distributed component version. */
    struct ImageProviderApiVersion final {
        std::uint32_t major{};
        std::uint32_t minor{};
    };

    /** @brief Exact reviewed redistributable SDK binary identity if a provider requires one. */
    struct ImageProviderSdk final {
        std::string id;
        std::string version;
        Sha256Digest binary;
        bool operator==(const ImageProviderSdk &) const noexcept = default;
    };

    /** @brief Exact product component variant and its reviewed binary, license notices and optional SDK identity. */
    struct ImageProviderComponent final {
        std::string package;
        std::string version;
        std::string variant;
        Sha256Digest packageManifest;
        Sha256Digest binary;
        std::string license;
        Sha256Digest licenseNotices;
        std::optional<ImageProviderSdk> sdk;
        bool operator==(const ImageProviderComponent &) const noexcept = default;
    };

    /** @brief Narrow portable evidence copied at the host boundary from a verified installed component. */
    struct ImageProviderInstallation final {
        ImageProviderComponent component;
        std::uint64_t revision{};
        Sha256Digest authenticatedManifest;
    };

    /** @brief Exact selected driver/device compatibility keys admitted by a component's host policy. */
    struct ImageProviderDeviceRequirement final {
        RenderAdapterId adapter;
        Sha256Digest compatibility;
        std::string driverIdentity;
        std::string driverVersion;
    };

    /** @brief Inert provider/variant metadata contributed explicitly by the composition owner. */
    struct ImageProviderDescriptor final {
        ImageProviderRoute route;
        ImageProviderApiVersion api;
        std::optional<RenderBackendId> backend;
        RenderCapabilitySet requiredFeatures;
        std::optional<RenderQueueKind> queue;
        bool requiresExplicitGraph{};
        bool requiresCompletionObservation{};
        std::optional<ImageProviderComponent> component; /**< Absent for an explicitly registered linked component. */
        std::optional<ImageProviderDeviceRequirement> device;
        std::size_t minimumCookedArtifacts{};              /**< Number of required variants, even when absent from the installed cook. */
        std::vector<Sha256Digest> requiredCookedArtifacts; /**< Exact expected variant keys supplied by product composition. */
    };

    /** @brief Machine-local component evidence, independent of resource compatibility or selection. */
    enum class ImageProviderInstallStatus : std::uint8_t {
        Linked,
        Missing,
        Verified,
        Mismatch
    };

    /** @brief Effective support following exact API, installation, device, format and cook admission. */
    enum class ImageProviderReadiness : std::uint8_t {
        Ready,
        ApiMismatch,
        ComponentUnavailable,
        SdkUnavailable,
        BackendMismatch,
        DeviceMismatch,
        CapabilityUnavailable,
        TextureUnsupported,
        CookedArtifactMissing
    };

    /** @brief Immediate read-only answer for an explicitly requested route on a selected device generation. */
    struct ImageProviderAvailability final {
        ImageProviderInstallStatus installation{};
        ImageProviderReadiness readiness{};
        std::uint64_t deviceIncarnation{};
        std::uint64_t capabilityRevision{};
        std::uint64_t installRevision{};
    };

    /** @brief Current product/project requirements and bounded evidence, borrowed from established host owners for one query. */
    struct ImageProviderEvidence final {
        ImageProviderApiVersion hostApi;
        const ImageProviderInstallation *installation{}; /**< Host product inventory evidence after its trust validation. */
        const ImageProviderSdk *availableSdk{}; /**< Exact host-verified runtime identity; installed bytes alone cannot supply it. */
        std::span<const Sha256Digest> admittedCookedArtifacts; /**< Existing cooked-artifact owner has verified every supplied digest. */
        std::span<const RenderTextureDescriptor> textures;     /**< Actual scene/effect resources required for this requested mode. */
    };

    namespace ImageProviderRegistryErrors {
        extern const ErrorCodeDescriptor InvalidDescriptor;
        extern const ErrorCodeDescriptor InvalidQuery;
        extern const ErrorCodeDescriptor DuplicateRoute;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor RegistrySealed;
        extern const ErrorCodeDescriptor RegistryUnsealed;
        extern const ErrorCodeDescriptor UnknownRoute;
        extern const ErrorCodeDescriptor AllocationFailed;
    }  // namespace ImageProviderRegistryErrors

    /** @brief Describes the actual linked native spatial or temporal reconstruction graph provider.
     * @param kind Exact native reconstruction semantics. Native-resolution rendering is a separate route.
     * @param cooked Borrowed stage artifacts supplied by the owning cook. Missing entries remain discoverably unavailable.
     * @return Inert descriptor or typed duplicate, malformed cook or unsupported mode failure.
     * @details Validates supplied artifacts through the existing native shader admission without loading a module or device.
     */
    [[nodiscard]] Result<ImageProviderDescriptor> DescribeNativeReconstructionImageProvider(
        ReconstructionKind kind, std::span<const CookedNativeReconstructionKernel> cooked);

    /** @brief Bounded category-scoped registry of explicit inert contributions.
     * @details Build and seal before project work. A sealed registry is immutable. Registration and queries neither initialize
     * a provider nor probe or load hardware, binaries, SDKs or licenses. The caller supplies verified host evidence.
     */
    class ImageProviderRegistry final {
    public:
        static constexpr std::size_t MaximumRoutes = 128;
        static constexpr std::size_t MaximumArtifacts = 16;
        static constexpr std::size_t MaximumTextures = 16;

        /** @brief Copies one provider declaration into its category's namespace before sealing.
         * @param descriptor Complete supported route and requirements; no native resources.
         * @return Success or a typed descriptor, duplicate, capacity or lifecycle failure.
         */
        [[nodiscard]] Result<void> Register(ImageProviderDescriptor descriptor);

        /** @brief Freezes all category inventories in deterministic route order. Repeated sealing is harmless. */
        void Seal() noexcept;

        /** @brief Borrows immutable declarations after sealing. @return Empty before sealing. */
        [[nodiscard]] std::span<const ImageProviderDescriptor> Descriptors() const noexcept;

        /** @brief Finds a route after sealing. @param route Exact category/provider/mode identity.
         * @return Borrowed descriptor, or a typed malformed, unknown or unsealed error.
         */
        [[nodiscard]] Result<const ImageProviderDescriptor *> Find(const ImageProviderRoute &route) const;

        /** @brief Intersects one provider contract with actual selected backend and host evidence.
         * @param route Exact registered identity; never searches another route.
         * @param backend Selected backend's immutable capabilities.
         * @param device Exact selected driver/adapter cache identity when the provider has such a restriction.
         * @param evidence Current owner-verified product/cook records and actual requested texture descriptors.
         * @return Portable effective readiness or a typed query or lifecycle failure.
         */
        [[nodiscard]] Result<ImageProviderAvailability> Query(const ImageProviderRoute &route, const RenderBackendCapabilities &backend,
                                                              const RenderPipelineDeviceIdentity *device,
                                                              const ImageProviderEvidence &evidence) const;

    private:
        std::vector<ImageProviderDescriptor> descriptors_;
        bool sealed_{};
    };
}  // namespace Horo::Render
