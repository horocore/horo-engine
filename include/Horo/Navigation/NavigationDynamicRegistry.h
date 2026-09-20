#pragma once

/**
 * @file NavigationDynamicRegistry.h
 * @brief Generation-safe Scene-scoped dynamic navigation obstacles, modifiers, and immutable snapshots.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Navigation/NavigationAreas.h"
#include "Horo/Navigation/NavigationIdentity.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <variant>

namespace Horo::Navigation {
    namespace Detail {
        struct NavigationDynamicRegistryState;
        struct NavigationDynamicRegistrySnapshotState;
    }  // namespace Detail

    struct NavigationWorldActivationDescriptor;

    /** @brief Version of the provider-neutral dynamic registry value contract. */
    inline constexpr std::uint32_t CurrentNavigationDynamicRegistryContractVersion = 1;

    struct NavigationDynamicRegistryRevisionTag;
    struct NavigationDynamicRecordRevisionTag;

    /** @brief Monotonic immutable publication revision of one dynamic registry. */
    using NavigationDynamicRegistryRevision = NavigationIdentity<NavigationDynamicRegistryRevisionTag>;
    /** @brief Monotonic semantic revision of one obstacle or modifier record. */
    using NavigationDynamicRecordRevision = NavigationIdentity<NavigationDynamicRecordRevisionTag>;

    /** @brief Source family retained in dynamic-record provenance. */
    enum class NavigationDynamicSourceKind : std::uint8_t {
        SceneEntity,
        AuthoredModifier,
        GameplaySystem,
        Count,
    };

    /** @brief Project-defined layer mask used to select obstacle and modifier consumers. */
    struct NavigationDynamicLayerMask final {
        std::uint64_t bits{};

        /** @brief Tests whether two contributions share at least one consumer layer. */
        [[nodiscard]] constexpr bool Intersects(const NavigationDynamicLayerMask other) const noexcept {
            return (bits & other.bits) != 0;
        }

        /** @brief Reports whether no consumer layer is selected. */
        [[nodiscard]] constexpr bool Empty() const noexcept {
            return bits == 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicLayerMask &) const noexcept = default;
    };

    /**
     * @brief Exact Scene, owner, and source evidence for one dynamic contribution.
     * @details The owner generation is independent from the registry slot generation. It prevents a destroyed Scene
     * entity or gameplay owner from updating a replacement that reuses the same logical slot.
     */
    struct NavigationDynamicProvenance final {
        NavigationWorldId world;                          /**< Exact navigation-world incarnation. */
        NavigationSceneRuntimeId scene;                   /**< Exact runtime-Scene incarnation. */
        NavigationSceneGeneration sceneGeneration;        /**< Exact Scene activation generation. */
        NavigationDynamicOwnerId owner;                   /**< Stable owner identity supplied by host composition. */
        NavigationDynamicOwnerGeneration ownerGeneration; /**< Exact owner incarnation. */
        NavigationDynamicSourceRevision sourceRevision;   /**< Monotonic source revision for this semantic contribution. */
        NavigationDynamicSourceKind source{NavigationDynamicSourceKind::SceneEntity}; /**< Provenance family. */
        NavigationModifierId authoredModifier; /**< Authored modifier identity, required only for that source kind. */

        /** @brief Validates all identity, source-kind, and authored-reference invariants. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicProvenance &) const noexcept = default;
    };

    /** @brief Provider-neutral axis-aligned box used by bounded dynamic overlay and carving policies. */
    struct NavigationDynamicBoxShape final {
        Math::Vec3 center{};
        Math::Vec3 halfExtents{1.0F, 1.0F, 1.0F};

        /** @brief Validates finite center and strictly positive finite extents. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicBoxShape &) const noexcept = default;
    };

    /** @brief Provider-neutral vertical cylinder used by bounded dynamic overlay and carving policies. */
    struct NavigationDynamicCylinderShape final {
        Math::Vec3 center{};
        float radius{1.0F};
        float halfHeight{1.0F};

        /** @brief Validates finite center and strictly positive finite dimensions. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicCylinderShape &) const noexcept = default;
    };

    /** @brief Closed provider-neutral shape vocabulary for dynamic navigation contributions. */
    using NavigationDynamicShape = std::variant<NavigationDynamicBoxShape, NavigationDynamicCylinderShape>;

    /** @brief Semantic filter or cost operation contributed by one runtime modifier. */
    enum class NavigationDynamicModifierOperation : std::uint8_t {
        Exclude,
        OverrideArea,
        OverrideAreaAndCost,
        Count,
    };

    /** @brief Stable obstacle input staged for the next Scene navigation safe point. */
    struct NavigationObstacleDescriptor final {
        NavigationObstacleId id;                                   /**< Stable logical obstacle identity. */
        NavigationDynamicProvenance provenance;                    /**< Exact Scene/owner/source evidence. */
        NavigationDynamicShape shape{NavigationDynamicBoxShape{}}; /**< Bounded provider-neutral shape. */
        NavigationDynamicLayerMask layers{1};                      /**< Consumer layers affected by this obstacle. */
        std::int32_t priority{};                                   /**< Stable policy priority; larger values win. */
        std::uint64_t updateTick{};                                /**< Fixed-tick visibility target for this value. */
        bool enabled{true};                                        /**< Disabled records retain identity but have no active effect. */

        /** @brief Validates the complete detached descriptor. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Stable modifier input staged for the next Scene navigation safe point. */
    struct NavigationModifierDescriptor final {
        NavigationModifierId id;                                   /**< Stable authored or runtime modifier identity. */
        NavigationDynamicProvenance provenance;                    /**< Exact Scene/owner/source evidence. */
        NavigationDynamicShape shape{NavigationDynamicBoxShape{}}; /**< Bounded provider-neutral shape. */
        NavigationDynamicLayerMask layers{1};                      /**< Consumer layers affected by this modifier. */
        std::int32_t priority{};                                   /**< Stable policy priority; larger values win. */
        NavigationDynamicModifierOperation operation{NavigationDynamicModifierOperation::Exclude};
        std::optional<NavigationAreaId> area; /**< Required for area override operations. */
        std::optional<float> traversalCost;   /**< Required only for area-and-cost override. */
        std::uint64_t updateTick{};           /**< Fixed-tick visibility target for this value. */
        bool enabled{true};                   /**< Disabled records retain identity but have no active effect. */

        /** @brief Validates the complete detached descriptor and operation payload. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Compile-time ceilings for one dynamic registry publication and staged batch. */
    struct NavigationDynamicRegistryHardLimits final {
        static constexpr std::size_t Obstacles = 4096;
        static constexpr std::size_t Modifiers = 4096;
        static constexpr std::size_t PendingCommands = 8192;
        static constexpr std::size_t MutationsPerCommit = 2048;
        static constexpr std::uint64_t MaximumUpdateIntervalTicks = 1'000'000;
    };

    /** @brief Product/profile limits that may only narrow dynamic registry hard ceilings. */
    struct NavigationDynamicRegistryLimits final {
        std::size_t maximumObstacles{NavigationDynamicRegistryHardLimits::Obstacles};
        std::size_t maximumModifiers{NavigationDynamicRegistryHardLimits::Modifiers};
        std::size_t maximumPendingCommands{NavigationDynamicRegistryHardLimits::PendingCommands};
        std::size_t maximumMutationsPerCommit{NavigationDynamicRegistryHardLimits::MutationsPerCommit};
        std::uint64_t minimumUpdateIntervalTicks{1};

        /** @brief Validates all positive product limits against compile-time ceilings. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicRegistryLimits &) const noexcept = default;
    };

    /** @brief Exact Scene/world fence used by dynamic registry publication. */
    struct NavigationDynamicSceneBinding final {
        NavigationWorldId world;
        NavigationSceneRuntimeId scene;
        NavigationSceneGeneration sceneGeneration;

        /** @brief Checks that every replacement fence is non-zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return world.IsValid() && scene.IsValid() && sceneGeneration.IsValid();
        }

        [[nodiscard]] constexpr auto operator<=>(const NavigationDynamicSceneBinding &) const noexcept = default;
    };

    /** @brief Immutable active obstacle record copied into registry snapshots. */
    struct NavigationObstacleRecord final {
        NavigationObstacleHandle handle;
        NavigationObstacleId id;
        NavigationDynamicProvenance provenance;
        NavigationDynamicShape shape{NavigationDynamicBoxShape{}};
        NavigationDynamicLayerMask layers{1};
        std::int32_t priority{};
        std::uint64_t lastUpdateTick{};
        NavigationDynamicRecordRevision revision;
        bool enabled{true};
    };

    /** @brief Immutable active modifier record copied into registry snapshots. */
    struct NavigationModifierRecord final {
        NavigationModifierHandle handle;
        NavigationModifierId id;
        NavigationDynamicProvenance provenance;
        NavigationDynamicShape shape{NavigationDynamicBoxShape{}};
        NavigationDynamicLayerMask layers{1};
        std::int32_t priority{};
        NavigationDynamicModifierOperation operation{NavigationDynamicModifierOperation::Exclude};
        std::optional<NavigationAreaId> area;
        std::optional<float> traversalCost;
        std::uint64_t lastUpdateTick{};
        NavigationDynamicRecordRevision revision;
        bool enabled{true};
    };

    /** @brief Counts and exact publication evidence returned by one committed staged batch. */
    struct NavigationDynamicCommitResult final {
        NavigationDynamicSceneBinding binding;
        NavigationDynamicRegistryRevision revision;
        std::size_t registrations{};
        std::size_t updates{};
        std::size_t removals{};
    };

    /**
     * @brief Immutable value snapshot safe to retain and read from query or avoidance jobs.
     * @details The snapshot owns fixed-capacity copies. It contains no registry pointer, mutable container, provider handle,
     * callback, or Scene lifetime lease; currentness must be checked separately against the binding and revision.
     */
    class NavigationDynamicRegistrySnapshot final {
    public:
        /** @brief Constructs an invalid empty snapshot. */
        NavigationDynamicRegistrySnapshot() noexcept = default;

        /** @brief Reports whether a Scene-bound publication was captured. */
        [[nodiscard]] bool IsValid() const noexcept;

        /** @brief Returns the exact Scene/world fence observed by this snapshot. @return Immutable binding value. */
        [[nodiscard]] constexpr const NavigationDynamicSceneBinding &Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns the immutable publication revision. @return Registry revision, or invalid for an empty snapshot. */
        [[nodiscard]] constexpr NavigationDynamicRegistryRevision Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns identity/priority-ordered obstacle records. @return Snapshot-owned immutable records. */
        [[nodiscard]] std::span<const NavigationObstacleRecord> Obstacles() const noexcept;
        /** @brief Returns identity/priority-ordered modifier records. @return Snapshot-owned immutable records. */
        [[nodiscard]] std::span<const NavigationModifierRecord> Modifiers() const noexcept;
        /** @brief Resolves one exact obstacle handle against this publication.
         * @param handle Generation-safe obstacle handle.
         * @return Record copy or a typed invalid/stale-handle error.
         */
        [[nodiscard]] Result<NavigationObstacleRecord> FindObstacle(NavigationObstacleHandle handle) const;
        /** @brief Resolves one exact modifier handle against this publication.
         * @param handle Generation-safe modifier handle.
         * @return Record copy or a typed invalid/stale-handle error.
         */
        [[nodiscard]] Result<NavigationModifierRecord> FindModifier(NavigationModifierHandle handle) const;

    private:
        friend class NavigationDynamicRegistry;
        explicit NavigationDynamicRegistrySnapshot(std::shared_ptr<const Detail::NavigationDynamicRegistrySnapshotState> records) noexcept;
        NavigationDynamicSceneBinding binding_;
        NavigationDynamicRegistryRevision revision_;
        std::shared_ptr<const Detail::NavigationDynamicRegistrySnapshotState> records_;
    };

    /**
     * @brief Owner-thread staged registry for Scene-scoped dynamic obstacles and semantic modifiers.
     * @details Stage methods only retain bounded value commands. The active records change exclusively at
     * CommitAtSafePoint, so query and avoidance jobs can read an immutable snapshot without racing owner mutation.
     */
    class NavigationDynamicRegistry final {
    public:
        /** @brief Creates an empty registry with validated profile limits.
         * @param limits Product/profile bounds; every value must remain within hard ceilings.
         * @return Registry or a typed invalid/capacity error.
         */
        [[nodiscard]] static Result<NavigationDynamicRegistry> Create(const NavigationDynamicRegistryLimits &limits = {});

        NavigationDynamicRegistry(const NavigationDynamicRegistry &) = delete;
        NavigationDynamicRegistry &operator=(const NavigationDynamicRegistry &) = delete;
        NavigationDynamicRegistry(NavigationDynamicRegistry &&) noexcept;
        NavigationDynamicRegistry &operator=(NavigationDynamicRegistry &&) = delete;
        ~NavigationDynamicRegistry();

        /** @brief Stages a new obstacle and returns a handle that becomes active at the next successful commit.
         * @param descriptor Detached provider-neutral obstacle value.
         * @return Reserved generation-safe handle or a typed invalid/conflict/capacity/shutdown error.
         */
        [[nodiscard]] Result<NavigationObstacleHandle> StageRegisterObstacle(const NavigationObstacleDescriptor &descriptor);
        /** @brief Stages a new modifier and returns a handle that becomes active at the next successful commit.
         * @param descriptor Detached provider-neutral modifier value.
         * @return Reserved generation-safe handle or a typed invalid/conflict/capacity/shutdown error.
         */
        [[nodiscard]] Result<NavigationModifierHandle> StageRegisterModifier(const NavigationModifierDescriptor &descriptor);
        /** @brief Stages an exact-generation obstacle replacement.
         * @param handle Active obstacle handle to replace.
         * @param expectedRevision Revision observed by the caller.
         * @param descriptor Detached replacement value with newer source evidence.
         * @return Success or a typed invalid, stale, conflict, rate, capacity, or shutdown error.
         */
        [[nodiscard]] Result<void> StageUpdateObstacle(NavigationObstacleHandle handle, NavigationDynamicRecordRevision expectedRevision,
                                                       const NavigationObstacleDescriptor &descriptor);
        /** @brief Stages an exact-generation modifier replacement.
         * @param handle Active modifier handle to replace.
         * @param expectedRevision Revision observed by the caller.
         * @param descriptor Detached replacement value with newer source evidence.
         * @return Success or a typed invalid, stale, conflict, rate, capacity, or shutdown error.
         */
        [[nodiscard]] Result<void> StageUpdateModifier(NavigationModifierHandle handle, NavigationDynamicRecordRevision expectedRevision,
                                                       const NavigationModifierDescriptor &descriptor);
        /** @brief Stages removal of an exact-generation obstacle record.
         * @param handle Active obstacle handle to remove.
         * @param expectedRevision Revision observed by the caller.
         * @return Success or a typed invalid, stale, conflict, capacity, or shutdown error.
         */
        [[nodiscard]] Result<void> StageRemoveObstacle(NavigationObstacleHandle handle, NavigationDynamicRecordRevision expectedRevision);
        /** @brief Stages removal of an exact-generation modifier record.
         * @param handle Active modifier handle to remove.
         * @param expectedRevision Revision observed by the caller.
         * @return Success or a typed invalid, stale, conflict, capacity, or shutdown error.
         */
        [[nodiscard]] Result<void> StageRemoveModifier(NavigationModifierHandle handle, NavigationDynamicRecordRevision expectedRevision);

        /**
         * @brief Applies the staged batch atomically at the owner Scene safe point.
         * @param activation Exact active Scene/world activation fence.
         * @param targetTick Fixed simulation tick at which the batch becomes visible.
         * @return Commit evidence or a typed invalid, stale, capacity, rate, or shutdown failure; failure preserves active state.
         */
        [[nodiscard]] Result<NavigationDynamicCommitResult> CommitAtSafePoint(const NavigationWorldActivationDescriptor &activation,
                                                                              std::uint64_t targetTick);

        /**
         * @brief Atomically drops the old Scene publication and binds a replacement Scene at a safe point.
         * @param activation Exact replacement Scene/world activation fence.
         * @param targetTick Fixed simulation tick for the empty replacement publication.
         * @return Empty replacement publication or typed failure; stale pending commands are not applied.
         */
        [[nodiscard]] Result<NavigationDynamicCommitResult> ReplaceSceneAtSafePoint(const NavigationWorldActivationDescriptor &activation,
                                                                                    std::uint64_t targetTick);

        /** @brief Drops staged commands and burns any issued pending slot generations. */
        void ClearStaged() noexcept;
        /** @brief Captures an immutable value snapshot of the active publication.
         * @return Snapshot or a typed no-data, capacity, or shutdown error.
         */
        [[nodiscard]] Result<NavigationDynamicRegistrySnapshot> Snapshot() const;
        /** @brief Closes staging and snapshot admission; retained snapshots remain valid. */
        void BeginShutdown() noexcept;

        /** @brief Reports whether teardown has closed this registry. */
        [[nodiscard]] constexpr bool IsShutdown() const noexcept {
            return shutdown_;
        }

        /** @brief Returns the current publication revision, or invalid before first Scene binding. */
        [[nodiscard]] constexpr NavigationDynamicRegistryRevision Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns the number of retained staged commands. */
        [[nodiscard]] constexpr std::size_t PendingCommandCount() const noexcept {
            return pendingCount_;
        }

    private:
        explicit NavigationDynamicRegistry(std::unique_ptr<Detail::NavigationDynamicRegistryState> state) noexcept;
        std::unique_ptr<Detail::NavigationDynamicRegistryState> state_;
        NavigationDynamicRegistryRevision revision_;
        std::size_t pendingCount_{};
        bool shutdown_{};
    };
}  // namespace Horo::Navigation
