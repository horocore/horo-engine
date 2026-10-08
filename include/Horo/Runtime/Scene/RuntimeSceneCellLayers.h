#pragma once

/** @file RuntimeSceneCellLayers.h
 * @brief Single-baseline cell data-layer membership, state filtering and fenced Scene publication.
 */
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"
#include "Horo/WorldStreaming/WorldLayerFiltering.h"
#include "Horo/WorldStreaming/WorldLayerState.h"

namespace Horo::Runtime {
    /** @brief Durable layer reference; runtime owner and state generations never enter cooked membership. */
    struct SceneCellDataLayer final {
        WorldStreaming::StreamingLayerId layer{};               /**< Unchanged manifest identity, including layer zero. */
        WorldStreaming::WorldLayerRevision ownershipRevision{}; /**< Exact classification publication used by cook. */
        WorldStreaming::WorldLayerFlags flags{};                /**< Exact manifest target flags. */
        [[nodiscard]] constexpr auto operator<=>(const SceneCellDataLayer &) const noexcept = default;
    };

    /** @brief One compact membership edge; an entity may belong to several layers without repeating components. */
    struct SceneCellLayerMembership final {
        SceneObjectId object{};                   /**< Stable baseline entity identity. */
        WorldStreaming::StreamingLayerId layer{}; /**< Stable declared data layer. */
        [[nodiscard]] constexpr auto operator<=>(const SceneCellLayerMembership &) const noexcept = default;
    };

    /** @brief Positive cook/load-time ceilings applied before metadata allocation. */
    struct SceneCellLayerLimits final {
        std::size_t maximumLayers{};
        std::size_t maximumMemberships{};
        std::size_t maximumRetainedBytes{}; /**< Baseline logical bytes plus compact metadata, with checked arithmetic. */
    };

    /** @brief Owns exactly one complete baseline and canonical membership metadata; never owns per-layer object copies. */
    class RuntimeSceneCellLayers final {
    public:
        RuntimeSceneCellLayers(const RuntimeSceneCellLayers &) = delete;
        RuntimeSceneCellLayers &operator=(const RuntimeSceneCellLayers &) = delete;
        RuntimeSceneCellLayers &operator=(RuntimeSceneCellLayers &&) = delete;
        /** @brief Transfers the sole baseline; the source becomes unusable for filtering. @param other Current owner. */
        RuntimeSceneCellLayers(RuntimeSceneCellLayers &&other) noexcept;
        /** @brief Returns the sole baseline. @return Borrowed complete immutable payload. */
        [[nodiscard]] const RuntimeSceneCellPayload &Baseline() const noexcept;
        /** @brief Returns canonical durable layer references. @return Borrowed span valid until move or destruction. */
        [[nodiscard]] std::span<const SceneCellDataLayer> Layers() const noexcept;
        /** @brief Returns canonical object/layer edges. @return Borrowed span valid until move or destruction. */
        [[nodiscard]] std::span<const SceneCellLayerMembership> Memberships() const noexcept;
        /** @brief Returns checked logical retained storage. @return Baseline plus metadata bytes. */
        [[nodiscard]] std::size_t RetainedBytes() const noexcept;
        /** @brief Reports whether this value still owns its baseline. @return False after moving ownership. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend Result<RuntimeSceneCellLayers> EncodeRuntimeSceneCellLayers(const WorldStreaming::WorldPartitionDescriptor &,
                                                                           RuntimeSceneCellPayload &&, const SceneCellPayloadIdentity &,
                                                                           std::span<const SceneCellDataLayer>,
                                                                           std::span<const SceneCellLayerMembership>, SceneCellLayerLimits,
                                                                           const CancellationToken &);
        /** @brief Takes validated metadata and transfers the complete baseline once. */
        RuntimeSceneCellLayers(RuntimeSceneCellPayload &&baseline, std::vector<SceneCellDataLayer> layers,
                               std::vector<SceneCellLayerMembership> memberships, std::size_t retainedBytes) noexcept;
        RuntimeSceneCellPayload baseline_;
        std::vector<SceneCellDataLayer> layers_;
        std::vector<SceneCellLayerMembership> memberships_;
        std::size_t retainedBytes_{};
        bool usable_{true};
    };

    /**
     * @brief Encodes canonical data-layer edges around a single already cooked cell baseline.
     * @param partition Exact immutable topology containing baseline cell and every layer.
     * @param baseline Complete owned baseline; consumed only on success, unchanged on any failure.
     * @param expected Exact source publication captured for membership cook.
     * @param layers Strictly ascending unique durable layer references with exact manifest flags.
     * @param memberships Strictly ascending unique object/layer pairs referring only to baseline objects and supplied layers.
     * @param limits Mandatory count and complete retained-storage ceilings.
     * @param cancellation Checked between validation/copy units and before ownership transfer.
     * @return Complete immutable encoding or typed invalid, stale, unsupported, capacity or cancellation failure.
     * @details No memberships means unconditional content. Multiple memberships use union semantics at filtering.
     * This is the Scene provider's in-memory encoding, not an ADR-023 wire schema. No runtime states are cooked.
     * @throws std::bad_alloc on bounded allocation failure; baseline remains unconsumed.
     */
    [[nodiscard]] Result<RuntimeSceneCellLayers> EncodeRuntimeSceneCellLayers(
        const WorldStreaming::WorldPartitionDescriptor &partition, RuntimeSceneCellPayload &&baseline,
        const SceneCellPayloadIdentity &expected, std::span<const SceneCellDataLayer> layers,
        std::span<const SceneCellLayerMembership> memberships, SceneCellLayerLimits limits, const CancellationToken &cancellation = {});

