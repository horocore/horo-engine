#pragma once

/**
 * @file DestructibleDescriptor.h
 * @brief Immutable destructible policy, provider-neutral feature tiers, and finite admission limits.
 */

#include "Horo/Destruction/DestructionIdentity.h"

#include <compare>
#include <cstdint>

namespace Horo::Destruction {
    /** @brief Version of the portable descriptor contract with fixed-tick damage cooldown policy. */
    inline constexpr std::uint32_t CurrentDestructibleDescriptorContractVersion = 2;

    struct DestructionConfigurationRevisionTag;
    /** @brief Non-zero immutable publication revision of one destructible configuration. */
    using DestructionConfigurationRevision = DestructionStableIdentity<DestructionConfigurationRevisionTag>;

    /** @brief Provider-neutral product preference; a tier grants no capability by itself. */
    enum class DestructionFeatureTier : std::uint8_t {
        Baseline,
        Standard,
        High,
        Count
    };

    /** @brief Closed core feature vocabulary independent of Physics, Render, platform, or provider names. */
    enum class DestructionFeature : std::uint8_t {
        PreCookedFracture,
        CookedSupport,
        StagedActivation,
        HierarchicalFracture,
        CosmeticDebris,
        DurableDormancy,
        AuthoritativeReplication,
        RuntimeGeometryGeneration,
        Count
    };

    /** @brief Fixed-width feature bits retained as inert descriptor data. */
    struct DestructionFeatureSet final {
        std::uint32_t bits{}; /**< One bit per known DestructionFeature. */

        /** @brief Tests one known feature without interpreting malformed enum values. @param feature Feature to query.
         * @return True only when the feature is known and its bit is present.
         */
        [[nodiscard]] constexpr bool Contains(const DestructionFeature feature) const noexcept {
            const auto index = static_cast<std::uint32_t>(feature);
            return index < static_cast<std::uint32_t>(DestructionFeature::Count) && (bits & (std::uint32_t{1} << index)) != 0;
        }

        /** @brief Checks that no unknown feature bit is present. @return True for a canonical closed set. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            constexpr auto count = static_cast<std::uint32_t>(DestructionFeature::Count);
            return (bits & ~((std::uint32_t{1} << count) - 1U)) == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const DestructionFeatureSet &) const noexcept = default;
    };

    /** @brief Produces the canonical bit for a compile-time-known feature. */
    template <DestructionFeature Feature>
    inline constexpr std::uint32_t DestructionFeatureBit = [] {
        static_assert(Feature < DestructionFeature::Count, "Feature must belong to the closed core vocabulary");
        return std::uint32_t{1} << static_cast<std::uint32_t>(Feature);
    }();

    /** @brief Required and explicitly optional features; the sets must be valid and disjoint. */
    struct DestructionFeatureRequirements final {
        DestructionFeatureSet required{}; /**< Missing required support rejects admission. */
        DestructionFeatureSet optional{}; /**< Unsupported optional support is omitted, never silently substituted. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionFeatureRequirements &) const noexcept = default;
    };

    /** @brief Compile-time safety ceilings for all core destruction tiers. */
    struct DestructionHardLimits final {
        static constexpr std::uint32_t ChunksPerDestructible = 1'024;                      /**< Absolute stable-chunk ceiling. */
        static constexpr std::uint32_t HierarchyDepth = 8;                                 /**< Absolute root-inclusive depth ceiling. */
        static constexpr std::uint32_t ActiveChunkBodies = 1'024;                          /**< Absolute active-body ceiling. */
        static constexpr std::uint32_t EventsPerTransition = 4'096;                        /**< Absolute event batch ceiling. */
        static constexpr std::uint32_t EventJournalEntries = 16'384;                       /**< Absolute journal capacity ceiling. */
        static constexpr std::uint32_t CosmeticDebrisParticles = 4'096;                    /**< Absolute cosmetic particle ceiling. */
        static constexpr std::uint64_t ArtifactBytes = 256ULL * 1024ULL * 1024ULL;         /**< Absolute artifact byte ceiling. */
        static constexpr std::uint64_t TransitionBytes = 512ULL * 1024ULL * 1024ULL;       /**< Absolute staging byte ceiling. */
        static constexpr std::uint64_t ResidentBytes = 1ULL * 1024ULL * 1024ULL * 1024ULL; /**< Absolute resident ceiling. */
        static constexpr std::uint64_t WorkItemsPerTransition = 524'288;                   /**< Absolute bounded-work ceiling. */
    };

