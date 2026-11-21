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

    class PerceptionSenseGeneration;

    /** @brief A detached query input authorized by the exact live Perception owners. */
    struct PerceptionQueryInput final {
        Runtime::RuntimeSceneView scene;
        AgentHandle agent;
        std::uint64_t spatialRevision{};
        PerceptionMemorySnapshot memory;
        std::shared_ptr<const BlackboardValueAuthorization> authorization;
        std::weak_ptr<const PerceptionSenseGeneration> provider;
    };

    /** @brief Paired writable collections; the smaller admitted key capacity bounds the atomic projection. */
    struct PerceptionBlackboardProjection final {
        BlackboardKeyId sources;   /**< EntityReference collection; exact admitted identities. */
        BlackboardKeyId positions; /**< WorldCoordinate collection; last-known memory facts only. */

        /**
         * @brief Validates the declared projection against the controller schema before sensing begins.
         * @param schema Captured blackboard schema for the recipient.
         * @return The shared item limit or a typed key, access or type failure.
         */
        [[nodiscard]] Result<std::size_t> Validate(const BlackboardSchema &schema) const;
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
        PerceptionPublication(const PerceptionPublication &) = delete;
        PerceptionPublication &operator=(const PerceptionPublication &) = delete;
        PerceptionPublication(PerceptionPublication &&) noexcept = default;
        PerceptionPublication &operator=(PerceptionPublication &&) = delete;

        /** @brief Admits one listener and its typed projection keys before sensing.
         * @param scene Current destination Scene view. @param listener Exact live recipient entity.
         * @param blackboard Recipient's admitted blackboard. @param projection Distinct, writable typed collections.
         * @return An owner-lane publisher for this binding, or a typed Scene, key, access or schema error.
         * @details No mutable Scene or blackboard borrow is retained. Schema or instance replacement requires readmission.
         */
        [[nodiscard]] static Result<PerceptionPublication> PrepareAtSafePoint(const Runtime::RuntimeSceneView &scene,
                                                                              Runtime::EntityRef listener,
                                                                              const BlackboardInstance &blackboard,
                                                                              PerceptionBlackboardProjection projection);

        /** @brief Filters the complete memory before publishing one blackboard batch and matching debug values.
         * @param scene Current Scene owner, borrowed synchronously. @param filtering Latest committed filter owner.
         * @param memory Exact agent memory. @param blackboard The same active blackboard admitted during preparation.
         * @param simulationTick Monotonic completed tick. Repeated identical tick/filter publication is idempotent.
         * @return One revisioned complete result or typed failure; no partial blackboard or debug publication.
         */
        [[nodiscard]] Result<PerceptionPublicationSnapshot> PublishAtBlackboardSync(Runtime::RuntimeScene &scene,
                                                                                    const PerceptionSpatialBroadphase &filtering,
                                                                                    AIPerceptionMemory &memory,
                                                                                    BlackboardInstance &blackboard,
                                                                                    std::uint64_t simulationTick);
        /** @brief Reads debug values only against their current filter/Scene publication.
         * @param scene Fresh Scene view. @param filtering Current filter owner. @param blackboard Current recipient blackboard.
         * @return Current value copy or typed stale failure after policy replacement, despawn or teardown.
         */
        [[nodiscard]] Result<PerceptionPublicationSnapshot> Current(const Runtime::RuntimeSceneView &scene,
                                                                    const PerceptionSpatialBroadphase &filtering,
                                                                    const BlackboardInstance &blackboard) const;

        /** @brief Returns the number of policy-admitted facts in the last capacity-rejected batch.
         * @return Zero before a rejection or after a successful replacement or reset. Read on the owner lane.
         */
        [[nodiscard]] std::size_t RejectedByCapacity() const noexcept {
            return rejectedByCapacity_;
        }

        /** @brief Revokes stored debug publication at disable, replacement or unload. */
        void Reset() noexcept;

    private:
        /** @brief Retains only immutable admission evidence and the recipient's exact identity. */
        PerceptionPublication(const Runtime::RuntimeSceneView &scene, Runtime::EntityRef listener, BlackboardInstanceBinding binding,
                              std::shared_ptr<const BlackboardSchema> schema, PerceptionBlackboardProjection projection,
                              std::size_t capacity) noexcept;
        /** @brief Checks the exact Scene recipient and the admitted blackboard generation and schema. */
        [[nodiscard]] bool MatchesBinding(const Runtime::RuntimeSceneView &scene, const BlackboardInstance &blackboard) const;
        Runtime::RuntimeSceneView scene_;
        Runtime::EntityRef listener_;
        BlackboardInstanceBinding binding_;
        std::shared_ptr<const BlackboardSchema> schema_;
        PerceptionBlackboardProjection projection_;
        std::size_t capacity_;
        std::optional<PerceptionPublicationSnapshot> current_;
        std::shared_ptr<const PerceptionSpatialSnapshot> filtering_;
        std::size_t rejectedByCapacity_{};
    };
}  // namespace Horo::AI