    /** @brief Owned submission evidence revalidated by the explicit host authority immediately before Scene publication. */
    struct SceneCellLayerSelectionEvidence final {
        SceneCellPayloadIdentity identity{};
        WorldStreaming::StreamingRuntimeOwnerToken world{};
        WorldStreaming::WorldLayerFilterPolicy policy{};
        std::vector<WorldStreaming::WorldLayerStateFence> states; /**< Every encoded layer, including excluded layers. */
    };

    /** @brief Source-free selected Scene definition with exact captured target and layer-state fences. */
    class RuntimeSceneCellLayerSelection final {
    public:
        RuntimeSceneCellLayerSelection(const RuntimeSceneCellLayerSelection &) = default;
        RuntimeSceneCellLayerSelection &operator=(const RuntimeSceneCellLayerSelection &) = delete;
        RuntimeSceneCellLayerSelection &operator=(RuntimeSceneCellLayerSelection &&) = delete;
        /** @brief Transfers complete submission facts; the source cannot be queued. @param other Current selection owner. */
        RuntimeSceneCellLayerSelection(RuntimeSceneCellLayerSelection &&other) noexcept;
        /** @brief Reports complete retained submission ownership. @return False after moving this selection. */
        [[nodiscard]] bool IsUsable() const noexcept;
        /** @brief Returns the selected immutable definition. @return Borrowed definition in original authored order. */
        [[nodiscard]] const RuntimeSceneDefinition &Definition() const noexcept;
        /** @brief Returns owned policy/state evidence. @return Borrowed exact publication facts. */
        [[nodiscard]] const SceneCellLayerSelectionEvidence &Evidence() const noexcept;

    private:
        friend Result<RuntimeSceneCellLayerSelection> FilterRuntimeSceneCellLayers(
            const RuntimeSceneCellLayers &, const WorldStreaming::WorldLayerFilterPolicy &,
            std::span<const WorldStreaming::WorldLayerFilterCandidate>, std::span<const WorldStreaming::WorldLayerStateRecord>,
            const WorldStreaming::WorldLayerFilterContext &, const CancellationToken &);
        /** @brief Owns complete validated selection and exact state fences. */
        RuntimeSceneCellLayerSelection(RuntimeSceneDefinition definition, SceneCellLayerSelectionEvidence evidence) noexcept;
        RuntimeSceneDefinition definition_;
        SceneCellLayerSelectionEvidence evidence_;
        bool usable_{true};
    };

    /**
     * @brief Materializes one bounded detached definition from target-included Activated layer membership.
     * @param payload Complete encoded baseline and metadata.
     * @param policy Exact target policy.
     * @param candidates Canonical complete encoded-layer ownership/flag snapshot.
     * @param states Same-order complete layer states at the candidates' exact ownership publications.
     * @param context Current mounted world, expected policy/revision, count ceiling and lifecycle gate.
     * @param cancellation Checked between entity units and before result publication.
     * @return Complete source-free definition or typed filtering/Scene/cancellation error with no partial output.
     * @details Unconditional entities always survive. A member survives once if any included layer is Activated.
     * Loaded, transitional, rollback and Failed states do not activate entities. Hierarchy/constraint validation rejects
     * selections that remove required references. The complete baseline asset requirements remain conservative;
     * filtering never guesses opaque gameplay dependencies. Work is load-time, bounded by admitted entities/edges/layers.
     * @throws std::bad_alloc on bounded preparation allocation failure; input remains unchanged.
     */
    [[nodiscard]] Result<RuntimeSceneCellLayerSelection> FilterRuntimeSceneCellLayers(
        const RuntimeSceneCellLayers &payload, const WorldStreaming::WorldLayerFilterPolicy &policy,
        std::span<const WorldStreaming::WorldLayerFilterCandidate> candidates,
        std::span<const WorldStreaming::WorldLayerStateRecord> states, const WorldStreaming::WorldLayerFilterContext &context,
        const CancellationToken &cancellation = {});

    /** @brief Explicit owner-thread publication authority; shared lease outlives all queued Scene work. */
    class SceneCellLayerAuthority {
    public:
        virtual ~SceneCellLayerAuthority() = default;
        /** @brief Checks current content, world/cell fence, complete policy/state evidence and required provider barrier.
         * @param evidence Exact immutable submission facts, including excluded layers.
         * @param fence Current residency attempt.
         * @return Success or typed stale, cancellation, closed or readiness rejection.
         * @details Pure bounded owner-thread predicate, called at admission and safe-point commit. */
        [[nodiscard]] virtual Result<void> ValidatePublication(const SceneCellLayerSelectionEvidence &evidence,
                                                               const WorldStreaming::StreamingFence &fence) const = 0;
    };

    /**
     * @brief Queues selected entities through existing detached RuntimeScene preparation and transactional replacement.
     * @param service Scene owner for this cell domain.
     * @param selection Complete selected definition/evidence, copied before return.
     * @param fence Exact partition epoch and cell attempt matching selection.
     * @param authority Non-null shared host authority revalidating all captured revisions and readiness.
     * @param cancellation Retained observer checked at admission and commit.
     * @return Admission result; deferred failure is returned through service.TakeOperationError().
     * @details State/policy/content replacement before commit rejects stale work and preserves active Scene.
     * Scene owns asset failure, cancellation, unload and shutdown; this contract owns no competing state machine.
     */
    [[nodiscard]] Result<void> QueueRuntimeSceneCellLayers(RuntimeSceneService &service, const RuntimeSceneCellLayerSelection &selection,
                                                           const WorldStreaming::StreamingFence &fence,
                                                           std::shared_ptr<const SceneCellLayerAuthority> authority,
                                                           const CancellationToken &cancellation = {});
}  // namespace Horo::Runtime
