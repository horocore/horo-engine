#pragma once

/**
 * @file NavigationProjectProfiles.h
 * @brief Project-authoritative navigation capacities, preview resolution and provider admission.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Navigation/NavigationCapabilities.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Horo::Navigation {
    struct NavigationProjectProfileIdentityTag;
    struct NavigationProjectProfileRevisionTag;
    struct NavigationProjectProfileFingerprintTag;
    struct NavigationPreviewPreferenceRevisionTag;

    /** @brief Stable project-authored identity of one navigation capacity profile. */
    using NavigationProjectProfileId = NavigationIdentity<NavigationProjectProfileIdentityTag>;
    /** @brief Monotonic revision of one project-authored profile. */
    using NavigationProjectProfileRevision = NavigationIdentity<NavigationProjectProfileRevisionTag>;
    /** @brief Deterministic fingerprint of all authoritative profile fields. */
    using NavigationProjectProfileFingerprint = NavigationIdentity<NavigationProjectProfileFingerprintTag>;
    /** @brief Monotonic revision of a non-authoritative developer-preview preference. */
    using NavigationPreviewPreferenceRevision = NavigationIdentity<NavigationPreviewPreferenceRevisionTag>;

    /** @brief Whether activation requires a provider capability or merely permits it. */
    enum class NavigationCapabilityRequirement : std::uint8_t {
        Optional,
        Required,
        Count
    };

    /** @brief Explicitly unit-labelled finite hard ceilings for one navigation profile. */
    struct NavigationCapacityLimits final {
        std::uint32_t maximumAgents{};               /**< Logical agents. */
        std::uint32_t maximumSurfaces{};             /**< Simultaneously active navigation surfaces. */
        std::uint32_t maximumResidentTiles{};        /**< Simultaneously resident navigation tiles. */
        std::uint32_t maximumConcurrentQueries{};    /**< Admitted query records. */
        std::uint64_t maximumBytesPerResidentTile{}; /**< Charged bytes for any one resident tile. */
        std::uint64_t maximumResidentMemoryBytes{};  /**< Aggregate resident navigation bytes. */
        std::uint64_t maximumWorkUnitsPerTick{};     /**< Aggregate provider-neutral work units per tick. */

        constexpr auto operator<=>(const NavigationCapacityLimits &) const noexcept = default;
    };

    /** @brief One observed/admitted usage sample checked without mutating runtime state. */
    struct NavigationCapacityUsage final {
        std::uint32_t agents{};               /**< Logical agents in use. */
        std::uint32_t surfaces{};             /**< Active surfaces in use. */
        std::uint32_t residentTiles{};        /**< Resident tiles in use. */
        std::uint32_t concurrentQueries{};    /**< Query records in use. */
        std::uint64_t bytesPerResidentTile{}; /**< Largest observed or requested charge for one resident tile. */
        std::uint64_t residentMemoryBytes{};  /**< Charged resident bytes in use. */
        std::uint64_t workUnitsThisTick{};    /**< Work units charged to the current tick. */
    };

    /** @brief Detached input used to transactionally construct or replace an authoritative project profile. */
    struct NavigationProjectProfileInput final {
        NavigationProjectProfileId id;             /**< Stable project identity. */
        NavigationProjectProfileRevision revision; /**< Monotonic authored revision. */
        NavigationCapacityLimits capacities;       /**< Finite hard ceilings. */
        NavigationQueryRequirement maximumQuery;   /**< Exact supported query/quality and per-query ceiling. */
        std::array<NavigationCapabilityRequirement, static_cast<std::size_t>(NavigationCapability::Count)> capabilities{};
    };

    /** @brief Immutable validated project authority for navigation capacity and capability admission. */
    class NavigationProjectProfile final {
    public:
        /**
         * @brief Validates and constructs one authoritative profile without publishing partial state.
         * @param input Detached project-authored candidate.
         * @return Immutable profile or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<NavigationProjectProfile> Create(const NavigationProjectProfileInput &input);

        /**
         * @brief Validates a complete successor while preserving the prior profile on failure.
         * @param previous Last-good immutable project profile.
         * @param input Complete candidate with the same identity and a strictly newer revision.
         * @return Immutable successor or a typed stale/invalid/capacity failure.
         */
        [[nodiscard]] static Result<NavigationProjectProfile> Replace(const NavigationProjectProfile &previous,
                                                                      const NavigationProjectProfileInput &input);

        /** @brief Returns the stable project identity. @return Non-zero profile identity. */
        [[nodiscard]] NavigationProjectProfileId Id() const noexcept;
        /** @brief Returns the authored revision. @return Non-zero monotonic revision. */
        [[nodiscard]] NavigationProjectProfileRevision Revision() const noexcept;
        /** @brief Returns the deterministic authoritative fingerprint. @return Non-zero stable fingerprint. */
        [[nodiscard]] NavigationProjectProfileFingerprint Fingerprint() const noexcept;
        /** @brief Compares exact captured authority, including finite capacities and every capability/query fact.
         * @param expected Validated project authority. @return True only for identical identity, revision, fingerprint and facts. */
        [[nodiscard]] bool MatchesAuthority(const NavigationProjectProfile &expected) const noexcept;
        /** @brief Returns the finite authoritative ceilings. @return Immutable capacity limits. */
        [[nodiscard]] const NavigationCapacityLimits &Capacities() const noexcept;
        /** @brief Returns the exact project query envelope. @return Immutable typed query requirement. */
        [[nodiscard]] const NavigationQueryRequirement &MaximumQuery() const noexcept;
        /**
         * @brief Returns the requirement for one closed provider capability.
         * @param capability Capability to inspect.
         * @return Required/optional, or Count for an unknown typed value.
         */
        [[nodiscard]] NavigationCapabilityRequirement Requirement(NavigationCapability capability) const noexcept;

    private:
        explicit NavigationProjectProfile(const NavigationProjectProfileInput &input,
                                          NavigationProjectProfileFingerprint fingerprint) noexcept;

        NavigationProjectProfileInput input_;
        NavigationProjectProfileFingerprint fingerprint_;
    };

    /** @brief Non-authoritative developer-preview ceilings tied to one exact project revision. */
    struct NavigationDeveloperPreviewPreference final {
        NavigationPreviewPreferenceRevision revision;     /**< Preference revision, never project authority. */
        NavigationProjectProfileRevision projectRevision; /**< Exact project revision being previewed. */
        NavigationCapacityLimits requestedMaximums;       /**< Requested ceilings, clamped to project authority. */
    };

    /** @brief Construction-guarded resolved capacities retaining exact project authority and preview provenance. */
    class ResolvedNavigationProjectProfile final {
    public:
        /** @brief Returns the stable project identity. @return Non-zero project profile identity. */
        [[nodiscard]] NavigationProjectProfileId Id() const noexcept;
        /** @brief Returns the exact authoritative project revision. @return Non-zero project revision. */
        [[nodiscard]] NavigationProjectProfileRevision ProjectRevision() const noexcept;
        /** @brief Returns the authoritative deterministic fingerprint. @return Non-zero project fingerprint. */
        [[nodiscard]] NavigationProjectProfileFingerprint ProjectFingerprint() const noexcept;
        /** @brief Returns applied preview provenance. @return Valid preview revision, or empty for project-only resolution. */
        [[nodiscard]] std::optional<NavigationPreviewPreferenceRevision> PreviewRevision() const noexcept;
        /** @brief Returns coherent resolved ceilings. @return Limits never exceeding project authority. */
        [[nodiscard]] const NavigationCapacityLimits &Capacities() const noexcept;
        /** @brief Returns the unchanged project query envelope. @return Immutable typed query requirement. */
        [[nodiscard]] const NavigationQueryRequirement &MaximumQuery() const noexcept;

    private:
        friend Result<ResolvedNavigationProjectProfile> ResolveNavigationProjectProfile(
            const NavigationProjectProfile &, const std::optional<NavigationDeveloperPreviewPreference> &);

        ResolvedNavigationProjectProfile(const NavigationProjectProfile &project,
                                         std::optional<NavigationPreviewPreferenceRevision> previewRevision,
                                         const NavigationCapacityLimits &capacities) noexcept;

        NavigationProjectProfileId id_;
        NavigationProjectProfileRevision projectRevision_;
        NavigationProjectProfileFingerprint projectFingerprint_;
        std::optional<NavigationPreviewPreferenceRevision> previewRevision_;
        NavigationCapacityLimits capacities_;
        NavigationQueryRequirement maximumQuery_;
    };

    /**
     * @brief Resolves optional developer-preview ceilings without allowing them to expand project authority.
     * @param project Validated authoritative project profile.
     * @param preview Optional developer-only preference for the exact project revision.
     * @return Resolved profile, or a typed invalid/stale failure; fields above project limits are clamped.
     */
    [[nodiscard]] Result<ResolvedNavigationProjectProfile> ResolveNavigationProjectProfile(
        const NavigationProjectProfile &project, const std::optional<NavigationDeveloperPreviewPreference> &preview = std::nullopt);

    /**
     * @brief Checks a complete usage sample against resolved finite ceilings.
     * @param profile Resolved project/preview limits.
     * @param usage Current or proposed aggregate usage.
     * @return Success, or NavigationErrors::ProjectProfileCapacityExceeded without reserving capacity.
     */
    [[nodiscard]] Result<void> AdmitNavigationCapacity(const ResolvedNavigationProjectProfile &profile,
                                                       const NavigationCapacityUsage &usage);

    /**
     * @brief Validates profile requirements against one exact provider capability snapshot before activation.
     * @param profile Immutable project authority.
     * @param provider Complete provider capability evidence.
     * @param expectedProviderRevision Exact non-zero revision retained by the activation candidate.
     * @return Success or a typed stale, unsupported, unavailable or capacity failure.
     */
    [[nodiscard]] Result<void> AdmitNavigationProjectProfile(const NavigationProjectProfile &profile,
                                                             const NavigationProviderCapabilities &provider,
                                                             std::uint64_t expectedProviderRevision);
}  // namespace Horo::Navigation
