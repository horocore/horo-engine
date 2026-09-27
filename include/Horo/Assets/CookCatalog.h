#pragma once

/**
 * @file CookCatalog.h
 * @brief Host-owned immutable cooker contribution catalog and sealed lookup.
 */

#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/Result.h"

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Assets {

    /**
     * @brief Bounded immutable borrowed view into host-owned source data for one cook invocation.
     */
    struct CookSourceView {
        AssetId id;                          /**< Stable asset identity. */
        AssetTypeId type;                    /**< Registered asset type. */
        AssetCookTargetId target;            /**< Target this invocation is cooking for. */
        Sha256Digest sourceDigest;           /**< SHA-256 of the source bytes. */
        std::span<const std::uint8_t> bytes; /**< Borrowed immutable source bytes. Valid only for invocation. */
    };

    /**
     * @brief Host-owned output sink. A cooker writes payloads and dependencies through this.
     *        Output is bounded and validated by the host before entering any cache or generation.
     */
    struct CookOutputSink {
        std::vector<std::uint8_t> payload;    /**< Cooked payload bytes written by the cooker. */
        std::vector<AssetId> dependencies;    /**< Asset identity dependencies the host must validate. */
        std::vector<std::string> diagnostics; /**< Human-readable diagnostic messages. */
    };

    /** @brief Stable byte-affecting strategy inputs supplied to the generic cache key. */
    struct CookerCacheIdentity {
        std::string_view version{"1.0.0"};
        Sha256Digest settingsDigest{};
        std::uint32_t settingsSchemaVersion{};
    };

    /**
     * @brief Abstract cooker strategy.
     * @details Both built-in cookers and C-ABI adapters implement this interface.
     *          Cook is background/tooling work and never runs in a frame-hot path.
     */
    class ICookerStrategy {
    public:
        virtual ~ICookerStrategy() = default;

        /** @brief Returns deterministic strategy version and effective settings for cache isolation. */
        [[nodiscard]] virtual CookerCacheIdentity CacheIdentity() const noexcept {
            return {};
        }

        /**
         * @brief Validates domain compatibility for fresh and cached logical payloads.
         * @param source Exact admitted source identity and target.
         * @param payload Borrowed domain payload bytes; never retained.
         * @return Success or a typed domain compatibility error before publication.
         */
        [[nodiscard]] virtual Result<void> ValidateCookedPayload(const CookSourceView &source,
                                                                 std::span<const std::uint8_t> payload) const {
            (void)source;
            (void)payload;
            return Result<void>::Success();
        }

        /**
         * @brief Cook the source asset into its target-specific payload.
         * @param source Borrowed immutable source view. Valid only for the invocation; must not be retained.
         * @param cancellation Host-owned cancellation token.
         * @return Cooked payload and dependencies, or a typed error.
         */
        [[nodiscard]] virtual Result<CookOutputSink> Cook(const CookSourceView &source, const CancellationToken &cancellation) const = 0;
    };

    /**
     * @brief Immutable contribution descriptor registered in the catalog.
     * @details Every contribution claims exactly one asset type and one or more targets.
     *          Duplicate type/target claims are rejected during candidate validation unless
     *          project policy names one exact contribution ID.
     */
    struct CookerContribution {
        std::string contributionId;                      /**< Stable unique contribution identity (e.g. "horo.builtin.mesh_null"). */
        AssetTypeId assetType;                           /**< Asset type this cooker handles. */
        std::vector<AssetCookTargetId> targets;          /**< Cook targets this contribution produces. */
        std::shared_ptr<const ICookerStrategy> strategy; /**< Immutable strategy instance. */

        /** @brief Returns true when this contribution claims the given type/target pair. */
        [[nodiscard]] bool Handles(const AssetTypeId &type, const AssetCookTargetId &target) const noexcept;
    };

    /**
     * @brief Immutable sealed catalog snapshot.
     * @details Entries are sorted by asset type, then target, then contribution ID.
     *          Lookup is allocation-free on the pinned snapshot.
     */
    class CookerCatalogSnapshot final {
    public:
        CookerCatalogSnapshot() = default;

        /**
         * @brief Constructs a snapshot from an immutable sorted entry list.
         * @param entries Sorted cooker contributions. The snapshot takes ownership.
         */
        explicit CookerCatalogSnapshot(std::vector<CookerContribution> entries);

        /**
         * @brief Finds a cooker strategy for the given asset type and target.
         * @param type Asset type to look up.
         * @param target Cook target to look up.
         * @return Strategy pointer, or nullptr when no matching contribution exists.
         */
        [[nodiscard]] const ICookerStrategy *Find(const AssetTypeId &type, const AssetCookTargetId &target) const noexcept;

        /** @brief Finds the exact registered contribution, including its stable cache identity. */
        [[nodiscard]] const CookerContribution *FindContribution(const AssetTypeId &type, const AssetCookTargetId &target) const noexcept;

    private:
        friend class CookerCatalog;

        std::vector<CookerContribution> entries_;
    };

    /**
     * @brief Host-owned mutable candidate builder for transactionally registering cooker contributions.
     * @details Build a candidate, validate it, and publish a new immutable snapshot atomically.
     *          The catalog is sealed after the first publish; further registrations fail with
     *          CatalogSealed.
     */
    class CookerCatalog final {
    public:
        /**
         * @brief A validated mutable candidate that may be published atomically.
         */
        struct Candidate {
            std::vector<CookerContribution> entries;
            std::vector<std::string> errors; /**< Validation errors collected during Register. */
        };

        CookerCatalog();
        ~CookerCatalog(); /**< Required for PIMPL destructor visibility. */

        /**
         * @brief Registers a cooker contribution into the candidate.
         * @param entry Contribution to register.
         * @return Result<void> with a typed DuplicateCooker or layout error on conflict.
         */
        [[nodiscard]] Result<void> Register(CookerContribution entry);

        /**
         * @brief Seals the catalog and publishes the immutable snapshot.
         * @return The new snapshot, or CatalogSealed/validation errors.
         * @details After the first successful Publish, the catalog is sealed. Subsequent
         *          Register or Publish calls fail with CatalogSealed.
         */
        [[nodiscard]] Result<std::shared_ptr<const CookerCatalogSnapshot>> Publish();

        /**
         * @brief Returns the current published snapshot, or nullptr when not yet published.
         */
        [[nodiscard]] std::shared_ptr<const CookerCatalogSnapshot> Snapshot() const noexcept;

        /**
         * @brief Returns true when the catalog has been sealed (Publish succeeded at least once).
         */
        [[nodiscard]] bool IsSealed() const noexcept;

        /**
         * @brief Clears the unsealed candidate entries without publishing.
         *        No-op when sealed.
         */
        void Reset();

    private:
        struct State;
        std::unique_ptr<State> state_;
    };

}  // namespace Horo::Assets
