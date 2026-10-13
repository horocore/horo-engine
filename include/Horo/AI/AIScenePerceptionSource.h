#pragma once

/**
 * @file AIScenePerceptionSource.h
 * @brief RuntimeScene conversion and residency seam for dependency-neutral perception memory.
 */

#include "Horo/AI/BlackboardInstance.h"
#include "Horo/AI/PerceptionMemory.h"
#include "Horo/AI/PerceptionSpatialBroadphase.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::AI {
    /**
     * @brief Projects one generation-checked runtime entity into AI memory's weak source representation.
     * @param entity Exact active-scene entity reference.
     * @return Dependency-neutral value projection; invalid inputs remain invalid.
     */
    [[nodiscard]] PerceptionSourceRef ProjectPerceptionSource(Runtime::EntityRef entity) noexcept;

    /**
     * @brief Borrows an active scene for synchronous source-validating memory queries.
     * @param scene Scene that owns the memory and entity slots; it must outlive each query.
     * @return Callback that reacquires the current scene view and validates incarnation, slot, and generation.
     */
    [[nodiscard]] PerceptionSourceLiveness PerceptionSceneLiveness(Runtime::RuntimeScene &scene) noexcept;

    /** @brief Both keys are required writable collections with capacity for the admitted memory profile (at most
     * MaximumBlackboardCollectionElements). */
    struct PerceptionBlackboardProjection final {
        BlackboardKeyId sources;   /**< EntityReference collection; exact admitted identities. */
        BlackboardKeyId positions; /**< WorldCoordinate collection; last-known memory facts only. */
    };

    /** @brief One complete filtered value publication shared by blackboard and debug readers. */
    struct PerceptionPublicationSnapshot final {
        AgentHandle agent;
        Runtime::EntityRef listener;
        std::uint64_t revision{};
        std::uint64_t spatialRevision{};
        std::uint64_t simulationTick{};
        std::uint64_t blackboardRevision{};
        PerceptionMemorySnapshot memory;
    };

    /** @brief Owner-thread publisher; one bounded memory/filter/blackboard transaction at BlackboardSync.
     * @details Call after every sense has staged observations. Policy replacement revokes Current immediately;
     * the next safe-point commit replaces both blackboard collections and the debug snapshot once. Retained
     * value copies are historical evidence, never current permission. Owners close sensing windows before policy commit.
     */
    class PerceptionPublication final {
    public:
        /** @brief Filters the complete memory before publishing one blackboard batch and matching debug values.
         * @param scene Current Scene owner, borrowed synchronously. @param filtering Latest committed filter owner.
         * @param listener Exact recipient listener. @param memory Exact agent memory.
         * @param blackboard Same agent's active blackboard. @param projection Distinct typed collection keys.
         * @param simulationTick Monotonic completed tick. Repeated identical tick/filter publication is idempotent.
         * @return One revisioned complete result or typed failure; no partial blackboard or debug publication.
         */
        [[nodiscard]] Result<PerceptionPublicationSnapshot> PublishAtBlackboardSync(Runtime::RuntimeScene &scene,
                                                                                    const PerceptionSpatialBroadphase &filtering,
                                                                                    Runtime::EntityRef listener, AIPerceptionMemory &memory,
                                                                                    BlackboardInstance &blackboard,
                                                                                    PerceptionBlackboardProjection projection,
                                                                                    std::uint64_t simulationTick);
        /** @brief Reads debug values only against their current filter/Scene publication.
         * @param scene Fresh Scene view. @param filtering Current filter owner. @param blackboard Current recipient blackboard.
         * @return Current value copy or typed stale failure after policy replacement, despawn or teardown.
         */
        [[nodiscard]] Result<PerceptionPublicationSnapshot> Current(const Runtime::RuntimeSceneView &scene,
                                                                    const PerceptionSpatialBroadphase &filtering,
                                                                    const BlackboardInstance &blackboard) const;
        /** @brief Revokes stored debug publication at disable, replacement or unload. */
        void Reset() noexcept;

    private:
        std::optional<PerceptionPublicationSnapshot> current_;
        std::shared_ptr<const PerceptionSpatialSnapshot> filtering_;
        BlackboardInstanceBinding binding_;
        PerceptionBlackboardProjection projection_;
    };
}  // namespace Horo::AI
