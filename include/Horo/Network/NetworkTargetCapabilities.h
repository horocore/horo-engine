#pragma once

/**
 * @file NetworkTargetCapabilities.h
 * @brief Product, package, host, project and selected network capability truth for one target.
 */

#include "Horo/Network/NetworkProjectSettings.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Horo::Network {
    namespace Detail {
        struct NetworkProductBuildIdTag;
        struct NetworkProductCapabilityRevisionTag;
        struct NetworkHostCapabilityRevisionTag;
        struct NetworkTransportProviderIdTag;
    }  // namespace Detail

    /** @brief Stable identity of the built product, not the current host invocation. */
    using NetworkProductBuildId = Foundation::Detail::NonZeroId64<Detail::NetworkProductBuildIdTag, NetworkErrors::IdentityInvalid>;
    /** @brief Monotonic immutable product-capability publication revision. */
    using NetworkProductCapabilityRevision =
        Foundation::Detail::NonZeroId64<Detail::NetworkProductCapabilityRevisionTag, NetworkErrors::IdentityInvalid>;
    /** @brief Monotonic host capability snapshot revision. */
    using NetworkHostCapabilityRevision =
        Foundation::Detail::NonZeroId64<Detail::NetworkHostCapabilityRevisionTag, NetworkErrors::IdentityInvalid>;
    /** @brief Horo-owned provider identity; native backend names and handles remain private. */
    using NetworkTransportProviderId =
        Foundation::Detail::NonZeroId64<Detail::NetworkTransportProviderIdTag, NetworkErrors::IdentityInvalid>;

    /** @brief Closed target platform vocabulary used by product and host evidence. */
    enum class NetworkTargetPlatform : std::uint8_t {
        Linux,
        MacOS,
        Windows,
        Android,
        IOS,
        Count,
    };

    /** @brief Closed set of platforms on which one packaged provider is qualified. */
    using NetworkTargetPlatformMask = std::uint32_t;

    /** @brief Whether a platform is declared in a closed provider mask. */
    [[nodiscard]] constexpr bool ContainsNetworkTargetPlatform(const NetworkTargetPlatformMask mask,
                                                               const NetworkTargetPlatform platform) noexcept {
        return platform < NetworkTargetPlatform::Count && (mask & (std::uint32_t{1} << static_cast<std::uint8_t>(platform))) != 0;
    }

    inline constexpr std::size_t MaximumNetworkTargetProviders = 8;
    inline constexpr std::size_t MaximumNetworkTargetObservedProviders = MaximumNetworkTargetProviders * 3 + 2;

    /** @brief Provider capabilities declared and qualified by one product artifact. */
    struct NetworkPackagedTransport final {
        NetworkTransportProviderId id{};
        NetworkTargetPlatformMask allowedPlatforms{};
        TransportCapabilities capabilities{};

        constexpr auto operator<=>(const NetworkPackagedTransport &) const noexcept = default;
    };

    /** @brief Safe immutable product facts; never current authority, provider configuration or a credential. */
    struct NetworkProductCapabilityManifest final {
        static constexpr std::uint32_t CurrentContractVersion = 1;

        std::uint32_t contractVersion{CurrentContractVersion};
        NetworkProductBuildId build{};
        NetworkProductCapabilityRevision revision{};
        NetworkTargetPlatform platform{NetworkTargetPlatform::Count};
        NetworkProjectRoleSet supportedRoles{NetworkProjectRoleSet::None};
        bool includesNetworkRuntime{};
        NetworkProjectProfileId profile{};
        NetworkProjectProfileRevision profileRevision{};
        NetworkProjectProtocolPolicy protocol{};
        std::array<NetworkPackagedTransport, MaximumNetworkTargetProviders> providers{};
        std::size_t providerCount{};
    };

    /** @brief Actual package contents reported by final artifact verification, separately from the manifest. */
    struct NetworkTargetPackageInventory final {
        NetworkProductBuildId build{};
        NetworkTargetPlatform platform{NetworkTargetPlatform::Count};
        NetworkProjectRoleSet supportedRoles{NetworkProjectRoleSet::None};
        bool includesNetworkRuntime{};
        NetworkProjectProtocolPolicy protocol{};
        std::array<NetworkPackagedTransport, MaximumNetworkTargetProviders> providers{};
        std::size_t providerCount{};
    };

    /** @brief One exact host registration, platform and configuration fact; no factory is invoked. */
    struct NetworkHostTransportFact final {
        NetworkTransportProviderId id{};
        bool installed{};
        bool hostSupported{};
        bool configured{};
        TransportCapabilities capabilities{};
    };

    /** @brief Host-owned snapshot of actual installed and supported capabilities. */
    struct NetworkTargetHostFacts final {
        NetworkHostCapabilityRevision revision{};
        NetworkTargetPlatform platform{NetworkTargetPlatform::Count};
        NetworkProjectRoleSet supportedRoles{NetworkProjectRoleSet::None};
        bool networkRuntimeInstalled{};
        NetworkProjectProtocolPolicy protocol{};
        std::array<NetworkHostTransportFact, MaximumNetworkTargetProviders> providers{};
        std::size_t providerCount{};
    };

    /** @brief Project/release requirements, distinct from package contents and current selection. */
    struct NetworkTargetRequirements final {
        NetworkProjectRoleSet requiredRoles{NetworkProjectRoleSet::None};
        NetworkTransportProviderId requiredProvider{}; /**< Invalid means any explicitly selected qualified provider. */
    };

    /** @brief Caller-owned lifecycle evidence for a frozen target assessment. */
    enum class NetworkTargetLifecycle : std::uint8_t {
        Active,
        Cancelling,
        ShuttingDown,
        Count,
    };

    /** @brief One explicit invocation selection fenced to exact immutable inputs. */
    struct NetworkTargetSelection final {
        NetworkProjectRole role{NetworkProjectRole::Count};
        NetworkTransportProviderId provider{}; /**< Absent only for Standalone. */
        ProtocolVersion protocolVersion{};     /**< Absent only for Standalone. */
        NetworkProductBuildId expectedBuild{};
        NetworkProductCapabilityRevision expectedProductRevision{};
        NetworkHostCapabilityRevision expectedHostRevision{};
        NetworkProjectSettingsRevision expectedProjectRevision{};
        NetworkTargetLifecycle lifecycle{NetworkTargetLifecycle::Active};
    };

    /** @brief Which exact capability prevented admission. */
    enum class NetworkTargetCapabilityKind : std::uint8_t {
        Input,
        PackageInventory,
        Platform,
        NetworkRuntime,
        Role,
        Provider,
        Protocol,
        ProjectProfile,
        Lifecycle,
    };

    /** @brief Stable machine-readable reason, independent of display text. */
    enum class NetworkTargetFailureReason : std::uint8_t {
        None,
        Invalid,
        Stale,
        Cancelled,
        ShuttingDown,
        NotPackaged,
        NotInstalled,
        HostUnsupported,
        ProjectUnavailable,
        NotSelected,
        Incompatible,
        PackageMismatch,
    };

    /** @brief Safe action category; a presenter may localize its wording. */
    enum class NetworkTargetRemediation : std::uint8_t {
        None,
        CorrectInput,
        RefreshSnapshot,
        RebuildPackage,
        InstallTarget,
        ChooseSupportedHost,
        ConfigureProject,
        SelectAvailableCapability,
        UpgradeProtocol,
        WaitForRestart,
    };

    /** @brief Exact first failure with typed subject identity and remediation, never a native provider value. */
    struct NetworkTargetDiagnostic final {
        NetworkTargetCapabilityKind capability{NetworkTargetCapabilityKind::Input};
        NetworkTargetFailureReason reason{NetworkTargetFailureReason::Invalid};
        NetworkTargetRemediation remediation{NetworkTargetRemediation::None};
        NetworkProjectRole role{NetworkProjectRole::Count};
        NetworkTransportProviderId provider{};
        ProtocolId protocol{};
        NetworkTargetPlatform platform{NetworkTargetPlatform::Count};
    };

    /** @brief Separate package, host, project and selection facts for one role. */
    struct NetworkTargetRoleStatus final {
        NetworkProjectRole role{NetworkProjectRole::Count};
        bool packaged{};
        bool hostSupported{};
        bool projectRequired{};
        bool selected{};
    };

    /** @brief Separate package, installation, host, project and selection facts for one provider. */
    struct NetworkTargetProviderStatus final {
        NetworkTransportProviderId id{};
        bool packaged{};
        bool installed{};
        bool hostSupported{};
        bool configured{};
        bool projectRequired{};
        bool selected{};
    };

    /** @brief Bounded, inert evidence snapshot; only an admitted assessment can authorize activation. */
    struct NetworkTargetCapabilityMatrix final {
        NetworkProductBuildId build{};
        NetworkProductCapabilityRevision productRevision{};
        NetworkHostCapabilityRevision hostRevision{};
        NetworkProjectSettingsRevision projectRevision{};
        std::array<NetworkTargetRoleStatus, static_cast<std::size_t>(NetworkProjectRole::Count)> roles{};
        std::array<NetworkTargetProviderStatus, MaximumNetworkTargetObservedProviders> providers{};
        std::size_t providerCount{};
        bool packagePlatformMatches{};
        bool hostPlatformMatches{};
        bool packageRuntimePresent{};
        bool hostRuntimeInstalled{};
        bool protocolPackaged{};
        bool protocolHostSupported{};
        bool protocolProjectRequired{};
        bool protocolSelected{};
    };

    /** @brief Pure assessment with either complete admission or one exact typed rejection. */
    struct NetworkTargetAssessment final {
        NetworkTargetCapabilityMatrix matrix{};
        NetworkTargetDiagnostic diagnostic{};

        /** @brief Whether activation may use this exact selection. @return True only with no rejection. */
        [[nodiscard]] constexpr bool Admitted() const noexcept {
            return diagnostic.reason == NetworkTargetFailureReason::None;
        }
    };

    /**
     * @brief Assess one exact product/package/host/project/selection without probing or activating a backend.
     * @param project Validated immutable portable project settings.
     * @param product Product-owned role, provider, platform and protocol declaration.
     * @param inventory Final package contents, independent of the declaration.
     * @param host Host-owned installed and supported capability evidence.
     * @param requirements Project/release required roles and optional exact provider.
     * @param selection One selected role/provider/protocol and exact revision/lifecycle evidence.
     * @return Bounded matrix and exact typed admission or first rejection with remediation.
     * @post Failure has no side effects and cannot grant a role or substitute another provider.
     */
    [[nodiscard]] NetworkTargetAssessment AssessNetworkTarget(const NetworkProjectSettings &project,
                                                              const NetworkProductCapabilityManifest &product,
                                                              const NetworkTargetPackageInventory &inventory,
                                                              const NetworkTargetHostFacts &host,
                                                              const NetworkTargetRequirements &requirements,
                                                              const NetworkTargetSelection &selection);

    /**
     * @brief Serialize one valid public product declaration as canonical bounded JSON.
     * @param product Valid manifest; invalid inputs fail without publication.
     * @return Canonical safe manifest or a typed invalid/capacity error.
     */
    [[nodiscard]] Result<std::string> SerializeNetworkProductCapabilityManifest(const NetworkProductCapabilityManifest &product);

    /**
     * @brief Parse a closed version-one product declaration, rejecting duplicates, future schemas and secrets.
     * @param document Untrusted bounded package manifest bytes.
     * @return Complete validated manifest or a typed invalid/capacity error.
     */
    [[nodiscard]] Result<NetworkProductCapabilityManifest> ParseNetworkProductCapabilityManifest(std::string_view document);
}  // namespace Horo::Network
