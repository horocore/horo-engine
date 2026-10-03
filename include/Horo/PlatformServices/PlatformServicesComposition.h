#pragma once

/**
 * @file PlatformServicesComposition.h
 * @brief Product-owned Platform Services selection, capability projection and generation retirement.
 */

#include "Horo/PlatformServices/PlatformProjectConfiguration.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"

#include <functional>
#include <memory>

namespace Horo::PlatformServices {
    /** @brief Product policy, separate from the stable four-bit provider ABI admission profiles. */
    enum class PlatformServicesProductProfile : std::uint8_t {
        Editor,
        DevelopmentGame,
        ShippingGame,
        Headless,
        DedicatedServer,
        Test,
        UnsupportedPlatform,
        Count
    };

    /** @brief Explicit selection; absence and Null never invoke a provider factory. */
    enum class PlatformServicesCompositionMode : std::uint8_t {
        Null,
        Test,
        ExactProvider,
        Absent
    };

    /** @brief Provenance supplied by the host's trust/product-manifest owner, never by gameplay. */
    enum class PlatformServicesProviderProvenance : std::uint8_t {
        TrustedDevelopment,
        PublicTestFixture,
        ServerManifest,
        ShippingManifest
    };

    /**
     * @brief Immutable verified product evidence bound to one exact configuration and provider generation.
     * @details The application/package owner verifies signatures, frozen package/module hashes, ABI, SDK runtime and entitlements
     * before supplying ShippingManifest evidence. ServerManifest certifies server-specific session/credential policy. This layer
     * consumes that result; it performs no discovery, signature verification, package loading or native SDK probing.
     */
    struct PlatformServicesProductEvidence final {
        PlatformServicesProviderProvenance provenance{PlatformServicesProviderProvenance::TrustedDevelopment};
        Sha256Digest configurationFingerprint{};
        PlatformProviderGeneration generation{};
        std::array<bool, static_cast<std::size_t>(PlatformServiceKind::Count)> allowedServices{};
    };

    /** @brief Complete host-owned selection; generations advance across all product transitions, including Null/absence. */
    struct PlatformServicesCompositionRequest final {
        const PlatformProjectConfiguration &configuration;
        PlatformServicesProductProfile profile{PlatformServicesProductProfile::Headless};
        PlatformServicesCompositionMode mode{PlatformServicesCompositionMode::Null};
        PlatformProviderGeneration generation{};
        std::optional<PlatformServicesProductEvidence> evidence;
    };

    /** @brief Exclusive candidate ownership and matching copied session evidence supplied by the exact host factory. */
    struct PlatformServicesCompositionCandidate final {
        std::unique_ptr<IPlatformServicesBackend> backend;
        PlatformSessionSnapshot session;
    };

    /** @brief Exact factory injected at the host boundary; it is never called for Null or absence.
     * @details The request is borrowed only for this synchronous call. The factory must return a fresh backend and copy any
     * configuration it needs after return. Backend destruction retains native resources when shutdown cannot safely drain them.
     */
    using PlatformServicesCompositionFactory =
        std::function<Result<PlatformServicesCompositionCandidate>(const PlatformServicesCompositionRequest &)>;

    /** @brief Public capability/diagnostic view contains no native binding, SDK detail or provider session. */
    struct PlatformServicesCompositionDiagnostics final {
        PlatformServicesProductProfile profile{PlatformServicesProductProfile::Headless};
        PlatformServicesCompositionMode mode{PlatformServicesCompositionMode::Absent};
        PlatformProviderGeneration generation{};
        std::optional<PlatformProviderId> provider;
        std::array<PlatformServiceAvailability, static_cast<std::size_t>(PlatformServiceKind::Count)> services = [] {
            std::array<PlatformServiceAvailability, static_cast<std::size_t>(PlatformServiceKind::Count)> unavailable;
            unavailable.fill(PlatformServiceAvailability::Unavailable);
            return unavailable;
        }();
        std::array<std::optional<PlatformServiceUnavailableReason>, static_cast<std::size_t>(PlatformServiceKind::Count)> reasons{};
    };

    /**
     * @brief Owner-lane product composition with fail-closed Null/absence and exclusive backend lifetime.
     * @details No GUI, renderer, overlay or proprietary SDK target is required. Transition closes the previous frontend before
     * invoking the new factory. Retained frontend leases stay alive but closed. A shutdown or candidate-rollback failure retains the
     * undrained owner and blocks replacement; it requires explicit host recovery/restart rather than publishing another provider over
     * retained state.
     */
    class PlatformServicesComposition final {
        struct CreationKey final {
            explicit CreationKey() noexcept = default;
        };

    public:
        /** @brief Construction is restricted to Start through its private creation key. @param key Start-owned creation permission. */
        explicit PlatformServicesComposition([[maybe_unused]] CreationKey key) noexcept {}

        /** @brief Validate selection before invoking an exact factory, then publish the complete generation.
         * @param request Immutable project and product policy.
         * @param factory Host-owned exact candidate factory; empty for Null/absence.
         * @return Complete composition or a typed error preserving provider failure details.
         */
        [[nodiscard]] static Result<std::unique_ptr<PlatformServicesComposition>> Start(
            const PlatformServicesCompositionRequest &request, const PlatformServicesCompositionFactory &factory = {});
        ~PlatformServicesComposition();
        PlatformServicesComposition(const PlatformServicesComposition &) = delete;
        PlatformServicesComposition &operator=(const PlatformServicesComposition &) = delete;

        /** @brief Acquire the active narrow frontend; Null and absence return typed unavailability.
         * @return Frontend lease for this generation or typed closed/Null/absent error.
         */
        [[nodiscard]] Result<std::shared_ptr<const PlatformServicesFrontend>> Frontend() const;
        /** @brief Return sanitized current capability evidence. @return Copied Horo-only product diagnostics. */
        [[nodiscard]] PlatformServicesCompositionDiagnostics Diagnostics() const noexcept;
        /** @brief Close old admission and retire its backend before starting a newer product generation.
         * @param request New complete policy with a strictly advancing generation.
         * @param factory Exact factory for the new selection.
         * @return Success or typed failure; failed new startup leaves this composition explicitly closed.
         */
        [[nodiscard]] Result<void> Transition(const PlatformServicesCompositionRequest &request,
                                              const PlatformServicesCompositionFactory &factory = {});
        /** @brief Revoke public capability truth before releasing provider state. @return Idempotent shutdown result. */
        [[nodiscard]] Result<void> Close();

    private:
        /** @brief Nonthrowing factory/backend boundary prepares one validated generation; adapter exceptions become typed unavailability.
         */
        template <typename Factory>
        [[nodiscard]] Result<void> Initialize(const PlatformServicesCompositionRequest &request, const Factory &factory) noexcept;
        /** @brief Inspect, project and activate one candidate; failed rollback retains ownership. */
        [[nodiscard]] Result<void> InitializeCandidate(const PlatformServicesCompositionRequest &request,
                                                       PlatformServicesCompositionCandidate candidate);
        PlatformServicesCompositionDiagnostics diagnostics_;
        std::shared_ptr<PlatformServicesFrontend> frontend_;
        std::shared_ptr<IPlatformServicesBackend> retainedBackend_;
        std::optional<Error> rollbackError_;
        bool open_{};
    };
}  // namespace Horo::PlatformServices
