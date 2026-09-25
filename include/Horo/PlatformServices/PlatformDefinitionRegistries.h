#pragma once

/**
 * @file PlatformDefinitionRegistries.h
 * @brief Typed immutable leaderboard, stat, and presence definition registries.
 */

#include "Horo/PlatformServices/PlatformStableIdRegistry.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    inline constexpr std::uint32_t PlatformDefinitionRegistrySchemaVersion = 1;

    /** @brief Retry/replay algebra fixed by one stat definition. */
    enum class StatMutationPolicy : std::uint8_t {
        SetMaximum,
        SetMinimum,
        SnapshotAtRevision,
        AddOnce
    };
    /** @brief Whether runtime presence detail is forbidden or accepted within a finite bound. */
    enum class PresenceDetailPolicy : std::uint8_t {
        Forbidden,
        Optional
    };

    /** @brief Inclusive portable numeric range. */
    struct ProgressionNumericRange final {
        std::int64_t minimum{};
        std::int64_t maximum{};
        [[nodiscard]] bool operator==(const ProgressionNumericRange &) const noexcept = default;
    };

    /** @brief One complete provider-neutral persistent-stat definition. */
    struct StatDefinition final {
        StatId id;                            /**< Active ADR-132 stat identity. */
        ProgressionAuthorityMode authority{}; /**< Immutable fact authority. */
        ProgressionValueKind valueKind{};     /**< Immutable numeric representation. */
        ProgressionNumericRange range;        /**< Inclusive accepted value range. */
        StatMutationPolicy mutation{};        /**< Immutable retry/replay algebra. */
        std::string localizationKey;          /**< Bounded presentation key. */
        bool hidden{};                        /**< Presentation-only visibility. */
        [[nodiscard]] bool operator==(const StatDefinition &) const noexcept = default;
    };

    /** @brief One complete provider-neutral leaderboard definition. */
    struct LeaderboardDefinition final {
        LeaderboardId id;                     /**< Active ADR-132 leaderboard identity. */
        ProgressionAuthorityMode authority{}; /**< Immutable score authority. */
        ProgressionValueKind valueKind{};     /**< Immutable score representation. */
        ProgressionNumericRange range;        /**< Inclusive accepted score range. */
        LeaderboardOrdering ordering{};       /**< Immutable best-score comparison. */
        std::optional<StatId> sourceStat;     /**< Optional compatible authored stat projection. */
        std::string localizationKey;          /**< Bounded presentation key. */
        bool hidden{};                        /**< Presentation-only visibility. */
        [[nodiscard]] bool operator==(const LeaderboardDefinition &) const noexcept = default;
    };

    /** @brief One complete project-authored presence status definition. */
    struct PresenceDefinition final {
        PresenceStatusId id;                    /**< Active ADR-132 presence-status identity. */
        PresenceDetailPolicy detailPolicy{};    /**< Immutable free-detail policy. */
        std::uint32_t maximumDetailUtf8Bytes{}; /**< Zero when forbidden; finite when optional. */
        std::string localizationKey;            /**< Bounded presentation key. */
        bool hidden{};                          /**< Presentation-only visibility. */
        [[nodiscard]] bool operator==(const PresenceDefinition &) const noexcept = default;
    };

    /** @brief Explicit finite validation bounds shared by detached definition candidates. */
    struct PlatformDefinitionRegistryLimits final {
        std::uint32_t maximumDefinitions{2048};
        std::uint32_t maximumLocalizationKeyBytes{128};
        std::uint32_t maximumPresenceDetailBytes{1024};
    };

    /** @brief Detached complete stat document awaiting validation. */
    struct StatDefinitionRegistryCandidate final {
        std::uint32_t schemaVersion{PlatformDefinitionRegistrySchemaVersion};
        Sha256Digest stableIdRegistryFingerprint{};
        std::vector<StatDefinition> definitions;
    };

    /** @brief Detached complete leaderboard document awaiting validation. */
    struct LeaderboardDefinitionRegistryCandidate final {
        std::uint32_t schemaVersion{PlatformDefinitionRegistrySchemaVersion};
        Sha256Digest stableIdRegistryFingerprint{};
        std::vector<LeaderboardDefinition> definitions;
    };

    /** @brief Detached complete presence document awaiting validation. */
    struct PresenceDefinitionRegistryCandidate final {
        std::uint32_t schemaVersion{PlatformDefinitionRegistrySchemaVersion};
        Sha256Digest stableIdRegistryFingerprint{};
        std::vector<PresenceDefinition> definitions;
    };

    /** @brief Stable failures emitted by platform definition validation. */
    namespace PlatformDefinitionErrors {
        extern const ErrorCodeDescriptor UnsupportedVersion;
        extern const ErrorCodeDescriptor CapacityExceeded;
        extern const ErrorCodeDescriptor InvalidDefinition;
        extern const ErrorCodeDescriptor DuplicateDefinition;
        extern const ErrorCodeDescriptor UnknownIdentity;
        extern const ErrorCodeDescriptor IncompleteRegistry;
        extern const ErrorCodeDescriptor InvalidCrossReference;
        extern const ErrorCodeDescriptor StaleIdentityRegistry;
        extern const ErrorCodeDescriptor ImmutableContractChanged;
    }  // namespace PlatformDefinitionErrors

    /**
     * @brief Shared immutable storage for one typed platform-definition snapshot.
     * @tparam Definition Provider-neutral definition value stored by the snapshot.
     */
    template <typename Definition> class PlatformDefinitionRegistrySnapshot {
    public:
        /** @brief Returns the project identity captured by this snapshot. @return Non-owning project identity view valid for this
         * snapshot's lifetime. */
        [[nodiscard]] std::string_view StableIdProjectId() const noexcept {
            return projectId_;
        }

        /** @brief Returns the stable-ID registry fingerprint captured by this snapshot. @return Immutable fingerprint reference. */
        [[nodiscard]] const Sha256Digest &StableIdRegistryFingerprint() const noexcept {
            return stableFingerprint_;
        }

        /** @brief Returns the deterministic semantic fingerprint. @return Immutable fingerprint reference. */
        [[nodiscard]] const Sha256Digest &Fingerprint() const noexcept {
            return fingerprint_;
        }

        /** @brief Returns definitions in stable-ID order. @return Non-owning immutable span valid for this snapshot's lifetime. */
        [[nodiscard]] std::span<const Definition> Definitions() const noexcept {
            return definitions_;
        }

    protected:
        PlatformDefinitionRegistrySnapshot() = default;

        PlatformDefinitionRegistrySnapshot(std::string projectId, const Sha256Digest stableFingerprint, const Sha256Digest fingerprint,
                                           std::vector<Definition> definitions)
            : projectId_(std::move(projectId)), stableFingerprint_(stableFingerprint), fingerprint_(fingerprint),
              definitions_(std::move(definitions)) {}

        std::vector<Definition> definitions_;

    private:
        std::string projectId_;
        Sha256Digest stableFingerprint_{};
        Sha256Digest fingerprint_{};
    };

    /** @brief Immutable identity-sorted stat definition snapshot. */
    class StatDefinitionRegistry final : public PlatformDefinitionRegistrySnapshot<StatDefinition> {
    public:
        /** @brief Finds an exact stat without fallback. @param id Stable stat identity. @return Snapshot-owned definition or failure. */
        [[nodiscard]] Result<const StatDefinition *> Find(StatId id) const;

    private:
        friend Result<StatDefinitionRegistry> BuildStatDefinitionRegistry(const PlatformStableIdRegistry &,
                                                                          const StatDefinitionRegistryCandidate &,
                                                                          const PlatformDefinitionRegistryLimits &);
        using PlatformDefinitionRegistrySnapshot::PlatformDefinitionRegistrySnapshot;
    };

    /** @brief Immutable identity-sorted leaderboard definition snapshot. */
    class LeaderboardDefinitionRegistry final : public PlatformDefinitionRegistrySnapshot<LeaderboardDefinition> {
    public:
        /** @brief Finds an exact leaderboard. @param id Stable leaderboard identity. @return Snapshot-owned definition or failure. */
        [[nodiscard]] Result<const LeaderboardDefinition *> Find(LeaderboardId id) const;

    private:
        friend Result<LeaderboardDefinitionRegistry> BuildLeaderboardDefinitionRegistry(const PlatformStableIdRegistry &,
                                                                                        const StatDefinitionRegistry &,
                                                                                        const LeaderboardDefinitionRegistryCandidate &,
                                                                                        const PlatformDefinitionRegistryLimits &);
        using PlatformDefinitionRegistrySnapshot::PlatformDefinitionRegistrySnapshot;
    };

    /** @brief Immutable identity-sorted presence definition snapshot. */
    class PresenceDefinitionRegistry final : public PlatformDefinitionRegistrySnapshot<PresenceDefinition> {
    public:
        /** @brief Finds an exact status. @param id Stable presence identity. @return Snapshot-owned definition or failure. */
        [[nodiscard]] Result<const PresenceDefinition *> Find(PresenceStatusId id) const;

    private:
        friend Result<PresenceDefinitionRegistry> BuildPresenceDefinitionRegistry(const PlatformStableIdRegistry &,
                                                                                  const PresenceDefinitionRegistryCandidate &,
                                                                                  const PlatformDefinitionRegistryLimits &);
        using PlatformDefinitionRegistrySnapshot::PlatformDefinitionRegistrySnapshot;
    };

    /**
     * @brief Builds a complete immutable stat registry.
     * @param stableIds Captured project stable-ID registry.
     * @param candidate Detached authored stat document.
     * @param limits Finite validation bounds.
     * @return Canonical snapshot or a stable validation failure.
     */
    [[nodiscard]] Result<StatDefinitionRegistry> BuildStatDefinitionRegistry(const PlatformStableIdRegistry &stableIds,
                                                                             const StatDefinitionRegistryCandidate &candidate,
                                                                             const PlatformDefinitionRegistryLimits &limits = {});
    /**
     * @brief Builds a complete immutable leaderboard registry and validates stat references.
     * @param stableIds Captured project stable-ID registry.
     * @param stats Compatible stat snapshot used for cross-reference validation.
     * @param candidate Detached authored leaderboard document.
     * @param limits Finite validation bounds.
     * @return Canonical snapshot or a stable validation failure.
     */
    [[nodiscard]] Result<LeaderboardDefinitionRegistry> BuildLeaderboardDefinitionRegistry(
        const PlatformStableIdRegistry &stableIds, const StatDefinitionRegistry &stats,
        const LeaderboardDefinitionRegistryCandidate &candidate, const PlatformDefinitionRegistryLimits &limits = {});
    /**
     * @brief Builds a complete immutable presence registry.
     * @param stableIds Captured project stable-ID registry.
     * @param candidate Detached authored presence document.
     * @param limits Finite validation bounds.
     * @return Canonical snapshot or a stable validation failure.
     */
    [[nodiscard]] Result<PresenceDefinitionRegistry> BuildPresenceDefinitionRegistry(const PlatformStableIdRegistry &stableIds,
                                                                                     const PresenceDefinitionRegistryCandidate &candidate,
                                                                                     const PlatformDefinitionRegistryLimits &limits = {});
    /**
     * @brief Builds a compatible stat replacement while retaining the prior snapshot on failure.
     * @param previous Previously published immutable snapshot.
     * @param stableIds Replacement stable-ID registry.
     * @param candidate Detached replacement document.
     * @param limits Finite validation bounds.
     * @return Replacement snapshot or a failure; the prior snapshot is never mutated.
     */
    [[nodiscard]] Result<StatDefinitionRegistry> BuildStatDefinitionRegistryReplacement(
        const StatDefinitionRegistry &previous, const PlatformStableIdRegistry &stableIds, const StatDefinitionRegistryCandidate &candidate,
        const PlatformDefinitionRegistryLimits &limits = {});
    /**
     * @brief Builds a compatible leaderboard replacement while retaining the prior snapshot on failure.
     * @param previous Previously published immutable snapshot.
     * @param stableIds Replacement stable-ID registry.
     * @param stats Compatible replacement stat snapshot.
     * @param candidate Detached replacement document.
     * @param limits Finite validation bounds.
     * @return Replacement snapshot or a failure; the prior snapshot is never mutated.
     */
    [[nodiscard]] Result<LeaderboardDefinitionRegistry> BuildLeaderboardDefinitionRegistryReplacement(
        const LeaderboardDefinitionRegistry &previous, const PlatformStableIdRegistry &stableIds, const StatDefinitionRegistry &stats,
        const LeaderboardDefinitionRegistryCandidate &candidate, const PlatformDefinitionRegistryLimits &limits = {});
    /**
     * @brief Builds a compatible presence replacement while retaining the prior snapshot on failure.
     * @param previous Previously published immutable snapshot.
     * @param stableIds Replacement stable-ID registry.
     * @param candidate Detached replacement document.
     * @param limits Finite validation bounds.
     * @return Replacement snapshot or a failure; the prior snapshot is never mutated.
     */
    [[nodiscard]] Result<PresenceDefinitionRegistry> BuildPresenceDefinitionRegistryReplacement(
        const PresenceDefinitionRegistry &previous, const PlatformStableIdRegistry &stableIds,
        const PresenceDefinitionRegistryCandidate &candidate, const PlatformDefinitionRegistryLimits &limits = {});
}  // namespace Horo::PlatformServices