    /** @brief Complete finite ceilings captured by authoring, cook, packaging, and runtime admission. */
    struct DestructionLimits final {
        std::uint32_t maximumChunksPerDestructible{};   /**< Stable chunks admitted for one destructible. */
        std::uint32_t maximumHierarchyDepth{};          /**< Root-inclusive cooked hierarchy depth. */
        std::uint32_t maximumActiveChunkBodies{};       /**< Peak simultaneously active chunk bodies. */
        std::uint32_t maximumEventsPerTransition{};     /**< Peak facts published by one transition. */
        std::uint32_t maximumEventJournalEntries{};     /**< Retained canonical occurrence capacity. */
        std::uint32_t maximumCosmeticDebrisParticles{}; /**< Peak non-canonical cosmetic particles. */
        std::uint64_t maximumArtifactBytes{};           /**< Decoded canonical artifact bytes. */
        std::uint64_t maximumTransitionBytes{};         /**< Peak candidate/replacement staging bytes. */
        std::uint64_t maximumResidentBytes{};           /**< Peak complete live resident bytes. */
        std::uint64_t maximumWorkItemsPerTransition{};  /**< Deterministic planning work ceiling. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionLimits &) const noexcept = default;
    };

    /** @brief Canonical exact core profile for one product preference tier. */
    struct DestructionTierProfile final {
        DestructionFeatureTier tier{DestructionFeatureTier::Baseline}; /**< Exact tier, never an ordered fallback request. */
        DestructionFeatureSet supportedFeatures{};                     /**< Product support available for intersection. */
        DestructionLimits limits{};                                    /**< Maximum values a descriptor may lower. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionTierProfile &) const noexcept = default;
    };

    /** @brief Health thresholds expressed in canonical positive health units. */
    struct DestructionHealthPolicy final {
        float maximumHealth{100.0F};         /**< Intact starting and upper-clamped health. */
        float damagedHealthThreshold{75.0F}; /**< Health at or below which the object is Damaged. */
        float fractureHealthThreshold{0.0F}; /**< Health at or below which pre-cooked fracture is eligible. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionHealthPolicy &) const noexcept = default;
    };

    /** @brief Typed command sources allowed by the destructible's authored behavior. */
    enum class DestructionTriggerPolicy : std::uint8_t {
        ExplicitOnly,
        AccumulatedDamage,
        DamageAndContact,
        Count
    };

    /** @brief Cooked support evaluation requested by this descriptor. */
    enum class DestructionSupportPolicy : std::uint8_t {
        Disabled,
        CookedReachability,
        CookedHierarchy,
        Count
    };

    /** @brief Whether an authorized later state-machine contract may admit repair/reset. */
    enum class DestructionRepairPolicy : std::uint8_t {
        Forbidden,
        Authorized,
        Count
    };

    /** @brief Complete mutation behavior without native callbacks or backend policy. */
    struct DestructionBehaviorPolicy final {
        DestructionTriggerPolicy trigger{DestructionTriggerPolicy::AccumulatedDamage};  /**< Admitted command/evidence sources. */
        DestructionSupportPolicy support{DestructionSupportPolicy::CookedReachability}; /**< Exact cooked support policy. */
        DestructionRepairPolicy repair{DestructionRepairPolicy::Forbidden};             /**< Explicit repair capability intent. */
        std::uint32_t
            minimumDamageIntervalTicks{}; /**< Minimum fixed-tick gap between committed damage commands; zero disables cooldown. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionBehaviorPolicy &) const noexcept = default;
    };

    /** @brief Canonical chunk retention classification; sleep and visibility never select it. */
    enum class DestructionChunkRetention : std::uint8_t {
        GameplayAuthoritative,
        DurableDormancy,
        Count
    };

    /** @brief Explicit cosmetic debris behavior, separate from canonical chunk retention. */
    enum class DestructionDebrisPolicy : std::uint8_t {
        Disabled,
        FiniteLifetime,
        Count
    };

    /** @brief Cleanup intent whose timing applies only to cosmetic debris. */
    struct DestructionCleanupPolicy final {
        DestructionChunkRetention chunkRetention{DestructionChunkRetention::GameplayAuthoritative}; /**< Durable chunk policy. */
        DestructionDebrisPolicy debris{DestructionDebrisPolicy::Disabled};                          /**< Cosmetic-only debris policy. */
        float debrisLifetimeSeconds{}; /**< Positive finite lifetime only when debris is enabled. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionCleanupPolicy &) const noexcept = default;
    };

    /** @brief Canonical authority/replication intent; client prediction never grants authority. */
    enum class DestructionReplicationIntent : std::uint8_t {
        LocalAuthority,
        ServerAuthoritative,
        Count
    };

