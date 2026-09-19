#pragma once

/**
 * @file AssetCookerRegistry.h
 * @brief Host-owned asset-cooker extension point with deterministic keys and transactional output staging.
 */

#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Diagnostics.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

namespace Horo::Extensions {
    /** @brief Stable identity of one asset-cooker contribution. */
    struct AssetCookerId final {
        std::string value;
        [[nodiscard]] bool operator==(const AssetCookerId &) const noexcept = default;
    };

    /** @brief Immutable provider ownership, compatibility, and cache-identity declaration. */
    struct AssetCookerDescriptor final {
        AssetCookerId cookerId;                 /**< Canonical contribution identity. */
        std::string providerId;                 /**< Canonical module/provider identity. */
        std::uint64_t providerGeneration{};     /**< Non-zero activation generation. */
        Assets::AssetTypeId assetType;          /**< Exact imported asset type accepted by this cooker. */
        std::vector<AssetCookTargetId> targets; /**< Sorted unique platform/profile target identities. */
        std::string cookerVersion;              /**< Stable semantic implementation version in cache keys. */
        std::uint32_t artifactFormatVersion{};  /**< Non-zero cooked artifact format version. */
    };

    /** @brief Host-selected hard bounds for one cooker invocation. */
    struct AssetCookerLimits final {
        std::size_t maximumSourceBytes{256U * 1024U * 1024U};   /**< Maximum borrowed source byte count. */
        std::size_t maximumArtifactBytes{256U * 1024U * 1024U}; /**< Maximum staged payload byte count. */
        std::size_t maximumDependencies{16U * 1024U};           /**< Maximum unique asset dependencies. */
        std::size_t maximumDiagnostics{1024U};                  /**< Maximum structured provider diagnostics. */
        std::size_t maximumDiagnosticCodeBytes{128U};           /**< Maximum bytes in one diagnostic code. */
        std::size_t maximumDiagnosticMessageBytes{16U * 1024U}; /**< Maximum bytes in one diagnostic message. */
    };

    /** @brief Immutable borrowed source and canonical digest inputs for one cook invocation. */
    struct AssetCookerInput final {
        Assets::AssetId assetId;                   /**< Stable imported asset identity. */
        Assets::AssetTypeId assetType;             /**< Exact registered imported type. */
        AssetCookTargetId target;                  /**< Explicit platform/profile target. */
        Sha256Digest sourceDigest;                 /**< Host-verified digest of sourceBytes. */
        Sha256Digest metadataDigest;               /**< Digest of canonical cooker-visible metadata. */
        std::uint32_t metadataSchemaVersion{};     /**< Non-zero metadata schema version. */
        Sha256Digest settingsDigest;               /**< Digest of canonical effective cook settings. */
        std::uint32_t settingsSchemaVersion{};     /**< Non-zero settings schema version. */
        std::span<const std::uint8_t> sourceBytes; /**< Immutable host-owned bytes valid only during Cook. */
    };

    /** @brief Host cook request with optional exact conflict-policy selection. */
    struct AssetCookerRequest final {
        AssetCookerInput input;
        std::optional<AssetCookerId> selectedCooker; /**< Exact project-policy choice; empty requires one unambiguous provider. */
        AssetCookerLimits limits;
    };

    /** @brief Structured provider diagnostic attributed to the cooked asset and exact provider generation. */
    struct AssetCookerDiagnostic final {
        DiagnosticCode code;                                    /**< Stable diagnostic identity. */
        DiagnosticSeverity severity{DiagnosticSeverity::Error}; /**< Host presentation severity. */
        std::string message;                                    /**< Bounded provider detail. */
    };

    /** @brief Host-owned transaction-local sink; no provider output is published before successful completion. */
    class AssetCookerOutputSink final {
    public:
        /**
         * @brief Stages the complete cooked payload exactly once.
         * @param bytes Provider output copied into host-owned bounded storage.
         * @return Success or a typed duplicate/capacity failure.
         */
        [[nodiscard]] Result<void> WritePayload(std::span<const std::uint8_t> bytes);

        /**
         * @brief Adds one unique asset dependency to the staged output.
         * @param dependency Stable non-zero asset identity.
         * @return Success or a typed invalid/duplicate/capacity failure.
         */
        [[nodiscard]] Result<void> AddDependency(Assets::AssetId dependency);

        /**
         * @brief Adds one bounded structured diagnostic to the staged output.
         * @param diagnostic Complete diagnostic owned by the sink after success.
         * @return Success or a typed invalid/capacity failure.
         */
        [[nodiscard]] Result<void> AddDiagnostic(AssetCookerDiagnostic diagnostic);

    private:
        friend class AssetCookerRegistry;
        explicit AssetCookerOutputSink(const AssetCookerLimits &limits) noexcept;
        [[nodiscard]] Result<void> Complete();

