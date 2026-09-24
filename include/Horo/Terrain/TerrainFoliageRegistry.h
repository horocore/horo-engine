#pragma once

/**
 * @file TerrainFoliageRegistry.h
 * @brief Bounded host-owned Terrain/Foliage metadata registry and immutable query snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StrongId.h"
#include "Horo/Terrain/FoliageDefinition.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Terrain {
    namespace Detail {
        struct TerrainFoliageRegistryInstanceTag;
        struct TerrainFoliageRegistryRevisionTag;
    }  // namespace Detail

    /** @brief Host-issued identity of one Terrain/Foliage registry lifetime. */
    using TerrainFoliageRegistryInstanceId =
        Foundation::Detail::NonZeroId64<Detail::TerrainFoliageRegistryInstanceTag, TerrainErrors::IdentityInvalid>;
    /** @brief Monotonic immutable publication generation of one registry. */
    using TerrainFoliageRegistryRevision =
        Foundation::Detail::NonZeroId64<Detail::TerrainFoliageRegistryRevisionTag, TerrainErrors::IdentityInvalid>;

    /** @brief Explicit provider-neutral capabilities available to one host composition. */
    enum class TerrainFoliageCapability : std::uint8_t {
        TerrainQuery,
        FoliageQuery,
        TerrainRuntime,
        FoliageRuntime,
        RenderExtraction,
        PhysicsCollision,
        NavigationBlocking,
        CpuCulling,
        GpuIndirectCulling,
        VertexWind,
        Count,
    };

    /** @brief Compact capability set with a closed vocabulary and no implicit fallback. */
    class TerrainFoliageCapabilitySet final {
    public:
        /** @brief Constructs an empty capability set. */
        constexpr TerrainFoliageCapabilitySet() noexcept = default;

        /** @brief Constructs an empty capability set. @return Empty set. */
        [[nodiscard]] static constexpr TerrainFoliageCapabilitySet Empty() noexcept {
            return {};
        }

        /**
         * @brief Validates and canonicalizes a list of capabilities.
         * @param capabilities Borrowed capability values; duplicates are harmless.
         * @return Exact set or TerrainErrors::RegistryDescriptorInvalid.
         */
        [[nodiscard]] static Result<TerrainFoliageCapabilitySet> Create(std::span<const TerrainFoliageCapability> capabilities);

        /** @brief Checks whether a capability is granted. @param capability Capability to test. @return True when present. */
        [[nodiscard]] constexpr bool Contains(const TerrainFoliageCapability capability) const noexcept {
            const auto index = static_cast<std::uint8_t>(capability);
            return index < static_cast<std::uint8_t>(TerrainFoliageCapability::Count) && (bits_ & (std::uint32_t{1} << index)) != 0;
        }

        /**
         * @brief Checks whether every requested capability is granted.
         * @param required Required capability subset.
         * @return True when this set contains the complete requested set.
         */
        [[nodiscard]] constexpr bool ContainsAll(const TerrainFoliageCapabilitySet required) const noexcept {
            return (bits_ & required.bits_) == required.bits_;
        }

        /** @brief Reports whether no capability is granted. @return True for the empty set. */
        [[nodiscard]] constexpr bool IsEmpty() const noexcept {
            return bits_ == 0;
        }

        /** @brief Rejects unknown bits. @return True for a closed-vocabulary representation. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            constexpr auto count = static_cast<std::uint8_t>(TerrainFoliageCapability::Count);
            return (bits_ & ~(static_cast<std::uint32_t>(std::uint32_t{1} << count) - 1U)) == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainFoliageCapabilitySet &) const noexcept = default;

    private:
        explicit constexpr TerrainFoliageCapabilitySet(const std::uint32_t bits) noexcept : bits_(bits) {}

        std::uint32_t bits_{};
    };

    /** @brief Exact capability projection requested by a consumer from one explicit host composition. */
    struct TerrainFoliageCapabilityProjection final {
        TerrainFoliageCapabilitySet available{}; /**< Exact host grants captured by the registry. */
        TerrainFoliageCapabilitySet required{};  /**< Exact consumer request. */

        /** @brief Reports whether this projection satisfies its request. @return True only for a complete grant. */
        [[nodiscard]] constexpr bool IsSatisfied() const noexcept {
            return available.ContainsAll(required);
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainFoliageCapabilityProjection &) const noexcept = default;
    };

    /**
     * @brief Projects an explicit capability request without selecting a fallback.
     * @param available Capabilities installed by the composition root.
     * @param required Capabilities required by the consumer.
     * @return Satisfied projection or TerrainErrors::CapabilityUnsupported.
     */
    [[nodiscard]] Result<TerrainFoliageCapabilityProjection> ProjectTerrainFoliageCapabilities(TerrainFoliageCapabilitySet available,
                                                                                               TerrainFoliageCapabilitySet required);

    /** @brief Fixed host-selected ceilings for one immutable registry composition. */
    struct TerrainFoliageRegistryLimits final {
        static constexpr std::size_t MaximumDatasets = 4'096;
        static constexpr std::size_t MaximumFoliageTypes = 4'096;
        static constexpr std::size_t MaximumQueryResults = 4'096;

        std::size_t maximumDatasets{MaximumDatasets};         /**< Maximum copied dataset registrations. */
        std::size_t maximumFoliageTypes{MaximumFoliageTypes}; /**< Maximum copied foliage registrations. */
        std::size_t maximumQueryResults{MaximumQueryResults}; /**< Maximum handles emitted by one query. */

        /** @brief Checks positive values against compile-time ceilings. @return True for usable limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumDatasets > 0 && maximumDatasets <= MaximumDatasets && maximumFoliageTypes > 0 &&
                   maximumFoliageTypes <= MaximumFoliageTypes && maximumQueryResults > 0 && maximumQueryResults <= MaximumQueryResults;
        }
    };

    /** @brief Inert typed dataset metadata admitted to the registry. */
    struct TerrainDatasetRegistration final {
        TerrainDatasetDescriptor descriptor;                /**< Previously validated immutable dataset facts. */
        TerrainFeatureTierSet supportedTiers{};             /**< Exact content/host tiers, never a downgrade policy. */
        TerrainFoliageCapabilitySet requiredCapabilities{}; /**< Explicit consumer capabilities required by this dataset. */
    };

    /** @brief Inert typed foliage metadata admitted to the registry. */
    struct TerrainFoliageTypeRegistration final {
        FoliageTypeDefinition definition;                   /**< Previously validated immutable foliage definition. */
        TerrainFoliageCapabilitySet requiredCapabilities{}; /**< Explicit consumer capabilities required by this type. */
    };

    /** @brief Exact registry publication fence attached to every issued handle and query result. */
    struct TerrainFoliageRegistryBinding final {
        TerrainFoliageRegistryInstanceId registry{}; /**< Registry lifetime identity. */
        TerrainFoliageRegistryRevision revision{};   /**< Immutable publication revision. */

        /** @brief Checks both publication identities. @return True when the binding is structurally usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return registry.IsValid() && revision.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainFoliageRegistryBinding &) const noexcept = default;
    };

    /** @brief Generation-pinned dataset handle issued by one immutable snapshot. */
    struct TerrainDatasetRegistryHandle final {
        TerrainFoliageRegistryBinding binding{}; /**< Exact publication that issued the handle. */
        std::uint32_t slot{};                    /**< Dense canonical snapshot slot. */
        TerrainDatasetId dataset{};              /**< Stable dataset occupying the slot. */
        TerrainContentRevision content{};        /**< Exact descriptor content revision. */

        /** @brief Checks representation, not current snapshot membership. @return True when all fields are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return binding.IsValid() && dataset.IsValid() && content.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainDatasetRegistryHandle &) const noexcept = default;
    };

    /** @brief Generation-pinned foliage-type handle issued by one immutable snapshot. */
    struct TerrainFoliageTypeRegistryHandle final {
        TerrainFoliageRegistryBinding binding{}; /**< Exact publication that issued the handle. */
        std::uint32_t slot{};                    /**< Dense canonical snapshot slot. */
        FoliageTypeId type{};                    /**< Stable foliage type occupying the slot. */
        FoliageDefinitionRevision revision{};    /**< Exact definition revision. */

        /** @brief Checks representation, not current snapshot membership. @return True when all fields are usable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return binding.IsValid() && type.IsValid() && revision.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const TerrainFoliageTypeRegistryHandle &) const noexcept = default;
    };

    /** @brief Bounded dataset query with exact optional tier and capability filters. */
    struct TerrainDatasetRegistryQuery final {
        std::optional<TerrainFeatureTier> tier{};           /**< Exact tier filter, or every supported tier. */
        TerrainFoliageCapabilitySet requiredCapabilities{}; /**< Explicit required capabilities. */
    };

    /** @brief Bounded foliage query with an explicit capability filter. */
    struct TerrainFoliageTypeRegistryQuery final {
        TerrainFoliageCapabilitySet requiredCapabilities{}; /**< Explicit required capabilities. */
    };

    /** @brief Publication-fenced work evidence returned by a bounded query. */
    struct TerrainFoliageRegistryQueryResult final {
        TerrainFoliageRegistryBinding binding{}; /**< Exact immutable publication queried. */
        std::size_t matches{};                   /**< Handles written to caller-owned output storage. */
        std::size_t candidatesExamined{};        /**< Number of bounded records inspected. */
    };

    /** @brief Lifecycle gate for registry mutation and snapshot admission. */
    enum class TerrainFoliageRegistryState : std::uint8_t {
        Active,
        Cancelling,
        Closed,
    };

    class TerrainFoliageRegistry;

    /** @brief Immutable owning dataset/type view safe across replacement and registry shutdown. */
    class TerrainFoliageRegistrySnapshot final {
    public:
        struct State;
        TerrainFoliageRegistrySnapshot() = default;

        /** @brief Reports whether the snapshot pins a publication. @return True for a registry-issued snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact publication fence. @return Invalid binding for an empty snapshot. */
        [[nodiscard]] const TerrainFoliageRegistryBinding &Binding() const noexcept;
        /** @brief Returns host capabilities captured at creation. @return Immutable capability set. */
        [[nodiscard]] TerrainFoliageCapabilitySet Capabilities() const noexcept;
        /** @brief Returns datasets in stable identity order. @return Snapshot-owned immutable view. */
        [[nodiscard]] std::span<const TerrainDatasetRegistration> Datasets() const noexcept;
        /** @brief Returns foliage types in stable identity order. @return Snapshot-owned immutable view. */
        [[nodiscard]] std::span<const TerrainFoliageTypeRegistration> FoliageTypes() const noexcept;

        /**
         * @brief Finds one dataset by stable identity without allocation.
         * @param dataset Stable dataset identity.
         * @return Generation-pinned handle or a typed invalid/unknown failure.
         */
        [[nodiscard]] Result<TerrainDatasetRegistryHandle> FindDataset(TerrainDatasetId dataset) const;

        /**
         * @brief Finds one foliage type by stable identity without allocation.
         * @param type Stable foliage type identity.
         * @return Generation-pinned handle or a typed invalid/unknown failure.
         */
        [[nodiscard]] Result<TerrainFoliageTypeRegistryHandle> FindFoliageType(FoliageTypeId type) const;

        /**
         * @brief Queries datasets in canonical order into caller-owned bounded storage.
         * @param query Exact optional tier and capability requirements.
         * @param output Caller-owned handle storage; it is unchanged on failure.
         * @return Publication-fenced query evidence or a typed invalid/capacity/capability failure.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryQueryResult> QueryDatasets(const TerrainDatasetRegistryQuery &query,
                                                                              std::span<TerrainDatasetRegistryHandle> output) const;

        /**
         * @brief Queries foliage types in canonical order into caller-owned bounded storage.
         * @param query Explicit capability requirements.
         * @param output Caller-owned handle storage; it is unchanged on failure.
         * @return Publication-fenced query evidence or a typed invalid/capacity/capability failure.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryQueryResult> QueryFoliageTypes(const TerrainFoliageTypeRegistryQuery &query,
                                                                                  std::span<TerrainFoliageTypeRegistryHandle> output) const;

        /**
         * @brief Resolves a dataset handle against this exact publication.
         * @param handle Generation-pinned dataset handle.
         * @return Borrowed registration or a typed invalid/stale failure.
         */
        [[nodiscard]] Result<const TerrainDatasetRegistration *> Resolve(const TerrainDatasetRegistryHandle &handle) const;

        /**
         * @brief Resolves a foliage handle against this exact publication.
         * @param handle Generation-pinned foliage handle.
         * @return Borrowed registration or a typed invalid/stale failure.
         */
        [[nodiscard]] Result<const TerrainFoliageTypeRegistration *> Resolve(const TerrainFoliageTypeRegistryHandle &handle) const;

        /** @brief Projects a required capability set without fallback or backend discovery. */
        [[nodiscard]] Result<TerrainFoliageCapabilityProjection> ProjectCapabilities(TerrainFoliageCapabilitySet required) const;

    private:
        friend class TerrainFoliageRegistry;

        explicit TerrainFoliageRegistrySnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /** @brief Host-owned bounded metadata registry; mutation belongs to its composition owner. */
    class TerrainFoliageRegistry final {
    public:
        TerrainFoliageRegistry() = delete;
        ~TerrainFoliageRegistry();
        TerrainFoliageRegistry(TerrainFoliageRegistry &&) noexcept;
        TerrainFoliageRegistry &operator=(TerrainFoliageRegistry &&) noexcept;
        TerrainFoliageRegistry(const TerrainFoliageRegistry &) = delete;
        TerrainFoliageRegistry &operator=(const TerrainFoliageRegistry &) = delete;

        /**
         * @brief Creates an empty registry without discovering services or starting feature work.
         * @param instance Host-issued registry lifetime identity.
         * @param capabilities Exact capabilities explicitly composed by the host.
         * @param limits Independent finite storage and query ceilings.
         * @return Registry or a typed invalid/capacity/storage failure.
         */
        [[nodiscard]] static Result<TerrainFoliageRegistry> Create(TerrainFoliageRegistryInstanceId instance,
                                                                   TerrainFoliageCapabilitySet capabilities,
                                                                   TerrainFoliageRegistryLimits limits = {});

        /**
         * @brief Registers one inert dataset descriptor.
         * @param registration Previously validated typed metadata.
         * @return New publication revision or typed duplicate/capacity/capability/lifecycle failure.
         * @pre Called by the single composition owner; no callback or feature lifecycle is invoked.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryRevision> RegisterDataset(TerrainDatasetRegistration registration);

        /**
         * @brief Replaces one dataset at the exact next content revision and coherent bounds revision.
         * @param registration Complete detached replacement.
         * @return New revision or typed unknown/stale/capability/lifecycle failure; failure preserves publication.
         * @pre Content advances exactly once; unchanged bounds retain their revision, while changed bounds advance exactly once.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryRevision> ReplaceDataset(TerrainDatasetRegistration registration);

        /** @brief Removes one dataset from future snapshots. @return Whether it existed, or a lifecycle/identity failure. */
        [[nodiscard]] Result<bool> UnregisterDataset(TerrainDatasetId dataset);

        /**
         * @brief Registers one inert foliage definition.
         * @param registration Previously validated typed metadata.
         * @return New publication revision or typed duplicate/capacity/capability/lifecycle failure.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryRevision> RegisterFoliageType(TerrainFoliageTypeRegistration registration);

        /**
         * @brief Replaces one foliage definition at the exact next semantic revision.
         * @param registration Complete detached replacement.
         * @return New revision or typed unknown/stale/capability/lifecycle failure; failure preserves publication.
         */
        [[nodiscard]] Result<TerrainFoliageRegistryRevision> ReplaceFoliageType(TerrainFoliageTypeRegistration registration);

        /** @brief Removes one foliage type from future snapshots. @return Whether it existed, or a lifecycle/identity failure. */
        [[nodiscard]] Result<bool> UnregisterFoliageType(FoliageTypeId type);

        /**
         * @brief Pins the current immutable publication for bounded concurrent readers.
         * @note Capture is synchronized with publication and lifecycle transitions performed by the composition owner.
         */
        [[nodiscard]] Result<TerrainFoliageRegistrySnapshot> Snapshot() const;

        /** @brief Stops new mutations and snapshot capture; retained snapshots remain valid. */
        void BeginCancellation() noexcept;
        /** @brief Closes the registry and releases its live publication; retained snapshots remain valid. */
        void Shutdown() noexcept;
        /** @brief Returns the current lifecycle state. @return Active, cancelling, or closed. */
        [[nodiscard]] TerrainFoliageRegistryState Lifecycle() const noexcept;

    private:
        TerrainFoliageRegistry(TerrainFoliageRegistryInstanceId instance, TerrainFoliageCapabilitySet capabilities,
                               TerrainFoliageRegistryLimits limits, std::shared_ptr<const TerrainFoliageRegistrySnapshot::State> state);

        [[nodiscard]] Result<TerrainFoliageRegistryRevision> Publish(std::vector<TerrainDatasetRegistration> datasets,
                                                                     std::vector<TerrainFoliageTypeRegistration> foliageTypes);

        TerrainFoliageRegistryInstanceId instance_{};
        TerrainFoliageCapabilitySet capabilities_{};
        TerrainFoliageRegistryLimits limits_{};
        mutable std::mutex snapshotMutex_;
        std::shared_ptr<const TerrainFoliageRegistrySnapshot::State> state_;
        TerrainFoliageRegistryState lifecycle_{TerrainFoliageRegistryState::Active};
    };
}  // namespace Horo::Terrain
