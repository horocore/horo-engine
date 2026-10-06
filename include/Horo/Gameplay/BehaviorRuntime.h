#pragma once

/**
 * @file BehaviorRuntime.h
 * @brief Deterministic scene-scoped behavior lifecycle, input, event, and deferred mutation runner.
 */

#include "Horo/Gameplay/BehaviorRegistry.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace Horo::Gameplay {
    /** @brief Runtime-only state captured for one stable behavior attachment. */
    struct BehaviorInstanceReloadState {
        BehaviorInstanceId instanceId;
        BehaviorTypeId typeId;
        std::vector<std::byte> payload;
        bool started{};
    };

    /** @brief Complete bounded behavior snapshot used by one native reload transaction. */
    struct BehaviorRuntimeReloadSnapshot {
        std::vector<BehaviorInstanceReloadState> instances;
    };

    /** @brief Explicit per-scene bounds for behavior instances and deferred custom events. */
    struct BehaviorRuntimeLimits {
        std::size_t maximumInstances{16'384};
        std::size_t maximumQueuedEvents{4'096};
    };

    /** @brief Owns every behavior instance attached to one active runtime scene. */
    class BehaviorRuntime final {
    public:
        /**
         * @brief Constructs and activates all known scene behavior instances.
         * @param scene Runtime scene that outlives this runner.
         * @param registry Frozen registry that outlives this runner and its module factories.
         * @param limits Explicit admission budgets.
         * @return Active runner or a typed validation/factory error without partial lifetime leakage.
         */
        [[nodiscard]] static Result<std::unique_ptr<BehaviorRuntime>> Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                             BehaviorRuntimeLimits limits = {});
        /** @brief Activates callbacks with an explicitly permitted Physics context for this exact scene.
         * @param scene Runtime scene that outlives the runner.
         * @param registry Frozen module registry.
         * @param limits Admission budgets.
         * @param physics Host-admitted module/world binding; revoked before shutdown callbacks.
         * @return Active runner or typed foreign-scene/activation error without partial clients.
         */
        [[nodiscard]] static Result<std::unique_ptr<BehaviorRuntime>> Create(Runtime::RuntimeScene &scene, const BehaviorRegistry &registry,
                                                                             BehaviorRuntimeLimits limits,
                                                                             std::shared_ptr<const GameplayPhysicsContext> physics);
        ~BehaviorRuntime();
        BehaviorRuntime(const BehaviorRuntime &) = delete;
        BehaviorRuntime &operator=(const BehaviorRuntime &) = delete;

        /** @brief Delivers queued events/input, runs one deterministic tick, and commits mutations.
         * @param input Current tick input. @param delta Validated fixed step. @return Success or a lifecycle/callback error. */
        [[nodiscard]] Result<void> FixedUpdate(std::span<const GameplayInputAction> input, FixedDeltaTime delta) const;
        /** @brief Runs presentation-only callbacks without committing simulation mutation.
         * @param delta Current presentation frame delta. */
        void PresentationUpdate(FrameDeltaTime delta) const;
        /** @brief Enables or disables one attachment with exact lifecycle transitions.
         * @param instance Exact attachment identity. @param enabled Desired enablement. @return Success or a typed failure. */
        [[nodiscard]] Result<void> SetEnabled(BehaviorInstanceId instance, bool enabled) const;
        /** @brief Runs disable/destroy and releases every module-owned instance exactly once. */
        void Shutdown() const noexcept;
        /**
         * @brief Captures runtime-only state for every instance before shutdown.
         * @return Complete bounded snapshot or a typed failure that leaves this runtime active.
         */
        [[nodiscard]] Result<BehaviorRuntimeReloadSnapshot> CaptureReloadSnapshot() const;
        /**
         * @brief Restores an exact snapshot into newly created compatible instances.
         * @param snapshot State matched by stable behavior instance and type identity.
         * @return Success or a typed mismatch/restore failure.
         */
        [[nodiscard]] Result<void> RestoreReloadSnapshot(const BehaviorRuntimeReloadSnapshot &snapshot) const;
        /** @brief Reports the number of constructed scene-scoped instances. */
        [[nodiscard]] std::size_t InstanceCount() const noexcept;
        /**
         * @brief Creates the Gameplay owner for Scene's aggregate group transaction.
         * @details Preparation validates and reserves attachment metadata without calling factories.
         * Existing instances are preserved. Construction and disable/destroy notifications run only
         * after aggregate publication; a callback fault does not roll back committed entities.
         * The runner and its frozen registry must outlive the participant and all its candidates.
         * All preparation, publication, callbacks and destruction use the runner's owner lane.
         * @return Application-owned participant, explicitly registered before Scene service startup.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant();

    private:
        struct Impl;
        explicit BehaviorRuntime(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> impl_;
    };
}  // namespace Horo::Gameplay