    /** @brief Mutable construction data validated and copied into an immutable descriptor. */
    struct DestructibleDescriptorData final {
        std::uint32_t contractVersion{CurrentDestructibleDescriptorContractVersion}; /**< Portable schema version. */
        DestructibleId destructible{};                                               /**< Stable authored owner. */
        FractureArtifactContentIdentity content{};                                   /**< Exact immutable cooked content. */
        DestructionConfigurationRevision configurationRevision{};                    /**< Exact policy publication. */
        DestructionFeatureTier tier{DestructionFeatureTier::Baseline};               /**< Exact product preference. */
        DestructionFeatureRequirements features{};                                   /**< Required and optional feature intent. */
        DestructionLimits limits{};                                                  /**< Explicit limits no wider than the tier. */
        DestructionHealthPolicy health{};                                            /**< Canonical health thresholds. */
        DestructionBehaviorPolicy behavior{};                                        /**< Typed trigger/support/repair intent. */
        DestructionCleanupPolicy cleanup{};                                          /**< Durable and cosmetic cleanup intent. */
        DestructionReplicationIntent replication{DestructionReplicationIntent::LocalAuthority}; /**< Authority topology intent. */

        [[nodiscard]] constexpr auto operator<=>(const DestructibleDescriptorData &) const noexcept = default;
    };

    /** @brief Finite artifact facts checked before allocation, preparation, or partial publication. */
    struct DestructionArtifactFootprint final {
        std::uint32_t chunkCount{};                   /**< Stable chunks in the exact artifact. */
        std::uint32_t hierarchyDepth{};               /**< Root-inclusive cooked hierarchy depth. */
        std::uint32_t peakActiveChunkBodies{};        /**< Peak required Physics bodies. */
        std::uint32_t peakEventsPerTransition{};      /**< Peak canonical event batch size. */
        std::uint32_t requestedEventJournalEntries{}; /**< Required retained canonical journal capacity. */
        std::uint32_t peakCosmeticDebrisParticles{};  /**< Peak optional cosmetic particles. */
        std::uint64_t artifactBytes{};                /**< Decoded canonical artifact bytes. */
        std::uint64_t peakTransitionBytes{};          /**< Candidate plus replacement staging bytes. */
        std::uint64_t peakResidentBytes{};            /**< Complete live old/new resident bytes. */
        std::uint64_t peakWorkItemsPerTransition{};   /**< Complete deterministic planning work. */

        [[nodiscard]] constexpr auto operator<=>(const DestructionArtifactFootprint &) const noexcept = default;
    };

    /**
     * @brief Immutable validated destructible configuration.
     *
     * The value owns fixed-size backend-neutral data and is thread-compatible after creation. It owns no runtime world,
     * artifact lease, callback, queue, worker, native resource, or mutation authority. Dropping it therefore requires no
     * cancellation or shutdown coordination. Runtime owners must revalidate its exact generation, content and configuration
     * revision at admission and aggregate publication.
     */
    class DestructibleDescriptor final {
    public:
        /**
         * @brief Validates and copies one complete descriptor before any runtime allocation.
         * @param data Candidate policy and exact tier limits.
         * @return Immutable descriptor or a typed malformed, unsupported-feature, runtime-geometry, or limit failure.
         * @post Failure leaves the input unchanged and publishes no partial state.
         */
        [[nodiscard]] static Result<DestructibleDescriptor> Create(const DestructibleDescriptorData &data);

        /** @brief Returns the complete captured configuration. @return Borrowed immutable descriptor data. */
        [[nodiscard]] const DestructibleDescriptorData &Data() const noexcept;
        /** @brief Returns the supported required/optional intersection selected without fallback. @return Effective features. */
        [[nodiscard]] DestructionFeatureSet EffectiveFeatures() const noexcept;

    private:
        DestructibleDescriptor(const DestructibleDescriptorData &data, DestructionFeatureSet effectiveFeatures) noexcept;

        DestructibleDescriptorData data_;
        DestructionFeatureSet effectiveFeatures_;
    };

    /**
     * @brief Returns the exact canonical core profile for one known provider-neutral tier.
     * @param tier Exact requested tier; no lower or higher tier is selected.
     * @return Profile or DestructionErrors::TierInvalid for an unknown value.
     */
    [[nodiscard]] Result<DestructionTierProfile> GetDestructionTierProfile(DestructionFeatureTier tier);

    /**
     * @brief Validates exact descriptor, runtime generation, content revision, and finite artifact footprint before work.
     * @param descriptor Immutable captured configuration.
     * @param submittedTarget Target retained by the caller.
     * @param currentTarget Current owner identity at the admission boundary.
     * @param currentConfigurationRevision Current immutable configuration publication revision.
     * @param currentContent Current published fracture content identity.
     * @param footprint Exact decoded/cooked counts and peak cost, measured before allocation.
     * @return Success or a typed invalid, stale-generation, stale-configuration, stale-content, or limit failure.
     * @post Success grants no ownership and performs no allocation, queue mutation, callback, native work, or publication.
     */
    [[nodiscard]] Result<void> AdmitDestructibleDescriptor(const DestructibleDescriptor &descriptor, DestructionHandle submittedTarget,
                                                           DestructionHandle currentTarget,
                                                           DestructionConfigurationRevision currentConfigurationRevision,
                                                           const FractureArtifactContentIdentity &currentContent,
                                                           const DestructionArtifactFootprint &footprint);
}  // namespace Horo::Destruction