        AssetCookerLimits limits_;
        std::vector<std::uint8_t> payload_;
        std::vector<Assets::AssetId> dependencies_;
        std::unordered_set<Assets::AssetId, Assets::AssetIdHash> dependenciesSeen_;
        std::vector<AssetCookerDiagnostic> diagnostics_;
        bool payloadWritten_{};
        bool rejected_{};
    };

    /** @brief Trusted synchronous cooker invoked only through host-owned inputs and staging. */
    class IAssetCooker {
    public:
        virtual ~IAssetCooker() = default;

        /**
         * @brief Transforms one immutable imported source into target-ready staged output.
         * @param input Borrowed immutable input valid only for this synchronous call.
         * @param output Host-owned bounded staging sink.
         * @param cancellation Cooperative cancellation observed by provider work.
         * @return Success only after the provider has produced its complete staged output.
         */
        [[nodiscard]] virtual Result<void> Cook(const AssetCookerInput &input, AssetCookerOutputSink &output,
                                                const CancellationToken &cancellation) const = 0;
    };

    struct AssetCookerRegistryState;
    struct AssetCookerProviderState;

    /** @brief Move-only publication whose lifetime controls future cooker selection. */
    class AssetCookerRegistration final {
    public:
        ~AssetCookerRegistration() noexcept;
        AssetCookerRegistration(const AssetCookerRegistration &) = delete;
        AssetCookerRegistration &operator=(const AssetCookerRegistration &) = delete;
        AssetCookerRegistration(AssetCookerRegistration &&other) noexcept;
        AssetCookerRegistration &operator=(AssetCookerRegistration &&other);

        /** @brief Idempotently removes this exact provider generation from future cook admission. */
        void Reset();
        /** @brief Reports whether the publication remains discoverable for new cooks. @return True while registered. */
        [[nodiscard]] bool IsRegistered() const noexcept;

    private:
        friend class AssetCookerRegistry;
        AssetCookerRegistration(std::weak_ptr<AssetCookerRegistryState> registry,
                                std::shared_ptr<AssetCookerProviderState> provider) noexcept;

        std::weak_ptr<AssetCookerRegistryState> registry_;
        std::shared_ptr<AssetCookerProviderState> provider_;
    };

    /** @brief Complete host-owned cook result; failed or cancelled calls publish no partial result. */
    struct AssetCookerResult final {
        AssetCookerDescriptor provider;                 /**< Exact selected provider generation and contract. */
        Assets::AssetCookCacheKey cacheKey;             /**< Host-computed deterministic key. */
        std::vector<std::uint8_t> payload;              /**< Complete bounded staged payload. */
        std::vector<Assets::AssetId> dependencies;      /**< Canonically sorted unique dependencies. */
        std::vector<AssetCookerDiagnostic> diagnostics; /**< Complete attributed provider diagnostics. */
    };

    /** @brief Explicit composition-owned asset-cooker extension registry and invocation gateway. */
    class AssetCookerRegistry final {
    public:
        static constexpr std::size_t MaximumProviders = 256;

        AssetCookerRegistry();
        ~AssetCookerRegistry() noexcept;
        AssetCookerRegistry(const AssetCookerRegistry &) = delete;
        AssetCookerRegistry &operator=(const AssetCookerRegistry &) = delete;
        AssetCookerRegistry(AssetCookerRegistry &&) noexcept = default;
        AssetCookerRegistry &operator=(AssetCookerRegistry &&other);

        /**
         * @brief Publishes inert cooker metadata and its trusted implementation from the composition root.
         * @param descriptor Exact contribution, provider, target, and deterministic version declaration.
         * @param provider Shared provider retained by already-admitted synchronous cooks.
         * @return Lifetime registration or a typed invalid/duplicate/capacity/shutdown failure.
         * @note Registration never invokes provider code.
         */
        [[nodiscard]] Result<AssetCookerRegistration> Register(AssetCookerDescriptor descriptor,
                                                               std::shared_ptr<const IAssetCooker> provider);

        /**
         * @brief Selects and invokes one cooker while staging all output transactionally.
         * @param request Immutable input, exact target, optional policy selection, and host bounds.
         * @param cancellation Cooperative cancellation checked before and after provider invocation.
         * @return Complete attributed result and host-computed key, or failure with no partial output.
         */
        [[nodiscard]] Result<AssetCookerResult> Cook(const AssetCookerRequest &request, const CancellationToken &cancellation) const;

        /** @brief Idempotently closes registration and new cook admission. */
        void BeginShutdown();
        /** @brief Reports whether this registry has entered terminal shutdown. @return True after shutdown admission closes. */
        [[nodiscard]] bool IsShutdown() const;

    private:
        std::shared_ptr<AssetCookerRegistryState> state_;
    };
}  // namespace Horo::Extensions
